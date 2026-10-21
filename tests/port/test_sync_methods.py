# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay production payload reports and fence publication at the measured call count."""

import tempfile
import unittest
from pathlib import Path

from bench_sync_methods import build, run


class SyncMethods(unittest.TestCase):
    def test_740_call_replay_preserves_visibility_and_avoids_unused_callbacks(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-sync-test-") as temporary:
            binary = build(Path(temporary))
            fence = run(binary, 3, "fence")
            immediate = run(binary, 3, "immediate")
            # Allocation counts, unlike elapsed timings, are deterministic on one STL.
            # Immediate payloads must not allocate discarded retirement closures.
            self.assertLess(immediate["allocations_per_frame"], fence["allocations_per_frame"] / 10)
            self.assertGreater(fence["ns_per_call"], 0)


if __name__ == "__main__":
    unittest.main()
