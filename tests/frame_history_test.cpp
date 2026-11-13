#include "frame_history.h"
#include <cassert>
#include <cmath>
int main() {
    nx::FrameHistory history;
    float samples[nx::FrameHistory::capacity];
    assert(history.snapshot(samples) == 0);
    uint64_t now = 1000000;
    history.record(now);
    assert(history.snapshot(samples) == 0);
    for (unsigned i = 1; i <= 150; ++i) history.record(now += i * 1000000ull);
    assert(history.snapshot(samples) == 120);
    assert(std::abs(samples[0] - 31.0f) < 0.001f);
    assert(std::abs(samples[119] - 150.0f) < 0.001f);
    history.record(now += 3000000000ull);
    history.snapshot(samples);
    assert(samples[119] == 3000.0f); // long stalls are not averaged away
}
