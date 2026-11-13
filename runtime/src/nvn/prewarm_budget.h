#pragma once
#include <cstdint>

namespace nx::nvn {
// Submissions share one budget until the next presented frame.
struct PrewarmBudget {
    uint64_t frame = UINT64_MAX, spent_ns = 0;
    unsigned started = 0;
    void begin(uint64_t current_frame) {
        if (frame == current_frame) return;
        frame = current_frame;
        spent_ns = 0;
        started = 0;
    }
    bool available(uint64_t elapsed_ns, uint64_t limit_ns, unsigned limit_starts) const {
        return started < limit_starts && spent_ns + elapsed_ns < limit_ns;
    }
};
}
