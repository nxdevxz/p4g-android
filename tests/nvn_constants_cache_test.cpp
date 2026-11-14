// Include the implementation to exercise its private immutable-block cache with a fake upload ring.
#include "nvn/nvn_sets.cpp"
#include <cassert>

// Supply RTTI for UBSan in the discarded texture paths; this test never creates a texture.
namespace nx::nvn { Texture::~Texture() = default; }

namespace nx::nvn::replay {
State g_state;
uint64_t test_generation = 1;
unsigned test_uploads = 0;
bool test_wrap = false;
uint64_t dedup_generation() { return test_generation; }
Upload dedup_upload(const uint8_t*, uint64_t size, uint64_t padded, uint64_t align) {
    assert(size == 0x1000 && padded == size && align == 256);
    ++test_uploads;
    if (test_wrap) { ++test_generation; test_wrap = false; }
    return {reinterpret_cast<VkBuffer>(uintptr_t(1)), uint64_t(test_uploads) * 4096, nullptr};
}
std::array<uint8_t, 0x1000> driver_constants(ShaderStage, int st) {
    std::array<uint8_t, 0x1000> bytes{};
    std::memcpy(bytes.data(), g_state.storage[st], sizeof g_state.storage[st]);
    return bytes;
}
}

int main() {
    using namespace nx::nvn;
    using namespace nx::nvn::replay;
    sets_begin_submit();
    auto* first = constants_block_obj(ShaderStage::Vertex, 0);
    const auto initial = constants_upload(*first);
    assert(constants_block_obj(ShaderStage::Vertex, 0) == first);
    assert(constants_upload(*first).offset == initial.offset && test_uploads == 1);
    g_state.storage[0][3] = {0x1000, 128};
    auto* changed = constants_block_obj(ShaderStage::Vertex, 0);
    assert(changed != first);
    assert(constants_upload(*changed).offset != initial.offset && test_uploads == 2);
    g_state.storage[0][3] = {};
    assert(constants_block_obj(ShaderStage::Vertex, 0) == first);
    assert(constants_upload(*first).offset == initial.offset && test_uploads == 2);
    assert(constants_block_obj(ShaderStage::Compute, 5) == nullptr);
    assert(constants_block_obj(ShaderStage::Fragment, 1) != first);
    ++test_generation; // ring reset within a submit must discard the saved upload
    assert(constants_upload(*first).offset != initial.offset && test_uploads == 3);
    ++test_generation; test_wrap = true;
    constants_upload(*first); // allocation itself resets the ring
    constants_upload(*first);
    assert(test_uploads == 4);
    sets_begin_submit(); // recycled block bytes get a fresh upload even with unchanged keys
    constants_upload(*constants_block_obj(ShaderStage::Vertex, 0));
    assert(test_uploads == 5);
    for (uint64_t i = 1; i < 256; ++i) {
        g_state.storage[0][0] = {i * 256, 256};
        assert(constants_block_obj(ShaderStage::Vertex, 0));
    }
    g_state.storage[0][0] = {0x100000, 256};
    assert(constants_block_obj(ShaderStage::Vertex, 0) == nullptr); // bounded fallback
}
