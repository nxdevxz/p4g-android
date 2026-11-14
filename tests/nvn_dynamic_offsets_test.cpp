#include <cassert>
#include "nvn/nvn_dynamic_offsets.h"

using nx::nvn::replay::DynamicOffsets;
int main() {
    uint32_t ubo[64]{}, ssbo[64]{};
    ubo[2] = 256; ubo[63] = 512;
    ssbo[0] = 768; ssbo[5] = 1024;
    DynamicOffsets d((1ull << 2) | (1ull << 63), ~0ull, ubo,
                     (1ull << 0) | (1ull << 3) | (1ull << 5), (1ull << 0) | (1ull << 5), ssbo);
    assert(d.count(0, 4) == 5 && d.count(0, 2) == 5);
    assert(d.data(0)[0] == 256 && d.data(0)[1] == 512);
    assert(d.count(0, 1) == 2); // gap after set 0: do not supply SSBO offsets yet
    assert(d.count(1, 1) == 3 && d.count(1, 3) == 3);
    assert(d.data(1)[0] == 768 && d.data(1)[1] == 0 && d.data(1)[2] == 1024);
    assert(d.count(2, 2) == 0 && d.count(3, 1) == 0);
    DynamicOffsets only_ssbo(0, 0, ubo, 1, 1, ssbo);
    assert(only_ssbo.count(0, 1) == 0 && only_ssbo.count(1, 1) == 1);
    assert(only_ssbo.data(1)[0] == 768);
    DynamicOffsets empty(0, 0, ubo, 0, 0, ssbo);
    assert(empty.count(0, 4) == 0);
    DynamicOffsets full(~0ull, ~0ull, ubo, ~0ull, ~0ull, ssbo);
    assert(full.count(0, 4) == 128 && full.count(1, 1) == 64);
}
