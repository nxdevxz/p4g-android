#include "nvn/nvn_buffer_cache.cpp"
#include "nvn/nvn_sampler.cpp"
#include <cassert>

namespace nx { void log(const char*, ...) {} }
namespace nx::plat { uint64_t monotonic_ns() { return 0; } }
namespace nx::vk {
PFN_vkCreateBuffer p_vkCreateBuffer{};
PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements{};
PFN_vkAllocateMemory p_vkAllocateMemory{};
PFN_vkBindBufferMemory p_vkBindBufferMemory{};
PFN_vkMapMemory p_vkMapMemory{};
PFN_vkDestroyBuffer p_vkDestroyBuffer{};
PFN_vkFreeMemory p_vkFreeMemory{};
}
namespace nx::nvn {
Texture::~Texture() = default;
VkDevice vk_device() { return VK_NULL_HANDLE; }
uint32_t upload_memory_type(uint32_t) { return 0; }
uint64_t frame_recording_serial() { return 10; }
uint8_t* pending = nullptr;
void frame_guest_read(const uint8_t*, uint64_t) {
    if (pending) { *pending = 77; pending = nullptr; }
}
}
namespace nx::nvn::replay { UploadKind g_upload_kind = kUploadVertex; }
int main() {
    using namespace nx::nvn;
    using namespace nx::nvn::replay;
    setenv("NX_BUFFER_CACHE", "1", 1);
    std::vector<uint8_t> bytes(8193, 1), resident(8448, 1);
    const uint8_t* p = bytes.data();
    Block block; block.buffer = reinterpret_cast<VkBuffer>(uintptr_t(1)); block.ptr = resident.data();
    g_blocks.push_back(block);
    auto seed = [&] {
        const uint64_t h = full_hash_memory(p, bytes.size());
        nx::nvn::replay::g_buffer_entries[p] = {bytes.size(), resident.size(), h, 0, 0, 6, 20, 10};
        g_page_churn[page_churn_index(p)].cached_count = 1;
    };
    seed(); g_submits = 10;
    Upload out;
    assert(cache_upload(p, bytes.size(), resident.size(), 256, out));
    assert(out.ptr == resident.data() && nx::nvn::replay::g_buffer_entries[p].serial == 20);
    // Change outside the upstream sample windows within the same submit/frame.
    bytes[2048] = 2;
    assert(!cache_upload(p, bytes.size(), resident.size(), 256, out));
    assert(nx::nvn::replay::g_buffer_entries.empty() && g_retired.size() == 1 && g_retired[0].serial == 20);
    bytes[2048] = 1; seed(); ++g_entry_front_gen;
    pending = bytes.data() + bytes.size() - 1; // deferred GPU result changes a tail byte before hash
    assert(!cache_upload(p, bytes.size(), resident.size(), 256, out));
    assert(!pending && nx::nvn::replay::g_buffer_entries.empty());
    // Unaligned input hashes have the same value as aligned copies.
    std::vector<uint8_t> unaligned(65, 3), aligned(64, 3);
    assert(hash_memory(unaligned.data() + 1, 64) == hash_memory(aligned.data(), 64));
}
