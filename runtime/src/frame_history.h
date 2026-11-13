#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace nx {
// Intervals between host presentations, including stalls; not GPU execution time.
class FrameHistory {
public:
    static constexpr size_t capacity = 120;
    void record(uint64_t now_ns) {
        std::lock_guard lock(mutex);
        if (previous && now_ns > previous) {
            samples[next] = float(now_ns - previous) / 1000000.0f;
            next = (next + 1) % capacity;
            if (count < capacity) ++count;
        }
        previous = now_ns;
    }
    size_t snapshot(float* out) {
        std::lock_guard lock(mutex);
        for (size_t i = 0; i < count; ++i) out[i] = samples[(next + capacity - count + i) % capacity];
        return count;
    }
private:
    std::mutex mutex;
    std::array<float, capacity> samples{};
    size_t next = 0, count = 0;
    uint64_t previous = 0;
};
inline FrameHistory g_frame_history;
}
