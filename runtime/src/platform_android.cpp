#include <cstdlib>
// Android host (NativeActivity): memory (mmap at fixed addresses), pthreads, the app's window (ANativeWindow),
// game controllers + touch, AAudio, the Vulkan loader and android surface, a signal crash handler and
// android_main. Game data lives in the app's external files dir (see docs/ANDROID.md):
//   <files>/analysis/{main,sdk}.bin   module images (tools/nso2bin.py)
//   <files>/extract/romfs/...         the RomFS
//   <files>/user/...                  saves
//   <files>/config.txt                optional: resolution=WxH
#include <aaudio/AAudio.h>
#include <android/api-level.h>
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
#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
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
#if defined(NX_CUSTOM_DRIVER)
#include <adrenotools/driver.h>
#endif

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"
#include "platform_android.h"
#include "android_surface_size.h"
#include "frame_history.h"
#if NX_NCE
#include "nce/nce.h"
#endif

// Keep this after nx/module.h (included by nx/runtime.h): Android NDK 28's
// elf_common.h defines R_AARCH64_* macros with the same names as NX constants.
#include <link.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace nx {
void boot();
}

namespace nx::plat {

namespace {
android_app* g_app = nullptr;
std::string g_root;
std::mutex g_window_lock;
std::condition_variable g_window_ready;
std::atomic<ANativeWindow*> g_window{nullptr};
std::atomic<uint64_t> g_window_gen{0};  // bumped for every new window
std::atomic<bool> g_visible{false};
}  // namespace

// ---- memory ---------------------------------------------------------------------------------------------------

bool reserve(uint64_t addr, uint64_t size) {
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (uint64_t(p) != addr) {  // old kernels ignore the flag and treat addr as a hint
        munmap(p, size);
        return false;
    }
    return true;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    return mprotect(reinterpret_cast<void*>(a), e - a, PROT_READ | PROT_WRITE) == 0;
}

void decommit(uint64_t addr, uint64_t size) {
    madvise(reinterpret_cast<void*>(addr), size, MADV_DONTNEED);
    mprotect(reinterpret_cast<void*>(addr), size, PROT_NONE);
}

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static void* thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    place_fast_cores();
    s.fn(s.arg);
    return nullptr;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_size);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, thread_entry, new ThreadStart{fn, arg});
    pthread_attr_destroy(&a);
}

void exit_thread() { pthread_exit(nullptr); }
void exit_process(int code) { _exit(code); }

void sleep_ns(int64_t ns) {
    timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
    nanosleep(&ts, nullptr);
}

// Wonder's job system yields after every job item and in its barriers (svc YieldThread; DEBUG.md #44). NX_YIELD=spin:
// a short pause on the core instead of the scheduler (the main thread made ~52 voluntary context switches per frame
// in course, PHASE3.md B5); unset or sched: sched_yield, as before.
void yield() {
    static const bool spin = [] {
        const char* e = std::getenv("NX_YIELD");
        return e && std::strcmp(e, "spin") == 0;
    }();
    if (!spin) {
        sched_yield();
        return;
    }
    for (int k = 0; k < 64; k++) asm volatile("yield" ::: "memory");
}
void set_thread_name(const char* name) { pthread_setname_np(pthread_self(), name); }
int64_t current_thread_id() { return gettid(); }

// ---- sampling profiler (simpleperf is not always allowed on retail phones) ------------------------------------

namespace {
constexpr uint32_t kMaxSamples = 1u << 18;
constexpr int kFrames = 12;
struct Sample {
    int32_t tid;
    uint64_t guest_lr;
    uint64_t pc[kFrames];  // host pc, lr, then return addresses from the frame-pointer chain
};
std::atomic<uint32_t> g_nsamples{0};
Sample* g_samples = nullptr;
const int64_t* g_prof_tids = nullptr;
const uint64_t* const* g_prof_lr = nullptr;
size_t g_prof_count = 0;

void on_sample(int, siginfo_t*, void* uc) {
    uint32_t i = g_nsamples.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxSamples) return;
    Sample& s = g_samples[i];
    const mcontext_t& m = static_cast<ucontext_t*>(uc)->uc_mcontext;
    s.tid = gettid();
    s.guest_lr = 0;
    for (size_t k = 0; k < g_prof_count; k++)
        if (g_prof_tids[k] == s.tid) s.guest_lr = *g_prof_lr[k];
    std::memset(s.pc, 0, sizeof s.pc);
    s.pc[0] = m.pc;
    s.pc[1] = m.regs[30];
    // Frame records (fp -> [prev fp, lr]) on this thread's stack only: between sp and sp + 256 MiB, going up.
    uint64_t fp = m.regs[29], lo = m.sp, hi = m.sp + (256ull << 20);
    for (int k = 2; k < kFrames && fp >= lo && fp < hi && (fp & 7) == 0; k++) {
        const uint64_t* rec = reinterpret_cast<const uint64_t*>(fp);
        s.pc[k] = rec[1];
        if (rec[0] <= fp) break;
        fp = rec[0];
    }
}
}  // namespace

bool profile(const int64_t* tids, const uint64_t* const* guest_lr, size_t count, int seconds, const std::string& path) {
    if (!g_samples) g_samples = new Sample[kMaxSamples];
    g_prof_tids = tids;
    g_prof_lr = guest_lr;
    g_prof_count = count;
    g_nsamples = 0;
    struct sigaction sa{};
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;  // never on a guest stack (native execution)
    sigaction(SIGPROF, &sa, nullptr);
    std::vector<timer_t> timers;
    for (size_t k = 0; k < count; k++) {
        // The thread's CPU-time clock (Linux: MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)).
        clockid_t clock = clockid_t((~uint32_t(tids[k]) << 3) | 6);
        sigevent sev{};
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev.sigev_notify_thread_id = int(tids[k]);
        timer_t t;
        if (timer_create(clock, &sev, &t) != 0) continue;
        itimerspec its{{0, 1'000'000}, {0, 1'000'000}};
        timer_settime(t, 0, &its, nullptr);
        timers.push_back(t);
    }
    sleep_ns(int64_t(seconds) * 1'000'000'000);
    for (timer_t t : timers) timer_delete(t);
    sleep_ns(10'000'000);
    g_prof_count = 0;
    uint32_t n = std::min(g_nsamples.load(), kMaxSamples);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    Dl_info self{};
    dladdr(reinterpret_cast<void*>(&on_sample), &self);
    for (uint32_t i = 0; i < n; i++) {
        const Sample& s = g_samples[i];
        std::fprintf(f, "%d %llx", s.tid, (unsigned long long)s.guest_lr);
        for (int k = 0; k < kFrames && s.pc[k]; k++) {
            Dl_info d{};
            if (dladdr(reinterpret_cast<void*>(s.pc[k]), &d) && d.dli_fbase) {
                const char* name = d.dli_fbase == self.dli_fbase ? "self" : d.dli_fname;
                const char* slash = std::strrchr(name, '/');
                std::fprintf(f, " %s:%llx", slash ? slash + 1 : name,
                             (unsigned long long)(s.pc[k] - uint64_t(d.dli_fbase)));
            } else {
                std::fprintf(f, " ?:%llx", (unsigned long long)s.pc[k]);
            }
        }
        std::fputc('\n', f);
    }
    std::fclose(f);
    return true;
}

uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) { return fseeko(f, off, origin); }
uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    // A size that does not fit ssize_t (the game passes -1) reads nothing, as fread did (and the game expects).
    if (size > uint64_t(SSIZE_MAX)) return 0;
    int fd = fileno(f);
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, static_cast<uint8_t*>(buf) + done, size_t(size - done), off + int64_t(done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += uint64_t(n);
    }
    return done;
}
bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

void log_line(const char* line) { __android_log_write(ANDROID_LOG_INFO, nx::game::kInfo.id, line); }

std::string data_root() { return g_root; }

// The device's locale (Settings > System > Languages) as "es-MX".
std::string system_language() {
    if (!g_app || !g_app->config) return "";
    char lang[2] = {}, country[2] = {};
    AConfiguration_getLanguage(g_app->config, lang);
    AConfiguration_getCountry(g_app->config, country);
    if (!lang[0]) return "";
    std::string s(lang, lang[1] ? 2 : 1);
    if (country[0]) s += "-" + std::string(country, country[1] ? 2 : 1);
    return s;
}

std::string setting(const std::string& key) {
    std::ifstream f(g_root + "/config.txt");
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        auto eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return {};
}

// ---- window ---------------------------------------------------------------------------------------------------

// The window handed out stays valid for good (we hold a reference): Android destroys the activity's window when
// the app goes to the background or the screen locks (APP_CMD_TERM_WINDOW), and until then we wait for one.
void* create_window(uint64_t& generation) {
    std::unique_lock lock(g_window_lock);
    g_window_ready.wait(lock, [] { return g_window.load() != nullptr; });
    ANativeWindow* w = g_window.load();
    ANativeWindow_acquire(w);
    generation = g_window_gen.load();
    return w;
}

void* create_window() {
    uint64_t generation;
    return create_window(generation);
}

void release_window(void* w) { ANativeWindow_release(static_cast<ANativeWindow*>(w)); }

uint64_t window_generation() { return g_window_gen; }

ANativeWindow* android_window() { return g_window.load(); }  // platform_android.h

bool window_minimized() { return !g_visible || !g_window; }

void screen_size(uint32_t& w, uint32_t& h) {
    ANativeWindow* win = static_cast<ANativeWindow*>(create_window());
    int32_t a = ANativeWindow_getWidth(win), b = ANativeWindow_getHeight(win);
    ANativeWindow_release(win);
    w = uint32_t(std::max(a, b));  // landscape
    h = uint32_t(std::min(a, b));
}

// The render size: resolution=WxH if set, else the screen's size times the render scale (render_scale=0.5..1, or
// the GPU's default), never taller than 1080 (the game's art is made for 720p/1080p; more only costs GPU time).
void window_size(uint32_t& w, uint32_t& h) {
    uint32_t sw = 0, sh = 0;
    screen_size(sw, sh);
    std::string r = setting("resolution");
    if (r.empty()) r = setting("NX_RES");
    if (r.empty()) r = setting("RESOLUTION");
    unsigned cw = 0, ch = 0;
    if (!r.empty() && std::sscanf(r.c_str(), "%ux%u", &cw, &ch) == 2 && cw && ch) {
        w = cw;
        h = ch;
        return;
    }
    uint32_t target_h = 0;
    if (r == "360p" || r == "360") target_h = 360;
    else if (r == "480p" || r == "480") target_h = 480;
    else if (r == "720p" || r == "720") target_h = 720;
    else if (r == "1080p" || r == "1080") target_h = 1080;

    if (target_h && sh) {
        float scale = float(target_h) / float(sh);
        w = uint32_t(float(sw) * scale + 0.5f) & ~1u;
        h = target_h & ~1u;
        return;
    }
    std::string rs = setting("render_scale");
    float scale = rs.empty() ? default_render_scale() : std::strtof(rs.c_str(), nullptr);
    scale = std::clamp(scale, 0.25f, 1.0f);
    scale = std::min(scale, 1080.0f / float(sh));
    w = uint32_t(float(sw) * scale + 0.5f) & ~1u;
    h = uint32_t(float(sh) * scale + 0.5f) & ~1u;
}

// A GPU tier from the Vulkan device name (queried once, before the game starts its renderer): current Adreno 7xx
// and 8xx, Mali-G7xx and newer, Immortalis, Xclipse render at 1; Adreno 6xx at 0.85; anything older at 2/3
// (about 720p, the Switch's handheld resolution).
float default_render_scale() {
    static float scale = [] {
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(vulkan_get_instance_proc_addr());
        if (!gipa) return 2.0f / 3;
        auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        VkInstance inst = VK_NULL_HANDLE;
        if (!create || create(&ci, nullptr, &inst) != VK_SUCCESS) return 2.0f / 3;
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
        auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
        auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
        uint32_t n = 1;
        VkPhysicalDevice pd = VK_NULL_HANDLE;
        float s = 2.0f / 3;
        if (enumerate(inst, &n, &pd) >= 0 && n && pd) {
            VkPhysicalDeviceProperties p{};
            props(pd, &p);
            std::string name = p.deviceName;
            int adreno = 0, mali = 0;
            std::sscanf(name.c_str() + (name.find("Adreno") != std::string::npos ? name.find("Adreno") : name.size()),
                        "Adreno (TM) %d", &adreno);
            if (auto m = name.find("Mali-G"); m != std::string::npos) std::sscanf(name.c_str() + m, "Mali-G%d", &mali);
            if (adreno >= 700 || mali >= 710 || name.find("Immortalis") != std::string::npos ||
                name.find("Xclipse") != std::string::npos)
                s = 1.0f;
            else if (adreno >= 610 || mali >= 76)
                s = 0.85f;
            nx::log("gpu: %s -> default render scale %.2f", p.deviceName, s);
        }
        destroy(inst, nullptr);
        return s;
    }();
    return scale;
}

// ---- Vulkan ---------------------------------------------------------------------------------------------------

#if defined(NX_CUSTOM_DRIVER)
namespace {

bool module_loaded(const std::string& library) {
    struct Search {
        const std::string& library;
        bool found = false;
    } search{library};
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* opaque) {
        auto& current = *static_cast<Search*>(opaque);
        const char* path = info->dlpi_name;
        if (!path) return 0;
        const char* base = std::strrchr(path, '/');
        if (current.library == (base ? base + 1 : path)) {
            current.found = true;
            return 1;
        }
        return 0;
    }, &search);
    return search.found;
}

bool probe_vulkan(PFN_vkGetInstanceProcAddr gipa, VkDriverId driver, VkDriverId alternate,
                  uint32_t minimum_api, bool powervr, bool xclipse) {
    if (!gipa) {
        nx::log("gpu driver probe: vkGetInstanceProcAddr is unavailable");
        return false;
    }
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create) {
        nx::log("gpu driver probe: vkCreateInstance is unavailable");
        return false;
    }
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = minimum_api;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = extensions;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult created = create(&ci, nullptr, &instance);
    if (created != VK_SUCCESS) {
        nx::log("gpu driver probe: vkCreateInstance failed (%d, requested Vulkan %u.%u)", int(created),
                VK_API_VERSION_MAJOR(minimum_api), VK_API_VERSION_MINOR(minimum_api));
        return false;
    }
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        gipa(instance, "vkGetPhysicalDeviceProperties2"));
    bool matches = false;
    uint32_t count = 0;
    if (!destroy || !enumerate || !properties) {
        nx::log("gpu driver probe: required Vulkan instance functions are unavailable");
    } else if (const VkResult found = enumerate(instance, &count, nullptr); found != VK_SUCCESS || !count) {
        nx::log("gpu driver probe: physical-device enumeration failed (%d, count %u)", int(found), count);
    } else {
        std::vector<VkPhysicalDevice> devices(count);
        const VkResult listed = enumerate(instance, &count, devices.data());
        if (listed == VK_SUCCESS) {
            for (uint32_t i = 0; i < count; ++i) {
                VkPhysicalDeviceDriverProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                props.pNext = &identity;
                properties(devices[i], &props);
                const bool expected_driver = identity.driverID == driver ||
                    (alternate && identity.driverID == alternate) ||
                    (powervr && props.properties.vendorID == 0x1010) ||
                    (xclipse && (props.properties.vendorID == 0x144d ||
                                 std::strstr(props.properties.deviceName, "Xclipse") != nullptr));
                nx::log("gpu driver probe: %s, vendor %04x, driver id %u, Vulkan %u.%u",
                        props.properties.deviceName, props.properties.vendorID, unsigned(identity.driverID),
                        VK_API_VERSION_MAJOR(props.properties.apiVersion),
                        VK_API_VERSION_MINOR(props.properties.apiVersion));
                if (expected_driver && props.properties.apiVersion >= minimum_api) {
                    matches = true;
                    break;
                }
            }
        } else nx::log("gpu driver probe: physical-device listing failed (%d)", int(listed));
    }
    if (destroy) destroy(instance, nullptr);
    if (!matches)
        nx::log("gpu driver probe: no compatible device (expected id %u or %u, minimum Vulkan %u.%u)",
                unsigned(driver), unsigned(alternate), VK_API_VERSION_MAJOR(minimum_api),
                VK_API_VERSION_MINOR(minimum_api));
    return matches;
}

// Adapted from P5R Android v0.5.3. The custom-driver hook is independent of
// KGSL memory import, so it can also load Android PanVK, PowerVR and Xclipse packages.
void* custom_vulkan() {
    if (!g_app || !g_app->activity->internalDataPath) return nullptr;
    if (const char* value = std::getenv("NX_SYSTEM_DRIVER"); value && std::strcmp(value, "1") == 0)
        return nullptr;
    const std::string root = std::string(g_app->activity->internalDataPath) + "/gpu_driver/";
    std::ifstream marker(root + "active.txt");
    std::string path;
    if (!std::getline(marker, path) || path.size() > 256) return nullptr;
    const size_t slash = path.find('/');
    if (slash == std::string::npos) return nullptr;
    const std::string folder = path.substr(0, slash), library = path.substr(slash + 1);
    const bool panvk = folder.starts_with("panvk-");
    const bool powervr = folder.starts_with("powervr-");
    const bool xclipse = folder.starts_with("xclipse-");
    if (!panvk && !powervr && !xclipse && !folder.starts_with("turnip-")) return nullptr;
    const size_t prefix = panvk ? 6 : powervr ? 8 : xclipse ? 8 : 7;
    if (folder.size() != prefix + 36 || folder.find_first_not_of("0123456789abcdef-", prefix) != std::string::npos ||
        library.empty() || !library.ends_with(".so") ||
        library.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos)
        return nullptr;
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&custom_vulkan), &info) || !info.dli_fname) return nullptr;
    std::string hooks = info.dli_fname;
    hooks.resize(hooks.rfind('/') + 1);
    const std::string dir = root + folder + "/";
    if (powervr) {
        // Mesa PVR identifies itself as non-conformant and refuses to initialize without this explicit opt-in.
        setenv("PVR_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
        nx::log("gpu driver: enabled experimental Mesa PVR initialization");
    }
    void* handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
                                             hooks.c_str(), dir.c_str(), library.c_str(), nullptr, nullptr);
    void* gipa = handle ? dlsym(handle, "vkGetInstanceProcAddr") : nullptr;
    // Numeric Xclipse values keep this build compatible with older Android NDK Vulkan headers:
    // VK_DRIVER_ID_SAMSUNG_PROPRIETARY = 21 and VK_DRIVER_ID_MESA_RADV = 3.
    const VkDriverId expected = panvk ? VK_DRIVER_ID_MESA_PANVK
        : powervr ? VK_DRIVER_ID_IMAGINATION_PROPRIETARY
        : xclipse ? static_cast<VkDriverId>(21) : VK_DRIVER_ID_MESA_TURNIP;
    // VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA is value 25. Keep the numeric fallback so this also builds
    // with Android NDK Vulkan headers that predate that enum name.
    const VkDriverId alternate = powervr ? static_cast<VkDriverId>(25)
        : xclipse ? static_cast<VkDriverId>(3) : VkDriverId{};
    const uint32_t minimum_api = powervr ? VK_API_VERSION_1_1 : VK_API_VERSION_1_3;
    const bool verified = probe_vulkan(reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa), expected, alternate,
                                       minimum_api, powervr, xclipse);
    const bool loaded = module_loaded(library);
    if (!loaded || !verified) {
        nx::log("gpu driver: %s %s; using system driver", library.c_str(),
                loaded ? "failed verification" : "did not load");
        return nullptr;
    }
    nx::log("gpu driver: using %s (%s)", library.c_str(),
            panvk ? "PanVK" : powervr ? "PowerVR" : xclipse ? "Xclipse" : "Turnip");
    return gipa;
}

}  // namespace
#endif

void* vulkan_get_instance_proc_addr() {
    static void* gipa = []() -> void* {
#if defined(NX_CUSTOM_DRIVER)
        if (void* custom = custom_vulkan()) return custom;
#endif
        void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        return lib ? dlsym(lib, "vkGetInstanceProcAddr") : nullptr;
    }();
    return gipa;
}

const char* vulkan_surface_extension() { return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa) {
    ANativeWindow* win = static_cast<ANativeWindow*>(native_window);
    // Remove a previous requested buffer size before measuring the window.
    // Otherwise getWidth/getHeight can report the old 16:9 buffer instead of
    // the device's actual surface, perpetuating compositor stretching.
    if (win) {
        const int reset = ANativeWindow_setBuffersGeometry(win, 0, 0, 0);
        if (reset) nx::log("android: reset buffer geometry -> %d", reset);
    }
    uint32_t rw = 0, rh = 0;
    window_size(rw, rh);
    if (win && rw && rh) {
        int32_t cur_w = ANativeWindow_getWidth(win), cur_h = ANativeWindow_getHeight(win);
        if (cur_w > 0 && cur_h > 0) {
            const auto size = android_surface_size(uint32_t(cur_w), uint32_t(cur_h), rw, rh);
            int r_geom = ANativeWindow_setBuffersGeometry(win, int32_t(size.width), int32_t(size.height), 0);
            nx::log("android: aspect-preserving buffer %ux%u (screen %dx%d, selected %ux%u) -> %d",
                    size.width, size.height, cur_w, cur_h, rw, rh, r_geom);
        }
    }
    auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)(
        static_cast<VkInstance>(instance), "vkCreateAndroidSurfaceKHR"));
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = win;
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create(static_cast<VkInstance>(instance), &ci, nullptr, &s);
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- crash handler and entry ----------------------------------------------------------------------------------

namespace {

struct sigaction g_previous[32];  // the handlers before ours (debuggerd's)

void crash(int sig, siginfo_t* info, void* uc) {
    if ((sig == SIGSEGV || sig == SIGBUS) && nx::plat::write_watch_fault(info->si_addr)) return;  // write_watch.cpp
    nx::log("CRASH: signal %d, fault address %p", sig, info->si_addr);
#if NX_NCE
    // In the game's own code: its registers are in the signal's context, not in the thread's CPU.
    if (nx::self_thread()) {
        static nx::CPU fault;
        uint64_t pc = 0;
        if (nx::nce::guest_fault_context(uc, fault, pc)) {
            nx::log("  in guest code at %s, lr %s, sp %llx", nx::describe(pc).c_str(), nx::describe(fault.x[30]).c_str(),
                    (unsigned long long)fault.x[31]);
            for (int k = 0; k < 32; k += 4)
                nx::log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)fault.x[k],
                        (unsigned long long)fault.x[k + 1], (unsigned long long)fault.x[k + 2], (unsigned long long)fault.x[k + 3]);
            nx::backtrace(fault);
        }
    }
#endif
    if (nx::self_thread()) {
        nx::CPU& c = nx::current_cpu();
        nx::log("  guest lr %s  sp %llx", nx::describe(c.x[30]).c_str(), (unsigned long long)c.x[31]);
        for (int k = 0; k < 32; k += 4)
            nx::log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k],
                    (unsigned long long)c.x[k + 1], (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
        nx::backtrace(c);
        nx::log_recent_files();
    }
    // Hand the signal to the previous handler, Android's debuggerd: it logs the host backtrace (a tombstone).
    const struct sigaction& prev = g_previous[sig];
    if (prev.sa_flags & SA_SIGINFO) {
        if (prev.sa_sigaction) prev.sa_sigaction(sig, info, uc);
    } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler) {
        prev.sa_handler(sig);
    }
    _exit(128 + sig);
}

void game_thread(void*) {
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
}

void request_immersive();  // below: runs on the UI thread

// The panel's refresh while the game shows (Android 11+): 60 Hz, the rate of our vsync event (hle_vi.cpp) and the
// most the game presents. A 90 Hz panel (Lenovo Tab M11) showed a 30 fps frame for 3 refreshes or, when a present
// landed next to a refresh, 2 or 4: uneven motion in films and cutscenes. NX_WINDOW_HZ=<hz>: another rate, 0: leave
// the panel's own.
void request_frame_rate(ANativeWindow* w) {
    // Android 12+: the switch even when it is not seamless (60 <-> 90 Hz on the Tab M11 is not: the plain request
    // was accepted and ignored); Android 11: the plain request.
    using SetFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);
    using SetFrameRateStrategy = int32_t (*)(ANativeWindow*, float, int8_t, int8_t);
    static void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    static const auto set = reinterpret_cast<SetFrameRate>(lib ? dlsym(lib, "ANativeWindow_setFrameRate") : nullptr);
    static const auto set_always = reinterpret_cast<SetFrameRateStrategy>(
        lib ? dlsym(lib, "ANativeWindow_setFrameRateWithChangeStrategy") : nullptr);
    const char* e = getenv("NX_WINDOW_HZ");
    const float hz = e ? float(std::atof(e)) : 60.0f;
    if (!w || hz <= 0) return;
    if (!set && !set_always) return nx::log("android: no ANativeWindow_setFrameRate (Android 10 or older): the panel's own rate");
    int32_t r = set_always ? set_always(w, hz, 1 /* COMPATIBILITY_FIXED_SOURCE */, 1 /* CHANGE_FRAME_RATE_ALWAYS */)
                           : set(w, hz, 1);
    nx::log("android: window frame rate %.0f Hz requested (%d%s)", double(hz), int(r), set_always ? ", even if not seamless" : "");
}

void on_cmd(android_app* app, int32_t cmd) {
    using namespace nx::plat;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            {
                std::lock_guard lock(g_window_lock);
                g_window = app->window;
                ++g_window_gen;
            }
            g_window_ready.notify_all();
            g_visible = true;
            request_frame_rate(app->window);
            request_immersive();
            break;
        case APP_CMD_TERM_WINDOW:  // the window goes away once this returns
            {
                std::lock_guard lock(g_window_lock);
                g_window = nullptr;
            }
            clear_touch_input();
            g_visible = false;
            break;
        case APP_CMD_GAINED_FOCUS:
            g_visible = g_window != nullptr;
            request_immersive();  // bars shown by a swipe hide again
            break;
        case APP_CMD_LOST_FOCUS:
            break;
        case APP_CMD_PAUSE:  // home, app switcher, screen off: the game holds (no window), so does its sound
            clear_touch_input();
            audio_pause(true);
            break;
        case APP_CMD_RESUME:
            audio_pause(false);
            break;
    }
}

}  // namespace

// ---- immersive mode: the status and navigation bars hidden (a swipe from an edge shows them for a moment) --------
// Only the UI thread may touch the window's views; android_main runs on another. Our ANativeActivity_onCreate (on
// the UI thread) registers a pipe with the UI thread's looper: writing a byte to it runs the request there.

extern "C" void glue_ANativeActivity_onCreate(ANativeActivity* activity, void* saved, size_t size);

namespace {

ANativeActivity* g_activity = nullptr;
int g_ui_pipe[2] = {-1, -1};

void hide_system_bars(JNIEnv* env, jobject activity) {
    jclass act = env->GetObjectClass(activity);
    jobject window = env->CallObjectMethod(activity, env->GetMethodID(act, "getWindow", "()Landroid/view/Window;"));
    if (env->ExceptionCheck() || !window) {
        env->ExceptionClear();
        return;
    }
    jclass winc = env->GetObjectClass(window);
    jmethodID get_ctl = env->GetMethodID(winc, "getInsetsController", "()Landroid/view/WindowInsetsController;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    jobject ctl = get_ctl ? env->CallObjectMethod(window, get_ctl) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear(), ctl = nullptr;
    if (ctl) {  // Android 11+
        jclass cc = env->GetObjectClass(ctl);
        jclass types = env->FindClass("android/view/WindowInsets$Type");
        jint bars = env->CallStaticIntMethod(types, env->GetStaticMethodID(types, "systemBars", "()I"));
        env->CallVoidMethod(ctl, env->GetMethodID(cc, "setSystemBarsBehavior", "(I)V"), 2);  // show transient by swipe
        env->CallVoidMethod(ctl, env->GetMethodID(cc, "hide", "(I)V"), bars);
    } else {  // Android 10: immersive sticky | hide navigation | fullscreen | layout stable/hide navigation/fullscreen
        jobject decor = env->CallObjectMethod(window, env->GetMethodID(winc, "getDecorView", "()Landroid/view/View;"));
        if (decor)
            env->CallVoidMethod(decor, env->GetMethodID(env->GetObjectClass(decor), "setSystemUiVisibility", "(I)V"), 0x1706);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

int on_ui_request(int fd, int, void*) {
    char buf[16];
    while (read(fd, buf, sizeof buf) > 0) {
    }
    if (g_activity) hide_system_bars(g_activity->env, g_activity->clazz);
    return 1;
}

void request_immersive() {
    char c = 1;
    if (g_ui_pipe[1] >= 0) (void)write(g_ui_pipe[1], &c, 1);
}

}  // namespace

// Frames presented so far, for the launcher's FPS counter (GameActivity.nativeFrames, in the game's own process).
// RegisterNatives below also keeps this callable when hidden visibility and section GC discard name lookup.
static jlong native_frames(JNIEnv*, jclass) { return jlong(nx::g_frame.load()); }
static jfloatArray native_frame_times(JNIEnv* env, jclass) {
    float samples[nx::FrameHistory::capacity];
    const auto count = jsize(nx::g_frame_history.snapshot(samples));
    jfloatArray result = env->NewFloatArray(count);
    if (result && count) env->SetFloatArrayRegion(result, 0, count, samples);
    return result;
}
extern "C" JNIEXPORT jlong JNICALL Java_io_github_belmantegu_wonderrecomp_GameActivity_nativeFrames(JNIEnv*, jclass) {
    return native_frames(nullptr, nullptr);
}
extern "C" JNIEXPORT jlong JNICALL Java_io_github_p4grecomp_GameActivity_nativeFrames(JNIEnv*, jclass) {
    return native_frames(nullptr, nullptr);
}

extern "C" JNIEXPORT void ANativeActivity_onCreate(ANativeActivity* activity, void* saved, size_t size) {
    g_activity = activity;
    jclass activity_class = activity->env->GetObjectClass(activity->clazz);
    JNINativeMethod frames_method{const_cast<char*>("nativeFrames"), const_cast<char*>("()J"),
                                  reinterpret_cast<void*>(native_frames)};
    if (activity->env->RegisterNatives(activity_class, &frames_method, 1) != JNI_OK &&
        activity->env->ExceptionCheck())
        activity->env->ExceptionClear();
    // Other games sharing the runtime need not implement this optional overlay API.
    if (activity->env->GetStaticMethodID(activity_class, "nativeFrameTimes", "()[F")) {
        JNINativeMethod method{const_cast<char*>("nativeFrameTimes"), const_cast<char*>("()[F"),
                               reinterpret_cast<void*>(native_frame_times)};
        activity->env->RegisterNatives(activity_class, &method, 1);
    }
    if (activity->env->ExceptionCheck()) activity->env->ExceptionClear();
    activity->env->DeleteLocalRef(activity_class);
    if (g_ui_pipe[0] < 0 && pipe2(g_ui_pipe, O_NONBLOCK | O_CLOEXEC) == 0)
        ALooper_addFd(ALooper_forThread(), g_ui_pipe[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, on_ui_request, nullptr);
    glue_ANativeActivity_onCreate(activity, saved, size);
}

// All-files access (Android 11+): needed to read Internal storage/<game title>. Asks for it by opening the system page.
static bool has_all_files_access(android_app* app) {
    // On Android 10 the launcher requests the legacy storage permissions.
    if (android_get_device_api_level() < 30) return true;
    JNIEnv* env = nullptr;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
    jclass envc = env->FindClass("android/os/Environment");
    jmethodID m = envc ? env->GetStaticMethodID(envc, "isExternalStorageManager", "()Z") : nullptr;
    bool ok = m && env->CallStaticBooleanMethod(envc, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (envc) env->DeleteLocalRef(envc);
    return ok;
}

static void request_all_files_access(android_app* app) {
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    // The installed package (a test build may be renamed: android/build_apk.ps1 -TestPackage).
    jclass actc = env->GetObjectClass(app->activity->clazz);
    auto pkg = static_cast<jstring>(env->CallObjectMethod(app->activity->clazz, env->GetMethodID(actc, "getPackageName", "()Ljava/lang/String;")));
    const char* pkg_chars = pkg ? env->GetStringUTFChars(pkg, nullptr) : nullptr;
    const std::string name = pkg_chars ? pkg_chars : nx::game::kInfo.android_package;
    if (pkg_chars) env->ReleaseStringUTFChars(pkg, pkg_chars);
    jclass uric = env->FindClass("android/net/Uri");
    jobject uri = env->CallStaticObjectMethod(uric, env->GetStaticMethodID(uric, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
                                              env->NewStringUTF(("package:" + name).c_str()));
    jclass intentc = env->FindClass("android/content/Intent");
    jobject intent = env->NewObject(intentc, env->GetMethodID(intentc, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"),
                                    env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"), uri);
    env->CallVoidMethod(app->activity->clazz, env->GetMethodID(actc, "startActivity", "(Landroid/content/Intent;)V"), intent);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

#if NX_NCE
static const char* const kDataProbe = "/exefs/main";  // native execution: the player's own exefs
#else
static const char* const kDataProbe = "/analysis/main.bin";
#endif
static bool has_data(const std::string& root) {
    if (std::strcmp(nx::game::kInfo.id, "p4g") == 0 &&
            access((root + "/game/installed.tsv").c_str(), R_OK) == 0 &&
            access((root + "/game/exefs/main").c_str(), R_OK) == 0) return true;
    return access((root + kDataProbe).c_str(), R_OK) == 0;
}

extern "C" void android_main(android_app* app) {
    using namespace nx::plat;
    g_app = app;
    // Game data: Internal storage/<game title> (put there by the player, needs all-files access) or the app's own
    // external files folder (adb push).
    // The folder picked on the home screen (games/<game>/java Home.java writes <files>/data_root.txt) comes first.
    std::string chosen;
    if (app->activity->internalDataPath) {
        std::ifstream f(std::string(app->activity->internalDataPath) + "/data_root.txt");
        std::getline(f, chosen);
        while (!chosen.empty() && (chosen.back() == '\r' || chosen.back() == '\n' || chosen.back() == ' ')) chosen.pop_back();
    }
    const std::string shared = !chosen.empty() ? chosen : std::string("/storage/emulated/0/") + nx::game::kInfo.title;
    const std::string own = app->activity->externalDataPath ? app->activity->externalDataPath : shared;
    if (!has_data(own) && !has_all_files_access(app)) {
        request_all_files_access(app);
        while (!has_all_files_access(app)) {  // keep the app responsive until the switch is turned on
            int events;
            android_poll_source* src;
            while (ALooper_pollOnce(100, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
                if (src) src->process(app, src);
                if (app->destroyRequested) _exit(0);
            }
        }
    }
    // The picked folder wins over the app's own one; else the own one (adb push), else Internal storage/<title>.
    const char* installed = std::getenv("NX_P4G_INSTALLED");
    const bool p4g_installed = std::strcmp(nx::game::kInfo.id, "p4g") == 0 && installed && installed[0] == '1';
    // The installed payload can live in Android/data while this chosen root still holds saves, mods and config.
    g_root = !chosen.empty() && (has_data(chosen) || p4g_installed) ? chosen : has_data(own) ? own : shared;
    // config.txt's NX_* lines become environment variables (the NVN layer reads its switches with getenv), without
    // overriding a variable already set. Android defaults: handheld (720p, the lighter GPU load).
    {
        std::ifstream f(g_root + "/config.txt");
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            auto eq = line.find('=');
            if (line.rfind("NX_", 0) == 0 && eq != std::string::npos) {
                const std::string key = line.substr(0, eq);
                const std::string value = line.substr(eq + 1);
                // Launcher and NativeActivity share a process: update stretch even if a previous launch set it to "1".
                setenv(key.c_str(), value.c_str(), key == "NX_STRETCH" ? 1 : 0);
            }
        }
        // NX_HANDHELD=0 in config.txt (the launcher's TV mode): docked, 1080p. svc_am.cpp tests only whether the variable
        // exists, so "0" must remove it.
        if (const char* h = getenv("NX_HANDHELD"); h && h[0] == '0')
            unsetenv("NX_HANDHELD");
        else
            setenv("NX_HANDHELD", "1", 0);
    }
    // The heap (scudo) zero-fills every allocation and gives freed pages back at once by default: in the replay, a
    // memset under each allocation and madvise churn (docs/nvn/android-perf/PLAN.md, phase 2). NX_MALLOC_TUNE=0: as
    // the system set it.
    if (const char* e = getenv("NX_MALLOC_TUNE"); !(e && e[0] == '0')) {
        int zero = mallopt(M_BIONIC_ZERO_INIT, 0), decay = mallopt(M_DECAY_TIME, 1);
        nx::log("heap: zero-fill off (%d), page release delayed (%d)", zero, decay);
    }
    start_memory_guard();
    gpu_turbo();
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    // A permission wait can process INIT_WINDOW before the callback is installed.
    if (app->window) on_cmd(app, APP_CMD_INIT_WINDOW);
    // NX_SHOW_WHEN_LOCKED=1 (unattended test runs, tools/wonder_phone_course.sh): shown over the lock screen and turning
    // the screen on, as an alarm app; the phone itself stays locked.
    uint32_t flags = AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN;
    if (const char* e = getenv("NX_SHOW_WHEN_LOCKED"); e && e[0] == '1')
        flags |= AWINDOW_FLAG_SHOW_WHEN_LOCKED | AWINDOW_FLAG_TURN_SCREEN_ON;
    ANativeActivity_setWindowFlags(app->activity, flags, 0);

    struct sigaction sa{};
    sa.sa_sigaction = crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &sa, &g_previous[s]);

    nx::log("%s (Switch recomp) on Android, data at %s (own folder %s: %s)", nx::game::kInfo.title, g_root.c_str(),
            own.c_str(), has_data(own) ? "has the data" : kDataProbe);
    static bool started = false;
    for (;;) {
        int events;
        android_poll_source* src;
        while (ALooper_pollOnce(started ? 0 : 50, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) _exit(0);
        }
        if (!started && g_window) {
            started = true;
            start_thread(game_thread, nullptr, 256ull << 20);  // the guest main thread; deep host recursion
        }
        if (started) sleep_ns(2'000'000);
    }
}
