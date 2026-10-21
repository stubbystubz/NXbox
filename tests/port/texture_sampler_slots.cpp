// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <map>
#include <random>
#include <utility>
using u32 = unsigned;
using SamplerId = unsigned;
constexpr unsigned NULL_SAMPLER_ID = 0;
#define LOG_DEBUG(...) ((void)0)
namespace Common {
struct SlotId {
    static constexpr u32 TAGGED_VALUE = 1u << 31;
};
} // namespace Common
struct Table {
    u32 current_limit = 255;
    unsigned reads = 0;
    std::array<unsigned, 256> descriptors{}, previous{};
    std::array<bool, 256> valid{};
    auto Read(int, u32 index) {
        ++reads;
        bool changed = !valid[index] || previous[index] != descriptors[index];
        valid[index] = true;
        previous[index] = descriptors[index];
        return std::pair{descriptors[index], changed};
    }
};
struct CountedMap : std::map<u32, SamplerId> {
    unsigned finds = 0;
    auto find(u32 index) {
        ++finds;
        return std::map<u32, SamplerId>::find(index);
    }
};
struct Channel {
    Table compute_sampler_table, graphics_sampler_table;
    CountedMap sampler_ids;
    // SAMPLER_STORAGE
};
template <class P>
struct TextureCache {
    P runtime{}; // No descriptor-read hook: exercise the shared default path.
    Channel storage;
    Channel* channel_state = &storage;
    int memory = 0;
    int* gpu_memory = &memory;
    bool texture_cache_fastpath = true;
    // SAMPLER_SWITCH
    unsigned resolutions = 0;
    unsigned FindSampler(unsigned descriptor, bool) {
        ++resolutions;
        return descriptor + 1;
    }
    SamplerId GetSamplerId(u32, bool);
};
// METHODS
int main() {
    TextureCache<int> fast;
    fast.texture_cache_fastpath = false; // New paths work without the older opt-in.
    const bool enabled = fast.sampler_slot_fastpath;
    setenv("NXBOX_TEXTURE_SAMPLER_SLOT_FASTPATH", "0", 1);
    TextureCache<int> reference;
    setenv("NXBOX_TEXTURE_SAMPLER_SLOT_FASTPATH", enabled ? "1" : "0", 1);
    reference.texture_cache_fastpath = false;
    assert(fast.GetSamplerId(3, false) == 1);
    for (int i = 0; i < 1000; ++i)
        assert(fast.GetSamplerId(3, false) == 1);
    assert(fast.storage.graphics_sampler_table.reads == 1001);
    assert(fast.storage.sampler_ids.finds == (enabled ? 0 : 1000));
    // Force a direct-map collision. Restore from the canonical map without resolving again.
    fast.GetSamplerId(67, false);
    assert(fast.GetSamplerId(3, false) == 1);
    assert(fast.resolutions == 2);
    std::array<Channel, 3> fast_channels{}, reference_channels{};
    std::mt19937 random{0x53c};
    for (unsigned step = 0; step < 50000; ++step) {
        const auto channel = random() % fast_channels.size();
        fast.channel_state = &fast_channels[channel];
        reference.channel_state = &reference_channels[channel];
        const bool compute = random() % 2;
        const auto index = random() % 260;
        auto& a = compute ? fast.channel_state->compute_sampler_table
                          : fast.channel_state->graphics_sampler_table;
        auto& b = compute ? reference.channel_state->compute_sampler_table
                          : reference.channel_state->graphics_sampler_table;
        if (index < 256 && random() % 5 == 0) {
            a.descriptors[index] = b.descriptors[index] = random() % 99;
        }
        if (random() % 19 == 0) {
            a.valid.fill(false);
            b.valid.fill(false); // Refresh/Invalidate generation.
        }
        assert(fast.GetSamplerId(index, compute) == reference.GetSamplerId(index, compute));
        // Repeated slots also cover content changes with a stable descriptor index.
        assert(fast.GetSamplerId(index, compute) == reference.GetSamplerId(index, compute));
        assert(fast.resolutions - 2 == reference.resolutions);
        assert(a.reads == b.reads);
    }
}
