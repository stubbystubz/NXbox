// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <utility>
#include <vector>
using u32 = unsigned;
using u64 = unsigned long long;
using ImageViewId = unsigned;
constexpr ImageViewId NULL_IMAGE_VIEW_ID = 0;
namespace Common {
struct SlotId {
    static constexpr u32 TAGGED_VALUE = 1u << 31;
};
} // namespace Common
#define LOG_DEBUG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
enum class ImageFlagBits { CpuModified = 1, GpuModified = 2, Tracked = 4 };
constexpr int operator&(int flags, ImageFlagBits bit) {
    return flags & static_cast<int>(bit);
}
bool False(int flags) {
    return flags == 0;
}
struct Image {
    int flags = 4;
    std::vector<int> aliased_images;
    int lru_index = 0;
    u64 alias_sync_frame_tick = std::numeric_limits<u64>::max();
    u64 alias_sync_generation = 0;
};
struct ImageViewBase {
    unsigned image_id = 0;
    bool buffer = false;
    u64 prepare_frame_tick = std::numeric_limits<u64>::max();
    bool IsBuffer() const {
        return buffer;
    }
};
struct Descriptor {
    int value = 0;
    int Address() const {
        return value;
    }
};
struct Table {
    u32 current_limit = 7;
    bool changed = true;
    int reads = 0;
    int descriptor_value = 1;
    std::pair<Descriptor, bool> Read(int, u32) {
        ++reads;
        return {{descriptor_value}, std::exchange(changed, false)};
    }
};
struct CountedMap : std::map<u32, ImageViewId> {
    int finds = 0;
    auto find(u32 key) {
        ++finds;
        return std::map<u32, ImageViewId>::find(key);
    }
};
struct Channel {
    Table graphics_image_table, compute_image_table;
    CountedMap image_view_ids;
    std::map<int, ImageViewId> image_views;
    std::array<std::vector<ImageViewId>, 2> indexed_image_view_ids;
};
bool valid_entry = true;
bool IsValidEntry(int, Descriptor) {
    return valid_entry;
}
// The real descriptor map keys are TICEntry; this host model uses its value.
struct DescriptorMap : std::map<int, ImageViewId> {
    auto try_emplace(Descriptor descriptor) {
        return std::map<int, ImageViewId>::try_emplace(descriptor.value);
    }
};
struct TestChannel : Channel {
    DescriptorMap image_views;
};
template <class P>
struct TextureCache {
    P runtime{}; // No descriptor-read hook: exercise the shared default path.
    TestChannel storage;
    TestChannel* channel_state = &storage;
    int memory = 0;
    int* gpu_memory = &memory;
    bool texture_cache_fastpath = true;
    bool alias_sync_fastpath = true;
    u64 alias_sync_generation = 1;
    u64 frame_tick = 0;
    std::array<ImageViewBase, 4> slot_image_views{};
    std::array<Image, 4> slot_images{};
    int prepares = 0, modifications = 0;
    struct Lru {
        int touches = 0;
        void Touch(int, u64) {
            ++touches;
        }
    } lru_cache;
    ImageViewId CreateImageView(Descriptor descriptor) {
        return descriptor.value;
    }
    void PrepareImage(unsigned, bool modification, bool) {
        ++prepares;
        modifications += modification;
    }
    void MarkModification(Image&) {
        ++modifications;
    }
    ImageViewId VisitImageView(u32, bool);
    void PrepareImageView(ImageViewId, bool, bool);
};
// PRODUCTION_METHODS
int main(int argc, char**) {
    TextureCache<int> cache;
    if (argc > 1) {
        cache.PrepareImageView(1, false, false);
        assert(cache.prepares == 1); // The force-refresh diagnostic overrides the fast path.
        return 0;
    }
    assert(cache.VisitImageView(2, false) == 1);
    const int cold_prepares = cache.prepares;
    for (int i = 0; i < 100; ++i)
        assert(cache.VisitImageView(2, false) == 1);
    assert(cache.storage.graphics_image_table.reads == 101); // Guest writes must remain visible.
    assert(cache.storage.image_view_ids.finds == 0);
    assert(cache.prepares == cold_prepares);
    assert(cache.lru_cache.touches == 1);
    cache.PrepareImageView(1, true, false);
    cache.PrepareImageView(1, true, false);
    assert(cache.modifications == 2); // Never skip the draw's modification tick.
    cache.slot_images[0].flags = 1;
    cache.PrepareImageView(1, false, false);
    assert(cache.prepares == cold_prepares + 1);
    cache.slot_images[0].flags = 4;
    cache.slot_images[0].aliased_images.push_back(1);
    cache.PrepareImageView(1, true, false);
    assert(cache.prepares == cold_prepares + 2);
    cache.slot_images[0].aliased_images.clear();
    cache.PrepareImageView(1, true, true);
    assert(cache.prepares == cold_prepares + 3);
    cache.slot_images[0].flags = 0; // Untracked images need the original path.
    cache.PrepareImageView(1, false, false);
    assert(cache.prepares == cold_prepares + 4);
    cache.slot_images[0].flags = 4;
    ++cache.frame_tick;
    cache.PrepareImageView(1, false, false);
    assert(cache.lru_cache.touches == 2);
    cache.texture_cache_fastpath = false;
    cache.VisitImageView(2, false);
    assert(cache.storage.image_view_ids.finds == 1);
    assert(cache.prepares == cold_prepares + 5);
    cache.texture_cache_fastpath = true;
    cache.storage.graphics_image_table.changed = true; // Deletion/rescale invalidates table reads.
    cache.storage.image_views.clear();
    cache.storage.graphics_image_table.descriptor_value = 2;
    assert(cache.VisitImageView(2, false) == 2);
    assert(cache.VisitImageView(2, false) == 2);
    valid_entry = false;
    cache.storage.graphics_image_table.changed = true;
    assert(cache.VisitImageView(2, false) == 0);
    assert(cache.VisitImageView(2, false) == 0);
    valid_entry = true;
    cache.VisitImageView(2, true); // Compute has its own index namespace.
    assert(cache.storage.compute_image_table.reads == 1);
    assert(cache.VisitImageView(8, false) == 0);
    cache.slot_image_views[1].buffer = true;
    const int before_buffer = cache.prepares;
    cache.PrepareImageView(1, false, false);
    cache.PrepareImageView(0, false, false);
    assert(cache.prepares == before_buffer);
}
