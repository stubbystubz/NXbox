// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <bit>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <tuple>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ImageViewId = unsigned;
using PixelFormat = unsigned;
constexpr unsigned NUM_RT = 8;
namespace Common::GpuProfile {
enum class Phase { TextureCacheUpdate };
struct PhaseScope {
    explicit PhaseScope(Phase) {}
};
} // namespace Common::GpuProfile
namespace Tegra::Engines {
struct Maxwell3D {
    struct Regs {
        enum class SamplerBinding { Independent, ViaHeaderBinding };
        struct Table {
            u64 address{};
            u32 limit{};
            u64 Address() const {
                return address;
            }
        } tex_header, tex_sampler;
        SamplerBinding sampler_binding{};
    } regs;
    struct {
        std::bitset<16> flags;
    } dirty;
};
} // namespace Tegra::Engines
namespace VideoCommon::Dirty {
enum { Descriptors, RenderTargets };
}
namespace Dirty = VideoCommon::Dirty;
namespace PassDump {
struct Config {
    bool enabled{};
} config;
const Config& GetConfig() {
    return config;
}
} // namespace PassDump
struct Table {
    u64 address = ~u64{};
    u32 limit{};
    unsigned calls{}, invalidations{};
    bool Synchronize(u64 a, u32 l) {
        ++calls;
        const bool changed = address != a || limit != l;
        if (changed) {
            address = a;
            limit = l;
            ++invalidations;
        }
        return changed;
    }
};
struct Channel {
    Table compute_sampler_table, compute_image_table, graphics_sampler_table, graphics_image_table;
    bool graphics_descriptors_initialized{}, graphics_linked_tsc{};
};
struct Compute {
    struct {
        bool linked_tsc{};
    } launch_description;
    struct {
        Tegra::Engines::Maxwell3D::Regs::Table tic, tsc;
    } regs;
};
enum ImageFlagBits : unsigned { Rescaled = 1, CpuModified = 2 };
constexpr bool True(unsigned flags) {
    return flags != 0;
}
constexpr unsigned NULL_IMAGE_VIEW_ID = 0;
struct ImageViewInOut {
    unsigned index;
    bool blacklist;
    unsigned id{};
    std::optional<PixelFormat> format_override{};
};
struct ImageViewBase {
    unsigned image_id;
};
struct Image {
    unsigned flags{}, scale_rating{};
    bool operator==(const Image&) const = default;
};
struct Cache {
    struct Runtime {
        bool CanViewFormat(const ImageViewBase&, PixelFormat) {
            assert(false); // This differential trace has no format overrides.
            return false;
        }
    } runtime;
    ImageViewId ReinterpretImageView(ImageViewId, PixelFormat) {
        assert(false);
        return NULL_IMAGE_VIEW_ID;
    }
    bool draw_state_fastpath = true;
    Tegra::Engines::Maxwell3D* maxwell3d{};
    Compute* kepler_compute{};
    Channel* channel_state{};
    u64 texture_bindings_serial{};
    struct {
        std::array<ImageViewId, NUM_RT> color_buffer_ids{};
        ImageViewId depth_buffer_id{};
    } render_targets;
    u32 rt_active_mask{};
    unsigned dirty_updates{}, dumps{};
    std::vector<std::tuple<unsigned, bool, bool>> preparations;
    // Stand-ins retain ordering and multiplicity, including two views of the same image.
    void PrepareImageView(unsigned id, bool modification, bool invalidate) {
        if (id)
            preparations.emplace_back(id, modification, invalidate);
    }
    bool IsFullClear(unsigned id) {
        return id % 3 == 0;
    }
    void UpdatePassDumpRenderTargets() {
        ++dumps;
    }
    std::array<ImageViewBase, 16> slot_image_views{};
    std::array<Image, 16> slot_images{};
    std::vector<unsigned> reads, scale_effects;
    bool has_deleted_images{};
    unsigned VisitImageView(unsigned index, bool) {
        reads.push_back(index);
        return index;
    }
    bool ScaleDown(Image& image) {
        if (!(image.flags & Rescaled))
            return false;
        scale_effects.push_back(static_cast<unsigned>(&image - slot_images.data()));
        image.flags &= ~Rescaled;
        return true;
    }
    void FillImageViews(std::span<ImageViewInOut>, bool, bool);
    void SynchronizeDescriptors(bool);
    void UpdateRenderTargets(bool);
};
// METHODS
void legacy_sync(Cache& c, bool compute) {
    auto& s = *c.channel_state;
    u64 sampler_address, image_address;
    u32 sampler_limit, image_limit;
    if (compute) {
        auto& e = *c.kepler_compute;
        image_address = e.regs.tic.Address();
        sampler_address = e.regs.tsc.Address();
        image_limit = e.regs.tic.limit;
        sampler_limit = e.launch_description.linked_tsc ? image_limit : e.regs.tsc.limit;
    } else {
        auto& e = *c.maxwell3d;
        image_address = e.regs.tex_header.Address();
        sampler_address = e.regs.tex_sampler.Address();
        image_limit = e.regs.tex_header.limit;
        sampler_limit = e.regs.sampler_binding ==
                                Tegra::Engines::Maxwell3D::Regs::SamplerBinding::ViaHeaderBinding
                            ? image_limit
                            : e.regs.tex_sampler.limit;
    }
    const bool a = (compute ? s.compute_sampler_table : s.graphics_sampler_table)
                       .Synchronize(sampler_address, sampler_limit);
    const bool b = (compute ? s.compute_image_table : s.graphics_image_table)
                       .Synchronize(image_address, image_limit);
    if (a || b)
        ++c.texture_bindings_serial;
}
void legacy_update(Cache& c, bool clear) {
    if (c.maxwell3d->dirty.flags[Dirty::RenderTargets]) {
        ++c.dirty_updates;
        return;
    }
    if (PassDump::GetConfig().enabled)
        c.UpdatePassDumpRenderTargets();
    for (auto id : c.render_targets.color_buffer_ids)
        c.PrepareImageView(id, true, clear && c.IsFullClear(id));
    const auto id = c.render_targets.depth_buffer_id;
    c.PrepareImageView(id, true, clear && c.IsFullClear(id));
}
void legacy_fill(Cache& c, std::span<ImageViewInOut> views, bool compute, bool blacklist) {
    bool rescaled;
    do {
        rescaled = false;
        for (auto& view : views) {
            view.id = c.VisitImageView(view.index, compute);
            if (blacklist && view.blacklist && view.id) {
                auto& image = c.slot_images[c.slot_image_views[view.id].image_id];
                rescaled |= c.ScaleDown(image);
                image.scale_rating = 0;
            }
        }
    } while (rescaled);
}
int main() {
    std::mt19937 random{0x01234567};
    Compute compute;
    std::array<Tegra::Engines::Maxwell3D, 3> engines{};
    std::array<Channel, 3> fast_channels{}, old_channels{};
    Cache fast, old;
    fast.kepler_compute = old.kepler_compute = &compute;
    for (unsigned step = 0; step < 100000; ++step) {
        // Channel switches, first use with flags clear, independent/linked limits,
        // dirty-but-identical registers, and sampler_binding without a dirty flag.
        const auto channel = random() % 3;
        auto& engine = engines[channel];
        fast.maxwell3d = old.maxwell3d = &engine;
        fast.channel_state = &fast_channels[channel];
        old.channel_state = &old_channels[channel];
        if (random() % 5 == 0) {
            auto& table = random() % 2 ? engine.regs.tex_header : engine.regs.tex_sampler;
            table.address = random();
            table.limit = random() % 100;
            engine.dirty.flags[Dirty::Descriptors] = true;
        }
        if (random() % 7 == 0)
            engine.regs.sampler_binding =
                static_cast<decltype(engine.regs.sampler_binding)>(random() % 2);
        const bool is_compute = random() % 4 == 0;
        compute.launch_description.linked_tsc = random() % 2;
        compute.regs.tic = {random(), random() % 100};
        compute.regs.tsc = {random(), random() % 100};
        fast.draw_state_fastpath = step % 2 != 0;
        fast.SynchronizeDescriptors(is_compute);
        legacy_sync(old, is_compute);
        assert(fast.texture_bindings_serial == old.texture_bindings_serial);
        for (auto tables : {std::pair{&fast.channel_state->graphics_image_table,
                                      &old.channel_state->graphics_image_table},
                            std::pair{&fast.channel_state->graphics_sampler_table,
                                      &old.channel_state->graphics_sampler_table}}) {
            assert(tables.first->address == tables.second->address);
            assert(tables.first->limit == tables.second->limit);
            assert(tables.first->invalidations == tables.second->invalidations);
        }
        engine.dirty.flags[Dirty::RenderTargets] = random() % 11 == 0;
        fast.rt_active_mask = 0;
        for (unsigned i = 0; i <= NUM_RT; ++i) {
            auto id = random() % 4 == 0 ? random() % 16 : 0;
            if (i == NUM_RT)
                fast.render_targets.depth_buffer_id = id;
            else
                fast.render_targets.color_buffer_ids[i] = id;
            if (id)
                fast.rt_active_mask |= 1u << i;
        }
        old.render_targets = fast.render_targets;
        PassDump::config.enabled = random() % 2;
        const bool clear = random() % 2;
        fast.UpdateRenderTargets(clear);
        legacy_update(old, clear);
        assert(fast.preparations == old.preparations);
        assert(fast.dirty_updates == old.dirty_updates && fast.dumps == old.dumps);
        fast.preparations.clear();
        old.preparations.clear();
        for (unsigned i = 0; i < 16; ++i) {
            fast.slot_image_views[i].image_id = old.slot_image_views[i].image_id = random() % 16;
            fast.slot_images[i] = {static_cast<unsigned>(random() % 4),
                                   static_cast<unsigned>(random() % 5)};
        }
        old.slot_images = fast.slot_images;
        std::vector<ImageViewInOut> views;
        for (unsigned i = 0, n = random() % 32; i < n; ++i)
            views.push_back({static_cast<unsigned>(random() % 16), random() % 2 != 0});
        auto reference_views = views;
        const bool blacklist = random() % 2;
        fast.FillImageViews(views, is_compute, blacklist);
        legacy_fill(old, reference_views, is_compute, blacklist);
        assert(fast.slot_images == old.slot_images);
        assert(fast.reads == old.reads && fast.scale_effects == old.scale_effects);
        for (unsigned i = 0; i < views.size(); ++i)
            assert(views[i].id == reference_views[i].id);
        fast.reads.clear();
        old.reads.clear();
        fast.scale_effects.clear();
        old.scale_effects.clear();
    }
    unsigned fast_calls = 0, old_calls = 0;
    for (unsigned i = 0; i < engines.size(); ++i) {
        fast_calls += fast_channels[i].graphics_image_table.calls;
        old_calls += old_channels[i].graphics_image_table.calls;
    }
    assert(fast_calls < old_calls);
}
