#include "../runtime/src/platform_android_audio.cpp"
#include <cassert>

static int logs = 0;
namespace nx { void log(const char*, ...) { ++logs; } }
namespace nx::plat {
uint64_t monotonic_ns() { return 1000000000ull; }
bool headless() { return false; }
}

int main() {
    using namespace nx::plat;
    uint32_t input[512]{};
    assert(!audio_submit(input, sizeof input, nullptr));
    assert(opens == 0); // disabled/muted output must not be opened by submit
    assert(audio_stream_create());
    g_enabled = true;
    g_read = 100;
    g_written = 612;
    g_device = 96000;
    g_latency_ns = 50000000;
    g_stream->state = AAUDIO_STREAM_STATE_DISCONNECTED;
    audio_reopen_if_disconnected();
    assert(opens == 2 && closes == 1);
    assert(g_device == 0 && audio_latency_ns() == 0);
    assert(g_read == 100 && g_written == 612); // preserve ring and pending completion positions
    audio_pause(true);
    g_stream->state = AAUDIO_STREAM_STATE_DISCONNECTED;
    audio_reopen_if_disconnected();
    assert(opens == 2 && pauses == 1);
    audio_pause(false);
    assert(opens == 3 && closes == 2);
    g_stream->state = AAUDIO_STREAM_STATE_DISCONNECTED;
    fail_open = true;
    audio_reopen_if_disconnected();
    assert(g_stream == nullptr);
    fail_open = false;
    audio_reopen_if_disconnected();
    assert(g_stream != nullptr && opens == 5);
    input[0] = 0x12345678;
    write_audio_ring(g_ring.data(), kRingFrames, g_read, input, 512);
    uint32_t out[32]{};
    int before = logs;
    audio_cb(g_stream, nullptr, out, 32);
    assert(out[0] == input[0] && g_device == 32 && g_read == 132);
    assert(logs == before); // callback collects statistics but never logs
    audio_error_cb(g_stream, nullptr, AAUDIO_ERROR_DISCONNECTED);
    assert(logs == before && g_disconnected);
    audio_reopen_if_disconnected();
    assert(g_device == 0);
    AAudioStream_close(g_stream);
    g_stream = nullptr;
}
