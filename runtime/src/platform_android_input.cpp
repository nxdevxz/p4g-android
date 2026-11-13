#include <cstdlib>
// Android host (NativeActivity): memory (mmap at fixed addresses), pthreads, the app's window (ANativeWindow),
// game controllers + touch, AAudio, the Vulkan loader and android surface, a signal crash handler and
// android_main. Game data lives in the app's external files dir (see docs/ANDROID.md):
//   <files>/analysis/{main,sdk}.bin   module images (tools/nso2bin.py)
//   <files>/extract/romfs/...         the RomFS
//   <files>/user/...                  saves
//   <files>/config.txt                optional: resolution=WxH
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"
#include "platform_android.h"
#include "input_stick.h"
#include "touch_layout.h"

// Android input: game controllers and touch (split from platform_android.cpp).
namespace nx::plat {

// ---- input: game controllers + touch --------------------------------------------------------------------------
// Touch: left virtual stick; P4G can keep its base fixed, while floating/legacy layouts are relative to touch-down. The
// right half has four buttons in a
// diamond around the thumb (Y attack up-left, X special up-right, B dash down-left, A interact down-right), top
// corners L / R, top centre +. Games with a drawn layout (touch_layout.cpp) use its buttons instead, and may make the
// rest of the right half a second stick (P5R: the camera).

namespace {
std::mutex g_in;
PadState g_pad_buttons;  // controller state
enum Stick : uint8_t { kNoStick, kLeftStick, kRightStick };
struct Touch {
    int32_t id;
    float x0, y0, x, y;
    Stick stick;
    uint64_t button;
    uint64_t down_ns;
};
std::vector<Touch> g_touches;
// A quick tap still reaches the game: a button let go sooner than kMinHoldNs after it went down stays held until
// then (some P5R menus missed taps of 100-200 ms that the game had read on several frames).
struct Latch {
    uint64_t button, until_ns;
};
std::vector<Latch> g_latched;
constexpr uint64_t kMinHoldNs = 150'000'000;

// P4G consumes button edges. Replay short taps as distinct HID samples even
// if both Android events arrived between two reads.
std::array<uint8_t, 64> g_pending_edges{};
uint64_t g_sampled_buttons = 0;
uint64_t g_next_button_sample = 0;

bool p4g_input() {
    static const bool enabled = [] {
        const bool p4g = std::strcmp(nx::game::kInfo.id, "p4g") == 0;
        if (p4g) nx::log("hid: P4G radial left-stick mapping enabled (outer radius 0.98)");
        return p4g;
    }();
    return enabled;
}

float gamepad_stick_full() {
    static const float full = [] {
        const char* e = std::getenv("NX_STICK_FULL");
        char* end = nullptr;
        const float v = e ? std::strtof(e, &end) : 0.80f;
        if (e && (end == e || *end || !std::isfinite(v))) return 0.80f;
        return v == 0 ? 0.0f : std::clamp(v, 0.60f, 1.0f);
    }();
    return full;
}

bool gamepad_run_fix() {
    static const bool on = [] {
        if (std::strcmp(nx::game::kInfo.id, "p4g") != 0) return false;
        const char* e = std::getenv("NX_P4G_STICK_FIX");
        const bool enabled = (!e || std::strcmp(e, "0") != 0) && gamepad_stick_full() > 0;
        nx::log("p4g: gamepad left-stick run-fix-v4 %s (deadzone 0.10, full %.3f, release 0.55, hold 100 ms; neutral stops)",
                enabled ? "enabled" : "disabled", double(gamepad_stick_full()));
        return enabled;
    }();
    return on;
}

GamepadRunStick g_gamepad_run;
int32_t g_run_device = -1;
bool g_run_seen = false;
size_t g_run_history = 0;
thread_local const char* g_left_read_origin = "none";

void sample_gamepad_left(uint64_t now) {
    if (!gamepad_run_fix() || !g_run_seen) return;
    const StickPosition left = g_gamepad_run.sample(now);
    g_pad_buttons.lx = left.x, g_pad_buttons.ly = left.y;
}

// Temporary analog diagnosis: one aggregate per second, at most 300 lines per process. All access is under g_in.
// Record Android input separately from the merged pad, so a low physical sample, a second input device, and a
// touch/script override can be distinguished. These observers never change movement or discard input events.
bool analog_trace_enabled() {
    static const bool on = [] {
        const char* aggregate = std::getenv("NX_P4G_ANALOG_TRACE");
        const char* detail = std::getenv("NX_P4G_STICK_TRACE");
        const char* verbose = std::getenv("NX_VERBOSE_LOG");
        return (aggregate && aggregate[0] == '1') || (detail && detail[0] == '1') ||
               (verbose && verbose[0] == '1');
    }();
    return on;
}

struct AnalogTrace {
    uint64_t next_ns = 0, last_event_ns = 0, sequence = 0;
    unsigned lines = 0, events = 0, switches = 0, zeros = 0, reads = 0, touch_reads = 0;
    int32_t device = -1, source = 0, action = 0, min_device = -1;
    float x = 0, y = 0, min_x = 0, min_y = 0;
    double raw_min = 1e30, raw_max = 0, merged_min = 1e30, merged_max = 0;
    int32_t min_lx = 0, min_ly = 0;
    const char* min_origin = "none";
    bool seen = false;
} g_analog_trace;

void trace_controller(int32_t device, int32_t source, int32_t action, float x, float y, uint64_t now) {
    auto& d = g_analog_trace;
    if (d.lines >= 300) return;
    if (!d.seen) nx::log("hid: analog-trace-v2 enabled (1 Hz aggregates; max 300 lines; mapping unchanged)");
    if (d.last_event_ns && d.device != device) ++d.switches;
    d.seen = true;
    d.device = device, d.source = source, d.action = action;
    d.x = x, d.y = y, d.last_event_ns = now;
    ++d.sequence, ++d.events;
    const double radius = std::hypot(double(x), double(y));
    if (radius < d.raw_min) d.raw_min = radius, d.min_x = x, d.min_y = y, d.min_device = device;
    d.raw_max = std::max(d.raw_max, radius);
    if (radius <= 0.12) ++d.zeros;
}

void trace_merged(const PadState& p, const char* origin, bool touch_left, uint64_t now) {
    auto& d = g_analog_trace;
    if (d.lines >= 300) return;
    if (!d.seen && touch_left) {
        nx::log("hid: analog-trace-v2 enabled (1 Hz aggregates; max 300 lines; mapping unchanged)");
        d.seen = true;
    }
    if (!d.seen) return;
    // Do not spend the trace budget while sitting in a menu with no movement.
    const bool recent = d.last_event_ns && now - d.last_event_ns < 5'000'000'000ull;
    if (!recent && !touch_left && !p.lx && !p.ly) { d.next_ns = 0; return; }
    if (!d.next_ns) d.next_ns = now + 1'000'000'000ull;
    ++d.reads;
    if (touch_left) ++d.touch_reads;
    const double radius = std::hypot(double(p.lx), double(p.ly)) / kStickMax;
    if (radius < d.merged_min) {
        d.merged_min = radius, d.min_lx = p.lx, d.min_ly = p.ly, d.min_origin = origin;
    }
    d.merged_max = std::max(d.merged_max, radius);
    if (now < d.next_ns) return;
    nx::log("hid: analog-trace-v2 t_ms=%llu seq=%llu events=%u switches=%u dev=%d src=%#x action=%d "
            "age_ms=%llu raw=(%.4f,%.4f) raw_range=%.4f..%.4f raw_min=(%.4f,%.4f,dev=%d) zeros=%u "
            "mapped=(%d,%d) merged=(%d,%d,%s) merged_range=%.4f..%.4f merged_min=(%d,%d,%s) "
            "reads=%u touch_reads=%u run_fix=%u running=%u batch_peak=%.4f history=%zu",
            (unsigned long long)(now / 1'000'000), (unsigned long long)d.sequence, d.events, d.switches,
            int(d.device), unsigned(d.source), int(d.action),
            (unsigned long long)(d.last_event_ns ? (now - d.last_event_ns) / 1'000'000 : 0),
            double(d.x), double(d.y), d.events ? d.raw_min : -1.0, d.events ? d.raw_max : -1.0,
            double(d.min_x), double(d.min_y), int(d.min_device), d.zeros,
            int(g_pad_buttons.lx), int(g_pad_buttons.ly), int(p.lx), int(p.ly), origin,
            d.merged_min, d.merged_max, int(d.min_lx), int(d.min_ly), d.min_origin, d.reads, d.touch_reads,
            unsigned(gamepad_run_fix()), unsigned(g_gamepad_run.running()), g_gamepad_run.batch_peak(), g_run_history);
    ++d.lines;
    d.next_ns = now + 1'000'000'000ull;
    d.events = d.switches = d.zeros = d.reads = d.touch_reads = 0;
    d.raw_min = d.merged_min = 1e30;
    d.raw_max = d.merged_max = 0;
}

uint64_t held_touch_buttons() {
    uint64_t held = 0;
    for (const auto& t : g_touches) held |= t.button;
    return held;
}

void queue_edges(uint64_t changed) {
    while (changed) {
        auto& pending = g_pending_edges[std::countr_zero(changed)];
        // Bound the backlog. Dropping a press/release pair keeps the final state.
        if (pending >= 8) --pending;
        else ++pending;
        changed &= changed - 1;
    }
}

void sample_touch_buttons(uint64_t now) {
    if (now < g_next_button_sample) return;
    uint64_t changed = 0;
    for (size_t bit = 0; bit < g_pending_edges.size(); ++bit) {
        auto& pending = g_pending_edges[bit];
        if (!pending) continue;
        --pending;
        changed |= uint64_t{1} << bit;
    }
    if (changed) {
        g_sampled_buttons ^= changed;
        // Keep each state visible to a game frame even if HID is polled more often.
        g_next_button_sample = now + 33'333'333;
    }
}

void reset_touch() {
    g_touches.clear();
    g_latched.clear();
    g_pending_edges.fill(0);
    g_sampled_buttons = 0;
    g_next_button_sample = 0;
}

void release_touch(const Touch& t, uint64_t now) {
    if (t.button && now < t.down_ns + kMinHoldNs) g_latched.push_back({t.button, t.down_ns + kMinHoldNs});
}
float g_w = 1, g_h = 1;
bool g_gamepad = false;  // the last input came from a controller: the drawn touch controls hide
uint64_t g_last_touch_ns = 0;  // the drawn controls dim after kTouchIdleNs without a touch
constexpr uint64_t kTouchIdleNs = 8'000'000'000;

bool gamepad_swap_abxy() {
    static const bool swap = [] {
        if (std::strcmp(nx::game::kInfo.id, "p4g") == 0) return true;
        const char* e = std::getenv("NX_GAMEPAD_LAYOUT");
        return e && std::strcmp(e, "1") == 0;
    }();
    return swap;
}

float stick_sens() {
    static const float s = [] {
        const char* e = std::getenv("NX_STICK_SENSITIVITY");
        int v = e ? std::atoi(e) : 100;
        if (v < 25) v = 25;
        if (v > 300) v = 300;
        return float(v) / 100.0f;
    }();
    return s;
}

float camera_sens() {
    static const float s = [] {
        const char* e = std::getenv("NX_CAMERA_SENSITIVITY");
        int v = e ? std::atoi(e) : 100;
        if (v < 25) v = 25;
        if (v > 300) v = 300;
        return float(v) / 100.0f;
    }();
    return s;
}

uint64_t key_to_button(int32_t k) {
    if (gamepad_swap_abxy()) {
        switch (k) {
            case AKEYCODE_BUTTON_A:
            case AKEYCODE_BUTTON_3: return kA;
            case AKEYCODE_BUTTON_B:
            case AKEYCODE_BUTTON_2: return kB;
            case AKEYCODE_BUTTON_X:
            case AKEYCODE_BUTTON_4: return kX;
            case AKEYCODE_BUTTON_Y:
            case AKEYCODE_BUTTON_1: return kY;
            default: break;
        }
    } else {
        switch (k) {
            case AKEYCODE_BUTTON_A:
            case AKEYCODE_BUTTON_3: return kB;  // bottom face button = Nintendo B
            case AKEYCODE_BUTTON_B:
            case AKEYCODE_BUTTON_2: return kA;  // right face button = Nintendo A
            case AKEYCODE_BUTTON_X:
            case AKEYCODE_BUTTON_4: return kY;
            case AKEYCODE_BUTTON_Y:
            case AKEYCODE_BUTTON_1: return kX;
            default: break;
        }
    }
    switch (k) {
        case AKEYCODE_BUTTON_L1:
        case AKEYCODE_BUTTON_7: return kL;
        case AKEYCODE_BUTTON_R1:
        case AKEYCODE_BUTTON_8: return kR;
        case AKEYCODE_BUTTON_L2:
        case AKEYCODE_BUTTON_5:
        case AKEYCODE_BUTTON_C: return kZL;
        case AKEYCODE_BUTTON_R2:
        case AKEYCODE_BUTTON_6:
        case AKEYCODE_BUTTON_Z: return kZR;
        case AKEYCODE_BUTTON_START:
        case AKEYCODE_BUTTON_10:
        case AKEYCODE_BUTTON_MODE: return kPlus;
        case AKEYCODE_BUTTON_SELECT:
        case AKEYCODE_BUTTON_9: return kMinus;
        case AKEYCODE_BUTTON_THUMBL:
        case AKEYCODE_BUTTON_11: return kStickL;
        case AKEYCODE_BUTTON_THUMBR:
        case AKEYCODE_BUTTON_12: return kStickR;
        case AKEYCODE_DPAD_LEFT: return kLeft;
        case AKEYCODE_DPAD_UP: return kUp;
        case AKEYCODE_DPAD_RIGHT: return kRight;
        case AKEYCODE_DPAD_DOWN: return kDown;
        case AKEYCODE_BACK: return kMinus;
        // A keyboard: Enter / Space confirm (A), Escape goes back (B). (Some tablets keep BUTTON_B for their stylus.)
        case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: case AKEYCODE_SPACE: case AKEYCODE_DPAD_CENTER: return kA;
        case AKEYCODE_ESCAPE: return kB;
        default: return 0;
    }
}

uint64_t touch_button(float x, float y) {
    float nx_ = x / g_w, ny = y / g_h;
    if (ny < 0.2f) return nx_ < 0.6f ? (nx_ < 0.45f ? kL : kPlus) : (nx_ > 0.85f ? kR : kZR);
    bool up = ny < 0.6f, right = nx_ > 0.8f;
    if (up) return right ? kX : kY;
    return right ? kA : kB;
}

}  // namespace

void clear_touch_input() {
    std::lock_guard lock(g_in);
    reset_touch();
    if (gamepad_run_fix()) {
        g_gamepad_run.reset();
        g_run_device = -1;
        g_run_seen = false;
        g_run_history = 0;
        g_pad_buttons.lx = g_pad_buttons.ly = 0;
    }
}

int32_t on_input(android_app* app, AInputEvent* e) {
    std::lock_guard lk(g_in);
    int32_t type = AInputEvent_getType(e);
    int32_t src = AInputEvent_getSource(e);
    // NativeActivity can deliver phone Back straight to this input queue,
    // bypassing Java dispatchKeyEvent. Keep controller Back mapped to Minus.
    if (type == AINPUT_EVENT_TYPE_KEY && AKeyEvent_getKeyCode(e) == AKEYCODE_BACK &&
        (src & AINPUT_SOURCE_GAMEPAD) != AINPUT_SOURCE_GAMEPAD &&
        (src & AINPUT_SOURCE_JOYSTICK) != AINPUT_SOURCE_JOYSTICK) {
        JNIEnv* env = nullptr;
        bool attached = app->activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK;
        if (!attached && app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return 0;
        jclass activity = env->GetObjectClass(app->activity->clazz);
        jmethodID confirm = env->GetMethodID(activity, "confirmExit", "()V");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (confirm && AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) {
            env->CallVoidMethod(app->activity->clazz, confirm);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        env->DeleteLocalRef(activity);
        if (!attached) app->activity->vm->DetachCurrentThread();
        if (confirm) return 1;
    }
    if (type == AINPUT_EVENT_TYPE_KEY) {
        uint64_t b = key_to_button(AKeyEvent_getKeyCode(e));
        if (!b) return 0;
        g_gamepad = true;
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN)
            g_pad_buttons.buttons |= b;
        else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP)
            g_pad_buttons.buttons &= ~b;
        return 1;
    }
    if (type != AINPUT_EVENT_TYPE_MOTION) return 0;
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        g_gamepad = true;
        auto axis = [&](int32_t a) {
            float v = AMotionEvent_getAxisValue(e, a, 0);
            constexpr float kDeadzone = 0.12f;
            if (std::fabs(v) < kDeadzone) return 0;
            float sign = v > 0 ? 1.f : -1.f;
            float scaled = (v - sign * kDeadzone) / (1.f - kDeadzone);
            return int32_t(std::clamp(scaled, -1.f, 1.f) * kStickMax);
        };
        if (gamepad_run_fix()) {
            const uint64_t now = monotonic_ns();
            const int32_t device = AInputEvent_getDeviceId(e);
            if (!g_run_seen || device != g_run_device) g_gamepad_run.reset(), g_run_device = device;
            g_run_seen = true;
            const int32_t action = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
            if (action == AMOTION_EVENT_ACTION_MOVE) {
                const float x = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
                const float y = -AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
                float peak_x = x, peak_y = y;
                double peak = std::isfinite(x) && std::isfinite(y) ? std::hypot(double(x), double(y)) : 0;
                g_run_history = AMotionEvent_getHistorySize(e);
                for (size_t h = 0; h < g_run_history; ++h) {
                    const float hx = AMotionEvent_getHistoricalAxisValue(e, AMOTION_EVENT_AXIS_X, 0, h);
                    const float hy = -AMotionEvent_getHistoricalAxisValue(e, AMOTION_EVENT_AXIS_Y, 0, h);
                    if (!std::isfinite(hx) || !std::isfinite(hy)) continue;
                    const double r = std::hypot(double(hx), double(hy));
                    if (r > peak) peak = r, peak_x = hx, peak_y = hy;
                }
                g_gamepad_run.update(x, y, peak_x, peak_y, now, gamepad_stick_full());
            } else {
                g_gamepad_run.reset();
                g_run_history = 0;
            }
            sample_gamepad_left(now);
        } else if (p4g_input()) {
            const StickPosition left = radial_stick(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0),
                    -AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0), 0.12f, 0.98f);
            g_pad_buttons.lx = left.x;
            g_pad_buttons.ly = left.y;
        } else {
            g_pad_buttons.lx = axis(AMOTION_EVENT_AXIS_X);
            g_pad_buttons.ly = -axis(AMOTION_EVENT_AXIS_Y);
        }
        int32_t rx = axis(AMOTION_EVENT_AXIS_Z);
        if (!rx) rx = axis(AMOTION_EVENT_AXIS_RX);
        int32_t ry = -axis(AMOTION_EVENT_AXIS_RZ);
        if (!ry) ry = -axis(AMOTION_EVENT_AXIS_RY);
        g_pad_buttons.rx = rx;
        g_pad_buttons.ry = ry;
        float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        g_pad_buttons.buttons &= ~(kLeft | kRight | kUp | kDown | kZL | kZR);
        if (hx < -0.5f) g_pad_buttons.buttons |= kLeft;
        if (hx > 0.5f) g_pad_buttons.buttons |= kRight;
        if (hy < -0.5f) g_pad_buttons.buttons |= kUp;
        if (hy > 0.5f) g_pad_buttons.buttons |= kDown;
        if (AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0) > 0.4f ||
            AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0) > 0.4f)
            g_pad_buttons.buttons |= kZL;
        if (AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0) > 0.4f ||
            AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0) > 0.4f)
            g_pad_buttons.buttons |= kZR;
        if (p4g_input() && analog_trace_enabled()) trace_controller(AInputEvent_getDeviceId(e), src, AMotionEvent_getAction(e),
                AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0),
                -AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0), monotonic_ns());
        return 1;
    }
    if ((src & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) return 0;
    if (!touch_enabled()) return 0;  // NX_TOUCH_SHOW=0
    int32_t action = AMotionEvent_getAction(e);
    int32_t kind = action & AMOTION_EVENT_ACTION_MASK;
    size_t index = size_t((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const uint64_t now = monotonic_ns();
    g_last_touch_ns = now;
    if (ANativeWindow* w = android_window()) {
        g_w = float(ANativeWindow_getWidth(w));
        g_h = float(ANativeWindow_getHeight(w));
    }
    if (kind == AMOTION_EVENT_ACTION_CANCEL) {
        reset_touch();
        return 1;
    }
    const bool queued = p4g_input();
    const uint64_t before = queued ? held_touch_buttons() : 0;
    if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        float x = AMotionEvent_getX(e, index), y = AMotionEvent_getY(e, index);
        g_gamepad = false;
        size_t drawn = 0;
        touch_buttons(drawn);
        if (drawn || p4g_input()) {  // P4G keeps its stick/camera regions even when every button is hidden
            uint64_t b = touch_hit(x / g_w, y / g_h, g_w / g_h);
            const bool stick_side = !b && touch_stick_side(x / g_w);
            const bool fixed_left = p4g_input() && touch_left_stick_fixed();
            const bool left = stick_side && (!fixed_left || touch_stick_hit(x / g_w, y / g_h, g_w / g_h));
            Stick stick = b ? kNoStick : left ? kLeftStick :
                    !stick_side && touch_right_stick() ? kRightStick : kNoStick;
            if (b || stick) {
                float x0 = x, y0 = y;
                if (left && fixed_left) {
                    float sx, sy, sr;
                    touch_stick_rest(sx, sy, sr);
                    x0 = sx * g_w;
                    y0 = sy * g_h;
                }
                g_touches.push_back({AMotionEvent_getPointerId(e, index), x0, y0, x, y, stick, b, now});
            }
        } else {
            bool left = x < g_w * 0.5f;
            g_touches.push_back({AMotionEvent_getPointerId(e, index), x, y, x, y, left ? kLeftStick : kNoStick,
                                 left ? 0 : touch_button(x, y), now});
        }
    } else if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP) {
        int32_t id = AMotionEvent_getPointerId(e, index);
        const bool all = kind == AMOTION_EVENT_ACTION_UP;
        g_touches.erase(std::remove_if(g_touches.begin(), g_touches.end(),
                                       [&](const Touch& t) {
                                           if (!all && t.id != id) return false;
                                           if (!queued) release_touch(t, now);
                                           return true;
                                       }),
                        g_touches.end());
    } else if (kind == AMOTION_EVENT_ACTION_MOVE) {
        for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) {
            int32_t id = AMotionEvent_getPointerId(e, k);
            for (auto& t : g_touches)
                if (t.id == id) {
                    t.x = AMotionEvent_getX(e, k);
                    t.y = AMotionEvent_getY(e, k);
                }
        }
    }
    if (queued) queue_edges(before ^ held_touch_buttons());
    return 1;
}

bool headless() {  // NX_HEADLESS (platform.h)
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

static void read_pad_impl(PadState& p, bool sample_buttons) {
    std::lock_guard lk(g_in);
    if (gamepad_run_fix()) sample_gamepad_left(monotonic_ns());
    const char* analog_origin = "none";
    bool analog_touch_left = false;
    p.buttons |= g_pad_buttons.buttons;
    if (g_pad_buttons.lx || g_pad_buttons.ly) {
        analog_origin = "controller";
        p.lx = g_pad_buttons.lx;
        p.ly = g_pad_buttons.ly;
    }
    p.rx = g_pad_buttons.rx;
    p.ry = g_pad_buttons.ry;
    float radius = std::max(1.f, g_h * 0.12f);
    size_t drawn = 0;
    touch_buttons(drawn);
    const uint64_t now = monotonic_ns();
    g_latched.erase(std::remove_if(g_latched.begin(), g_latched.end(), [&](const Latch& l) { return now >= l.until_ns; }),
                    g_latched.end());
    for (const auto& l : g_latched) p.buttons |= l.button;
    const bool queued = p4g_input();
    if (queued) {
        if (sample_buttons) sample_touch_buttons(now);
        p.buttons |= g_sampled_buttons;
    }
    for (const auto& t : g_touches) {
        float dx = (t.x - t.x0) / radius, dy = (t.y - t.y0) / radius;
        if (t.stick == kLeftStick) {
            analog_origin = "touch";
            analog_touch_left = true;
            float sx = dx * stick_sens();
            float sy = -dy * stick_sens();
            if (p4g_input()) {
                const StickPosition left = radial_stick(sx, sy, 0.f, 0.98f);
                p.lx = left.x;
                p.ly = left.y;
            } else {
                p.lx = int32_t(std::clamp(sx, -1.f, 1.f) * kStickMax);
                p.ly = int32_t(std::clamp(sy, -1.f, 1.f) * kStickMax);
            }
            // A drawn layout's stick pushed nearly all the way may hold a button too (Wonder: Y, dash), so running
            // needs no second thumb on the right side.
            if (drawn && std::abs(dx) > 0.85f) p.buttons |= touch_stick_full_button();
        } else if (t.stick == kRightStick) {
            float sx = dx * camera_sens();
            float sy = -dy * camera_sens();
            p.rx = int32_t(std::clamp(sx, -1.f, 1.f) * kStickMax);
            p.ry = int32_t(std::clamp(sy, -1.f, 1.f) * kStickMax);
        } else if (!queued) {
            p.buttons |= t.button;
        }
    }
    const int32_t before_script_lx = p.lx, before_script_ly = p.ly;
    input_script(p);  // NX_INPUT_SCRIPT (config.txt): scripted runs on the phone, as on the Mac
    if (p.lx != before_script_lx || p.ly != before_script_ly) analog_origin = "script";
    g_left_read_origin = analog_origin;
    if (sample_buttons && queued && analog_trace_enabled()) trace_merged(p, analog_origin, analog_touch_left, now);
}

void read_pad(PadState& p) { read_pad_impl(p, true); }
void read_pad_motion(PadState& p) { read_pad_impl(p, false); }

// Opt-in diagnosis at the final HID write, after its circular clamp. The project has no validated P4G player
// position/speed address: never report analog strength or a guessed guest-memory value as character speed.
void trace_gamepad_left(const PadState& p, uint64_t frame) {
    static const bool on = [] {
        const char* e = std::getenv("NX_P4G_STICK_TRACE");
        return e && std::strcmp(e, "1") == 0 && std::strcmp(nx::game::kInfo.id, "p4g") == 0;
    }();
    if (!on) return;
    std::lock_guard lk(g_in);
    if (!g_analog_trace.last_event_ns || std::strcmp(g_left_read_origin, "touch") == 0 ||
        std::strcmp(g_left_read_origin, "script") == 0) return;
    static uint64_t last_frame = UINT64_MAX;
    static double frame_min = 0;
    static unsigned lines = 0;
    if ((last_frame != UINT64_MAX && frame < last_frame) || lines >= 18'000) return;
    const double magnitude = std::hypot(double(p.lx), double(p.ly));
    if (frame == last_frame && magnitude >= frame_min) return;
    if (!lines) nx::log("p4g: final gamepad trace enabled (per presented frame plus lower writes, max 18000; character speed unavailable)");
    last_frame = frame;
    frame_min = magnitude;
    ++lines;
    nx::log("p4g: stick-final frame=%llu t_ns=%llu lx=%d ly=%d magnitude=%.3f normalized=%.6f "
            "buttons=%llx origin=%s run_fix=%u running=%u character_speed=unavailable",
            (unsigned long long)frame, (unsigned long long)monotonic_ns(), int(p.lx), int(p.ly),
            magnitude, magnitude / kStickMax, (unsigned long long)p.buttons, g_left_read_origin,
            unsigned(gamepad_run_fix()), unsigned(g_gamepad_run.running()));
}

// The drawn layout's buttons (brighter while pressed) and the stick under the thumb; nothing while a controller is
// in use or for games without a drawn layout.
size_t touch_overlay(OverlayShape* out, size_t max) {
    size_t n = 0, k = 0;
    const TouchButton* b = touch_buttons(n);
    if (!n && !p4g_input()) return 0;  // P4G may hide every button while keeping the analog ring
    std::lock_guard lk(g_in);
    if (g_gamepad || !touch_enabled()) return 0;
    // Dimmed to a third while nobody touches the screen (cutscenes, videos), back at once on the next touch.
    const bool idle = g_touches.empty() && monotonic_ns() - g_last_touch_ns > kTouchIdleNs;
    const float shade = touch_opacity_scale() * (idle ? 0.35f : 1.0f);
    const Touch* stick = nullptr;
    const Touch* camera = nullptr;
    uint64_t held = 0;
    for (const auto& t : g_touches) {
        if (t.stick == kLeftStick) stick = &t;
        if (t.stick == kRightStick) camera = &t;
        held |= t.button;
    }
    for (const auto& l : g_latched) held |= l.button;
    for (size_t i = 0; i < n && k < max; i++)
        out[k++] = {b[i].x, b[i].y, b[i].r, std::min(1.0f, (held & b[i].button ? 0.5f : 0.2f) * shade), b[i].label};
    // Fixed mode uses the configured ring; floating left/right sticks remain relative to touch-down.
    auto draw_stick = [&](const Touch& t) {
        if (k + 2 > max) return;
        float r = 0.12f, dx = (t.x - t.x0) / g_h, dy = (t.y - t.y0) / g_h;
        float len = std::hypot(dx, dy), s = len > r ? r / len : 1.f;
        out[k++] = {t.x0 / g_w, t.y0 / g_h, r, std::min(1.0f, 0.14f * shade)};
        out[k++] = {(t.x0 / g_h + dx * s) * g_h / g_w, t.y0 / g_h + dy * s, 0.055f, std::min(1.0f, 0.45f * shade)};
    };
    if (stick) {
        draw_stick(*stick);
    } else if (k < max) {  // where the stick goes
        float sx, sy, sr;
        touch_stick_rest(sx, sy, sr);
        out[k++] = {sx, sy, sr, std::min(1.0f, 0.08f * shade)};
    }
    if (camera) draw_stick(*camera);
    return k;
}

}  // namespace nx::plat

