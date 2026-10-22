# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Draw tables are immutable within a recording tick; resource states are per draw."""

import unittest
from pathlib import Path

D3D12 = Path(__file__).resolve().parents[2] / "src/video_core/renderer_d3d12"


def body(source, signature):
    start = source.index(signature)
    return source[start : source.index("\n}", start)]


class DescriptorTableReuse(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = (D3D12 / "d3d12_descriptor.h").read_text()
        cls.allocator = (D3D12 / "d3d12_descriptor.cpp").read_text()
        cls.rasterizer = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        cls.draw = body(cls.rasterizer, "void RasterizerD3D12::PrepareDraw(")

    def test_equality_uses_tick_sizes_and_full_slot_keys(self):
        match = body(self.allocator, "bool D3D12DescriptorAllocator::CanReusePreviousDrawTable(")
        self.assertIn("previous.tick == context.CurrentTick()", match)
        self.assertIn("previous.plan.sizes == plan.sizes", match)
        self.assertIn("previous.plan.keys == plan.keys", match)
        self.assertNotIn("hash", match.lower())
        key = self.header.split("struct DescriptorKey {", 1)[1].split("struct DescriptorPlan", 1)[0]
        for field in ("description", "resource", "kind", "size"):
            self.assertIn(field, key)
        self.assertIn("operator==(const DescriptorKey&) const = default", key)

    def test_plan_and_writer_share_key_construction(self):
        self.assertEqual(self.allocator.count("MakeDescriptorKey(resource, kind, desc, size)"), 2)
        make = body(self.allocator, "DescriptorKey D3D12DescriptorAllocator::MakeDescriptorKey(")
        self.assertIn("resource->handle.Get()", make)
        self.assertIn("key.kind = kind", make)
        self.assertIn("key.size = static_cast<u32>(size)", make)
        self.assertIn("std::memcpy(key.description.data(), desc, size)", make)
        self.assertIn("table.plan->keys.at(index)", self.allocator)
        self.assertIn("table.plan->resources.at(index) = resource", self.allocator)

    def test_reuse_returns_before_allocation_and_copies(self):
        finish = body(self.allocator, "DescriptorTable D3D12DescriptorAllocator::FinishDrawTable(")
        reuse = finish[: finish.index("const u32 count")]
        self.assertIn("if (CanReusePreviousDrawTable(plan, sampler))", reuse)
        self.assertIn("return previous.table;", reuse)
        self.assertIn("FrameCount::DescriptorTableReuses", reuse)
        self.assertNotIn("Allocate", reuse.split("{", 1)[1])
        self.assertNotIn("WriteCached", reuse)
        self.assertIn("previous.tick = context.CurrentTick()", finish)
        self.assertIn("std::swap(previous.plan, plan)", finish)

    def test_reservation_rechecks_tick_and_reserves_both_on_submission(self):
        reserve = self.draw.index("descriptors.Reserve(")
        finish = self.draw.index("descriptors.FinishDrawTable(")
        window = self.draw[reserve:finish]
        self.assertIn("reuse_views ? 0u : view_block.count", window)
        self.assertIn("reuse_samplers ? 0u : sampler_size", window)
        self.assertIn("if (context.CurrentTick() != table_tick)", window)
        self.assertIn("descriptors.Reserve(view_block.count, sampler_size)", window)
        self.assertLess(self.draw.index("CopyFeedback(sampled, &texture_cache.GetImageView(item.id).range)"), reserve)
        self.assertLess(self.draw.index("buffer_runtime.CopyUniform(buffer)"), reserve)

    def test_states_and_retention_run_even_when_tables_are_reused(self):
        finish = self.draw.index("descriptors.FinishDrawTable(")
        collection = self.draw[:finish]
        for state in (
            "need(buffer.resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)",
            "need(sampled, SrvState)",
            "need(buffer.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)",
            "need(view.ImageHandle(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)",
            "need(color_resources[slot], D3D12_RESOURCE_STATE_RENDER_TARGET)",
        ):
            self.assertIn(state, collection)
        record = self.draw[finish:]
        self.assertIn("context.TransitionBatch(state_scratch)", record)
        self.assertNotIn("reuse_views", record)
        self.assertNotIn("reuse_samplers", record)
        context = (D3D12 / "d3d12_context.cpp").read_text()
        transition = body(context, "void Context::Transition(")
        self.assertLess(
            transition.index("Retain(resource)"), transition.index("resource->state == state")
        )

    def test_root_cbvs_feedback_and_null_slots_are_planned(self):
        self.assertIn("const u32 cbv_size = root_cbvs ? 0u : TableSize(totals.cbv)", self.draw)
        self.assertIn("view_plan.Reset({cbv_size, srv_size, uav_size})", self.draw)
        self.assertRegex(
            self.draw,
            r"SrvDimension\(desc.type, desc.is_multisample\),\s*sampled,"
            r"\s*trace_binding \? &resolved_srv : nullptr\)",
        )
        for kind in ("Cbv", "Srv", "Uav", "Sampler"):
            self.assertIn("descriptors.WriteNull" + kind, self.draw)
        self.assertIn("table.first + slot", self.allocator)
        self.assertIn("first + offset", self.header)

    def test_profile_name_matches_counter(self):
        header = (D3D12 / "d3d12_context.h").read_text()
        source = (D3D12 / "d3d12_context.cpp").read_text()
        self.assertRegex(header, r"DescriptorCopies,\s*DescriptorTableReuses,\s*DescriptorWrites,")
        self.assertRegex(
            source, r'"descriptor_copies",\s*"descriptor_table_reuses",\s*"descriptor_writes",'
        )


if __name__ == "__main__":
    unittest.main()
