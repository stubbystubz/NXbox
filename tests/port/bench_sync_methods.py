# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile production query/fence paths against the existing host port doubles."""

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile

from test_d3d12_fence_manager import ROOT, STUBS
from test_query_report_timestamp import function_body

HEADERS = (
    "common/common_types.h",
    "common/settings.h",
    "common/thread.h",
    "video_core/delayed_destruction_ring.h",
    "video_core/gpu.h",
    "video_core/host1x/host1x.h",
    "video_core/host1x/syncpoint_manager.h",
    "video_core/rasterizer_interface.h",
)


def build(folder, revision=None, sanitize=False):
    def read(path):
        if revision:
            return subprocess.check_output(
                ["git", "show", f"{revision}:{path}"], cwd=ROOT, text=True
            )
        return (ROOT / path).read_text()

    (folder / "stubs.h").write_text(STUBS)
    for name in HEADERS:
        target = folder / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('#include "stubs.h"\n')
    fence = folder / "video_core/fence_manager.h"
    fence.write_text(read("src/video_core/fence_manager.h"))
    body = function_body(
        read("src/video_core/query_cache/query_cache.h"),
        "void QueryCacheBase<Traits>::CounterReport(",
    )
    source = folder / "sync.cpp"
    source.write_text(
        (ROOT / "tests/port/sync_methods.cpp").read_text().replace("__COUNTER_REPORT__", body)
    )
    binary = folder / "sync"
    flags = ["-O1", "-g", "-fsanitize=address,undefined"] if sanitize else ["-O3"]
    subprocess.run(
        [
            "clang++",
            "-std=c++20",
            "-pthread",
            *flags,
            "-I",
            str(folder),
            "-I",
            str(ROOT / "src"),
            str(source),
            "-o",
            str(binary),
        ],
        check=True,
    )
    return binary


def run(binary, frames, mode):
    env = dict(
        os.environ,
        NXBOX_TIMING_TRACE="0",
        NXBOX_WAIT_DIAG="0",
        NXBOX_QUERY_TIMESTAMP_AT_PUBLISH="0",
    )
    return json.loads(subprocess.check_output([str(binary), str(frames), mode], env=env, text=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision")
    parser.add_argument("--frames", type=int, default=2000)
    parser.add_argument("--repeat", type=int, default=7)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if args.frames < 1 or args.repeat < 1:
        parser.error("frames and repeat must be positive")
    with tempfile.TemporaryDirectory(prefix="nxbox-sync-") as temporary:
        binary = build(Path(temporary), args.revision, args.sanitize)
        for mode in ("fence", "immediate"):
            samples = [run(binary, args.frames, mode) for _ in range(args.repeat)]
            print(
                json.dumps(
                    {
                        "mode": mode,
                        "revision": args.revision or "worktree",
                        "samples": samples,
                        "median": {
                            key: statistics.median(row[key] for row in samples)
                            for key in samples[0]
                        },
                    }
                )
            )


if __name__ == "__main__":
    main()
