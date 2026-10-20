# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Protected async PSOs get one bounded wait before a draw is skipped.
Skipped draws are lost for good when the game renders them once: SM3DW bakes
its level ambient light at load and the level stayed black on Xbox (async on)."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp"
POOL = ROOT / "src/video_core/renderer_d3d12/d3d12_async_pso.h"


def skip_path(source, kind):
    """Scope to the pending-request path, independent of diagnostic text length."""
    signature, counter = {
        "graphics": ("IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline(", "Draws"),
        "compute": ("ComputePipeline* PipelineCache::CurrentComputePipeline()", "Dispatches"),
    }[kind]
    body = source.split(signature, 1)[1].split("\n}", 1)[0]
    return body.rsplit(
        "if (!may_skip && pso_waited.insert(" if kind == "graphics" else "if (pso_waited.insert(", 1
    )[1].split(f"Count(FrameCount::{counter}SkippedPendingPso)", 1)[0]


class PsoWait(unittest.TestCase):
    def test_both_skip_paths_wait_once_first(self):
        source = CACHE.read_text()
        for kind, key in (("graphics", "request_hash"), ("compute", "request")):
            with self.subTest(kind=kind):
                before = skip_path(source, kind)
                self.assertTrue(before.startswith(f"{key}).second) {{"))
                self.assertEqual(before.count("async->WaitFor("), 1)
                self.assertIn("PendingPsoWaitBudget()", before)
                # A completed first-use PSO is installed and returned before the skip counter.
                wait = before.index("async->WaitFor(")
                ready = before.index("if (settled.status == AsyncCache::Status::Ready)")
                self.assertLess(wait, ready)
                self.assertIn(f"return settled.result->{kind}.get();", before[ready:])

    def test_unadmitted_requests_keep_their_wait(self):
        # E2 (Wonder): 28 of 50 waits came back Missing because the pool was full; the
        # draw spent its only wait without waiting and was skipped for good afterwards.
        source = CACHE.read_text()
        for kind in ("graphics", "compute"):
            before = skip_path(source, kind)
            wait = before.index("async->WaitFor(")
            self.assertIn("if (settled.status == AsyncCache::Status::Missing)", before[wait:])
            self.assertIn("pso_waited.erase(", before[wait:])

    def test_waiting_requests_are_always_queued(self):
        # Odyssey (cold cache): the full pool refused the coin counter's PSO, its wait returned in
        # microseconds and the counter, drawn into its texture once, never showed.
        source = CACHE.read_text()
        graphics = source.split("CurrentGraphicsPipeline(", 1)[1].split(
            "Count(FrameCount::DrawsSkippedPendingPso);\n            trace_skip(may_skip", 1
        )[0]
        self.assertLess(
            graphics.index("const bool may_skip"), graphics.index("async->CanSubmit(!may_skip)")
        )
        self.assertIn("false, !may_skip);", graphics)
        compute = source.split("CurrentComputePipeline()", 1)[1]
        self.assertIn("async->CanSubmit(true)", compute)
        self.assertIn("async->Submit(async_key, std::move(job), false, true);", compute)
        pool = POOL.read_text()
        self.assertIn("(urgent || state->pending - stalled < state->capacity)", pool)

    def test_workers_wake_waiters(self):
        pool = POOL.read_text()
        self.assertIn("Entry WaitFor(", pool)
        self.assertIn("state.finished.notify_all();", pool)
        self.assertIn("finished.wait_for(", pool)


if __name__ == "__main__":
    unittest.main()
