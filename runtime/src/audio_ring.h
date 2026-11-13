#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace nx::plat {
inline void read_audio_ring(uint32_t* dst, const uint32_t* ring, size_t capacity, uint64_t position, size_t count) {
    assert(capacity && count <= capacity);
    const size_t offset = position % capacity, first = std::min(count, capacity - offset);
    if (!count) return;
    std::memcpy(dst, ring + offset, first * sizeof(uint32_t));
    std::memcpy(dst + first, ring, (count - first) * sizeof(uint32_t));
}
inline void write_audio_ring(uint32_t* ring, size_t capacity, uint64_t position, const void* data, size_t count) {
    assert(capacity && count <= capacity);
    const size_t offset = position % capacity, first = std::min(count, capacity - offset);
    if (!count) return;
    const auto* src = static_cast<const uint8_t*>(data);
    std::memcpy(ring + offset, src, first * sizeof(uint32_t));
    std::memcpy(ring, src + first * sizeof(uint32_t), (count - first) * sizeof(uint32_t));
}
}
