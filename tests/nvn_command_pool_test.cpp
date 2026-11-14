// Exercise the actual recorder's private pool without registering guest entry points.
#include "nvn/nvn.h"
#undef NX_HLE
#define NX_HLE(...)
#include "nvn/nvn_cmd.cpp"
#include <cassert>

int main() {
    using namespace nx::nvn;
    auto original = alloc_cmdlist();
    original->cmds.push_back(Cmd{Cmd::DrawArrays});
    const uint8_t bytes[] = {4, 5, 6};
    original->append_blob(bytes, sizeof bytes);
    original->handle = 42; original->epoch = 7; original->skinned = 2;
    original->scanned_size = 1; original->own_dispatches = 1; original->calls.push_back(99);
    const auto* identity = original.get();
    const size_t capacity = original->blob_bytes.capacity();
    auto in_flight = original;
    recycle_cmdlist(std::move(original));
    auto other = alloc_cmdlist();
    assert(other.get() != identity); // a replay still owns the retired list
    assert(in_flight->cmds.size() == 1 && in_flight->handle == 42);
    assert(std::memcmp(in_flight->blob_ptr(0), bytes, sizeof bytes) == 0);
    in_flight.reset();
    auto reused = alloc_cmdlist();
    assert(reused.get() == identity);
    assert(reused->cmds.empty() && reused->blobs.empty() && reused->blob_bytes.empty());
    assert(reused->blob_bytes.capacity() == capacity);
    assert(reused->handle == 0 && reused->epoch == 0 && reused->skinned == 0);
    assert(reused->scanned_size == SIZE_MAX && reused->own_dispatches == 0 && reused->calls.empty());
    // Bound the number of retained lists even when many are still being replayed.
    std::vector<std::shared_ptr<CmdList>> retained;
    for (int i = 0; i < 100; ++i) {
        auto list = alloc_cmdlist();
        retained.push_back(list);
        recycle_cmdlist(std::move(list));
    }
    assert(g_cmdlist_pool.size() <= 64);
}
