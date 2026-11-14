#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nx::nvn {
// Adapted from Chakeyy/p5r-android 5f17826. OpLoopMerge is 246, not OpTypeQueue (37).
// Preserve optional loop operands and every instruction other than debug lines/Nops.
inline bool optimize_spirv_mali(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5 || in[0] != 0x07230203) return false;
    std::vector<uint32_t> result(in.begin(), in.begin() + 5);
    result.reserve(in.size());
    bool modified = false;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16, op = in[i] & 0xffff;
        if (!len || len > in.size() - i || (op == 246 && len < 4)) return false;
        if (op == 0 || op == 8 || op == 317) {
            modified = true;
        } else {
            const size_t at = result.size();
            result.insert(result.end(), in.begin() + i, in.begin() + i + len);
            if (op == 246) {
                const uint32_t control = (in[i + 3] & ~1u) | 2u;
                modified |= control != in[i + 3];
                result[at + 3] = control;
            }
        }
        i += len;
    }
    if (modified) out.swap(result);
    return modified;
}
}  // namespace nx::nvn
