#pragma once
#include <cstdint>
// Host-only AAudio adapter: no Android device or timing emulation.
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
using aaudio_result_t = int;
using aaudio_data_callback_result_t = int;
constexpr int AAUDIO_OK = 0, AAUDIO_ERROR_DISCONNECTED = -899;
constexpr int AAUDIO_CALLBACK_RESULT_CONTINUE = 0, AAUDIO_FORMAT_PCM_I16 = 1;
constexpr int AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12, AAUDIO_STREAM_STATE_DISCONNECTED = 13;
struct AAudioStream { int state = 0; };
struct AAudioStreamBuilder {};
inline int opens = 0, closes = 0, starts = 0, pauses = 0;
inline bool fail_open = false;
inline int AAudio_createStreamBuilder(AAudioStreamBuilder** b) { *b = new AAudioStreamBuilder; return AAUDIO_OK; }
inline void AAudioStreamBuilder_delete(AAudioStreamBuilder* b) { delete b; }
inline void AAudioStreamBuilder_setFormat(AAudioStreamBuilder*, int) {}
inline void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder*, int) {}
inline void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder*, int) {}
inline void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder*, int) {}
inline void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder*, int (*)(AAudioStream*, void*, void*, int32_t), void*) {}
inline void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder*, void (*)(AAudioStream*, void*, int), void*) {}
inline int AAudioStreamBuilder_openStream(AAudioStreamBuilder*, AAudioStream** stream) {
    ++opens;
    if (fail_open) return -1;
    *stream = new AAudioStream;
    return AAUDIO_OK;
}
inline int AAudioStream_close(AAudioStream* stream) { ++closes; delete stream; return AAUDIO_OK; }
inline int AAudioStream_getFramesPerBurst(AAudioStream*) { return 192; }
inline int AAudioStream_getBufferSizeInFrames(AAudioStream*) { return 384; }
inline int AAudioStream_getBufferCapacityInFrames(AAudioStream*) { return 768; }
inline int AAudioStream_getPerformanceMode(AAudioStream*) { return AAUDIO_PERFORMANCE_MODE_LOW_LATENCY; }
inline int AAudioStream_getState(AAudioStream* stream) { return stream->state; }
inline int AAudioStream_requestStart(AAudioStream*) { ++starts; return AAUDIO_OK; }
inline int AAudioStream_requestPause(AAudioStream*) { ++pauses; return AAUDIO_OK; }
inline int AAudioStream_getTimestamp(AAudioStream*, int, int64_t*, int64_t*) { return -1; }
