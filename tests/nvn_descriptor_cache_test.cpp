#include "nvn/nvn_sets.cpp"
#include <cassert>
namespace nx::nvn {
Texture::~Texture() = default;
unsigned allocations = 0;
VkDescriptorSet descriptor_set(VkDescriptorSetLayout) {
    return reinterpret_cast<VkDescriptorSet>(uintptr_t(++allocations));
}
}
int main() {
    using namespace nx::nvn;
    using namespace nx::nvn::replay;
    ProgramLayout layout;
    for (int k = 0; k < 4; ++k) layout.sets[k] = bits_handle<VkDescriptorSetLayout>(k + 1);
    auto clear = [] {
        g_sets.writes.clear(); g_sets.buffers.clear(); g_sets.images.clear();
    };
    sets_begin_submit();
    g_sets.keys[0] = {{2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 10, 0, 4096}};
    g_sets.keys[1] = {{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 11, 0, 65536}};
    g_sets.keys[2] = {{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12, 13, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    g_sets.keys[3] = {{5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 14, 0, VK_IMAGE_LAYOUT_GENERAL}};
    resolve_sets(layout);
    assert(allocations == 4 && g_sets.writes.size() == 4);
    assert(g_sets.writes[1].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
    assert(g_sets.writes[1].pBufferInfo->buffer == bits_handle<VkBuffer>(11));
    assert(g_sets.writes[1].pBufferInfo->range == 65536);
    assert(g_sets.writes[2].pImageInfo->sampler == bits_handle<VkSampler>(13));
    assert(g_sets.writes[3].pImageInfo->imageLayout == VK_IMAGE_LAYOUT_GENERAL);
    const auto original = g_sets.sets[2];
    clear(); resolve_sets(layout);
    assert(allocations == 4 && g_sets.writes.empty() && g_sets.buffers.empty() && g_sets.images.empty());
    g_sets.keys[2][0].a = 99; // changed view invalidates only the texture set
    clear(); resolve_sets(layout);
    assert(allocations == 5 && g_sets.writes.size() == 1 && g_sets.sets[2] != original);
    g_sets.keys[2][0].a = 12; // hash-cache hit after the last-set fast path misses
    clear(); resolve_sets(layout);
    assert(allocations == 5 && g_sets.writes.empty() && g_sets.sets[2] == original);
    layout.sets[2] = bits_handle<VkDescriptorSetLayout>(55);
    clear(); resolve_sets(layout);
    assert(allocations == 6 && g_sets.writes.size() == 1);
    sets_begin_submit(); clear(); resolve_sets(layout);
    assert(allocations == 10 && g_sets.writes.size() == 4);
    g_sets.keys[0].clear();
    clear(); resolve_sets(layout);
    assert(!g_sets.sets[0] && allocations == 10 && g_sets.writes.empty());
    // More writes than the old fixed reservation: all stored pointers must remain valid.
    sets_begin_submit(); clear();
    for (auto& key : g_sets.keys) key.clear();
    for (uint32_t i = 0; i < 1100; ++i)
        g_sets.keys[0].push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10, uint64_t(i) * 256, 256});
    resolve_sets(layout);
    assert(g_sets.writes.size() == 1100);
    for (uint32_t i = 0; i < 1100; ++i) assert(g_sets.writes[i].pBufferInfo->offset == uint64_t(i) * 256);
}
