#include <cassert>
#include <cstring>
#include "nvn/nvn.h"

int main() {
    nx::nvn::CmdList list;
    const uint8_t inline_update[] = {1, 2, 3};
    const uint32_t first = list.append_blob(inline_update, sizeof inline_update);
    nx::nvn::VertexAttrib attrib{7, 12, 2};
    const uint32_t second = list.append_blob(&attrib, sizeof attrib);
    // Force arena reallocations; indices and aligned typed reads must survive.
    for (int i = 0; i < 1024; ++i) list.append_blob(inline_update, sizeof inline_update);
    assert(list.blob_size(first) == 3);
    assert(std::memcmp(list.blob_ptr(first, 3), inline_update, 3) == 0);
    const uint8_t* data = list.blob_ptr(second, sizeof attrib);
    assert(data && reinterpret_cast<uintptr_t>(data) % alignof(nx::nvn::VertexAttrib) == 0);
    const auto* actual = reinterpret_cast<const nx::nvn::VertexAttrib*>(data);
    assert(actual->format == 7 && actual->offset == 12 && actual->stream == 2);
    assert(list.blob_ptr(second, sizeof attrib + 1) == nullptr);
    assert(list.blob_ptr(UINT32_MAX) == nullptr && list.blob_size(UINT32_MAX) == 0);
    bool rejected = false;
    try { list.append_blob(&attrib, UINT64_C(1) << 32); }
    catch (const std::length_error&) { rejected = true; }
    assert(rejected);
    const size_t capacity = list.blob_bytes.capacity();
    list.blobs.clear(); list.blob_bytes.clear();
    assert(list.blob_ptr(0) == nullptr && list.blob_bytes.capacity() == capacity);
    const uint32_t reused = list.append_blob(&attrib, sizeof attrib);
    assert(reused == 0 && std::memcmp(list.blob_ptr(0), &attrib, sizeof attrib) == 0);
}
