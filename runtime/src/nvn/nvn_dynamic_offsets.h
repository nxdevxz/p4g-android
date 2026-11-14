#pragma once

#include <cstdint>

namespace nx::nvn::replay {

// Dynamic UBOs are in set 0, dynamic SSBOs in set 1. Vulkan requires one offset per
// dynamic binding of each bound set, including bindings absent from the current draw.
struct DynamicOffsets {
    uint32_t values[128] = {};
    uint32_t begin[5] = {};

    DynamicOffsets(uint64_t ubo_layout, uint64_t ubo_written, const uint32_t* ubo,
                   uint64_t ssbo_layout, uint64_t ssbo_written, const uint32_t* ssbo) {
        uint32_t used = 0;
        auto append = [&](uint64_t layout, uint64_t written, const uint32_t* offsets) {
            for (; layout; layout &= layout - 1) {
                const uint32_t binding = uint32_t(__builtin_ctzll(layout));
                values[used++] = (written >> binding & 1) ? offsets[binding] : 0;
            }
        };
        append(ubo_layout, ubo_written, ubo);
        begin[1] = used;
        append(ssbo_layout, ssbo_written, ssbo);
        begin[2] = begin[3] = begin[4] = used;
    }

    uint32_t count(uint32_t first, uint32_t sets) const { return begin[first + sets] - begin[first]; }
    const uint32_t* data(uint32_t first) const { return values + begin[first]; }
};

}  // namespace nx::nvn::replay
