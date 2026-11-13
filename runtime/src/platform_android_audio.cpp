// Android audio output; stream control stays outside the real-time callbacks.
#include <aaudio/AAudio.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <nx/runtime.h>
#include "platform.h"
#include "audio_ring.h"

// Android audio out: AAudio (split from platform_android.cpp).
namespace nx::plat {

// ---- audio: AAudio (callback pulls from a queue of submitted buffers) ------------------------------------------

namespace {
// The game's buffers are copied into a ring at submit and played from there. Each is handed back (done) when the
// device's read position reaches its end minus the A/V delay and a small burst-dependent reserve. Returning copied
// buffers early lets a game with three 5 ms buffers supply a device that pulls 20 ms at once. Once that reserve is
// filled, completions follow the device clock (audio_set_delay_ms, set from the frame rate: nvn_queue.cpp).
constexpr uint64_t kRingFrames = 1 << 16;  // ~1.4 s of 48 kHz stereo s16
struct Pending {
    uint64_t end;  // ring position (frames) after its last frame
    void* ctx;
};
std::mutex g_audio;
// Never acquired by the AAudio callbacks. Serializes stream lifetime and timestamp queries.
std::mutex g_stream_control;
bool g_enabled = false, g_paused = false;
std::atomic<bool> g_disconnected{false};
std::condition_variable g_audio_ready;
std::vector<uint32_t> g_ring(kRingFrames);  // one stereo s16 frame per element
uint64_t g_written = 0, g_read = 0;         // frames, since the stream opened
uint64_t g_device = 0;                      // frames the callback gave the device (silence included): AAudio's count
std::atomic<uint64_t> g_latency_ns{0};      // audio_latency_ns
std::deque<Pending> g_pending;
std::atomic<uint32_t> g_delay_frames{0};
uint32_t g_reserve_frames = 480;  // under g_audio; normally two bursts, capped at half the ring
bool g_priming = true;
AudioDone g_done = nullptr;
AAudioStream* g_stream = nullptr;
// Per ~2 s, logged from the completion worker: how the device pulls (callbacks, frames per call) and how often the game's
// buffers ran out (underruns: silence played, and the game's buffers come back late, its clock stalls).
struct CbStats {
    uint64_t from_ns = 0, calls = 0, frames = 0, underruns = 0, silent_frames = 0, max_gap_ns = 0, last_ns = 0;
    int32_t min_frames = 1 << 30, max_frames = 0;
} g_cb;

void audio_reopen_if_disconnected();

uint32_t reserve_for_burst(uint64_t frames) {
    return uint32_t(std::clamp<uint64_t>(frames * 2, 480, kRingFrames / 2));
}

// Completion callbacks can take the guest audio or renderer locks. Keep them on a worker, including completions
// made possible by audio_submit(), so a renderer submitting while holding its own lock cannot deadlock itself.
void audio_completions() {
    for (;;) {
        void* ctx = nullptr;
        bool completed = false;
        CbStats stats;
        uint64_t now;
        {
            std::unique_lock lk(g_audio);
            g_audio_ready.wait_for(lk, std::chrono::milliseconds(250), [] {
                const uint64_t due = g_read + g_reserve_frames + g_delay_frames.load(std::memory_order_relaxed);
                return !g_pending.empty() && g_pending.front().end <= due;
            });
            const uint64_t due = g_read + g_reserve_frames + g_delay_frames.load(std::memory_order_relaxed);
            if (!g_pending.empty() && g_pending.front().end <= due) {
                ctx = g_pending.front().ctx;
                g_pending.pop_front();
                completed = true;
            }
            now = monotonic_ns();
            if (g_cb.calls && now - g_cb.from_ns >= 2'000'000'000ull) {
                stats = g_cb;
                g_cb = {};
            }
        }
        if (stats.calls && (stats.underruns || std::getenv("NX_AUDIO_STATS")))
            nx::log("audio: %llu callbacks in %.1f s (%d..%d frames, longest %.1f ms apart); %llu underruns, %.0f ms silence",
                    (unsigned long long)stats.calls, double(now - stats.from_ns) / 1e9, stats.min_frames, stats.max_frames,
                    double(stats.max_gap_ns) / 1e6, (unsigned long long)stats.underruns, double(stats.silent_frames) / 48.0);
        {
            std::lock_guard stream_lock(g_stream_control);
            audio_reopen_if_disconnected();
        }
        if (completed) g_done(ctx);
    }
}

void cb_stats(int32_t frames, size_t missing_bytes) {
    CbStats& s = g_cb;
    const uint64_t now = monotonic_ns();
    if (!s.from_ns) s.from_ns = now;
    if (s.last_ns) s.max_gap_ns = std::max(s.max_gap_ns, now - s.last_ns);
    s.last_ns = now;
    s.calls++, s.frames += uint64_t(frames);
    s.min_frames = std::min(s.min_frames, frames), s.max_frames = std::max(s.max_frames, frames);
    if (missing_bytes) s.underruns++, s.silent_frames += missing_bytes / 4;
}

aaudio_data_callback_result_t audio_cb(AAudioStream*, void*, void* out, int32_t frames) {
    if (frames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;
    auto* dst = static_cast<uint32_t*>(out);
    size_t missing = 0;
    {
        std::lock_guard lk(g_audio);
        // AAudio's callback size may change or exceed the nominal hardware burst.
        g_reserve_frames = std::max(g_reserve_frames, reserve_for_burst(uint64_t(frames)));
        const uint64_t available = g_written - g_read;
        if (g_priming && available >= g_reserve_frames) g_priming = false;
        const uint64_t n = g_priming ? 0 : std::min<uint64_t>(uint64_t(frames), available);
        read_audio_ring(dst, g_ring.data(), kRingFrames, g_read, size_t(n));
        g_read += n;
        g_device += uint64_t(frames);
        missing = size_t(frames) - size_t(n);
        if (missing) std::memset(dst + n, 0, missing * 4);  // underrun: silence
        if (missing) g_priming = true;
        cb_stats(frames, missing * 4);
    }
    g_audio_ready.notify_one();
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void audio_error_cb(AAudioStream*, void*, aaudio_result_t error) {
    if (error == AAUDIO_ERROR_DISCONNECTED) g_disconnected.store(true, std::memory_order_relaxed);
}

bool audio_stream_create() {
    g_disconnected.store(false, std::memory_order_relaxed);
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, nullptr);
    AAudioStreamBuilder_setErrorCallback(b, audio_error_cb, nullptr);
    bool ok = AAudioStreamBuilder_openStream(b, &g_stream) == AAUDIO_OK;
    AAudioStreamBuilder_delete(b);
    if (!ok) { g_stream = nullptr; return false; }
    const int32_t burst = AAudioStream_getFramesPerBurst(g_stream);
    {
        std::lock_guard lk(g_audio);
        g_reserve_frames = reserve_for_burst(uint64_t(std::max(burst, 1)));
        g_priming = true;
        // AAudio timestamps restart at zero for every new stream; ring positions do not.
        g_device = 0;
        g_latency_ns.store(0, std::memory_order_relaxed);
        g_cb = {};
    }
    if (AAudioStream_requestStart(g_stream) != AAUDIO_OK) {
        AAudioStream_close(g_stream);
        g_stream = nullptr;
        return false;
    }
    nx::log("audio: AAudio stream: %d frames per burst, buffer %d of %d frames, performance mode %d",
            int(burst), int(AAudioStream_getBufferSizeInFrames(g_stream)),
            int(AAudioStream_getBufferCapacityInFrames(g_stream)), int(AAudioStream_getPerformanceMode(g_stream)));
    nx::log("audio: experimental burst-buffer-v1: %u reserve frames (%.1f ms), completion worker enabled",
            reserve_for_burst(uint64_t(std::max(burst, 1))),
            double(reserve_for_burst(uint64_t(std::max(burst, 1)))) / 48.0);
    return true;
}

void audio_reopen_if_disconnected() {
    if (!g_enabled || g_paused) return;
    if (g_stream && !g_disconnected.load(std::memory_order_relaxed) &&
        AAudioStream_getState(g_stream) != AAUDIO_STREAM_STATE_DISCONNECTED) return;
    if (g_stream) AAudioStream_close(g_stream);
    g_stream = nullptr;
    audio_stream_create();
}
}  // namespace

bool audio_open(AudioDone done) {
    std::lock_guard stream_lock(g_stream_control);
    g_done = done;
    // NX_MUTE=1 (and headless test runs): no device, the callers pace themselves with a timer (as on Windows and macOS).
    if (headless() || std::getenv("NX_MUTE") || !audio_stream_create()) return false;
    g_enabled = true;
    std::thread(audio_completions).detach();
    return true;
}

void audio_pause(bool paused) {
    std::lock_guard stream_lock(g_stream_control);
    g_paused = paused;
    if (!g_enabled) return;
    if (paused) {
        if (g_stream) AAudioStream_requestPause(g_stream);
    } else {
        audio_reopen_if_disconnected();
        if (g_stream && AAudioStream_requestStart(g_stream) == AAUDIO_ERROR_DISCONNECTED)
            audio_reopen_if_disconnected();
    }
}

// When the buffer starting at ring position `start` reaches the speaker: AAudio's last timestamp (a frame the device
// presented and when) moved by the frames between, the ring's queue ahead of it and the device's own buffer included.
// Every 8th buffer, smoothed (A/V sync, nvn_queue.cpp).
static void measure_latency(uint64_t start, uint64_t read, uint64_t device) {
    static uint32_t n = 0;
    if (n++ % 8) return;
    int64_t pos = 0, at = 0;
    if (!g_stream || AAudioStream_getTimestamp(g_stream, CLOCK_MONOTONIC, &pos, &at) != AAUDIO_OK || at <= 0) return;
    const int64_t frame = int64_t(device + (start - read));  // the device's index of the buffer's first frame
    const int64_t when = at + (frame - pos) * 1000000000ll / 48000;
    const int64_t now = int64_t(monotonic_ns());
    if (when <= now || when - now > 2000000000ll) return;
    const uint64_t l = uint64_t(when - now), old = g_latency_ns.load(std::memory_order_relaxed);
    g_latency_ns.store(old ? old + (int64_t(l) - int64_t(old)) / 8 : l, std::memory_order_relaxed);
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    std::lock_guard stream_lock(g_stream_control);
    audio_reopen_if_disconnected();
    if (!g_enabled || !g_stream) return false;
    uint64_t start, read, device;
    {
        std::lock_guard lk(g_audio);
        start = g_written, read = g_read, device = g_device;
        const uint64_t n = std::min<uint64_t>(bytes / 4, kRingFrames - (g_written - g_read));  // never more than a full ring
        write_audio_ring(g_ring.data(), kRingFrames, g_written, data, size_t(n));
        g_written += n;
        g_pending.push_back({g_written, ctx});
    }
    g_audio_ready.notify_one();
    measure_latency(start, read, device);
    return true;
}

uint64_t audio_latency_ns() { return g_latency_ns.load(std::memory_order_relaxed); }

void audio_set_delay_ms(uint32_t ms) {
    {
        std::lock_guard lk(g_audio);
        g_delay_frames.store(std::min<uint32_t>(ms, 500) * 48, std::memory_order_relaxed);
    }
    g_audio_ready.notify_one();
}

uint32_t audio_queued() {
    std::lock_guard lk(g_audio);
    return uint32_t(g_pending.size());
}

}  // namespace nx::plat
