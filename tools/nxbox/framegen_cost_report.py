# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare existing D3D12 profile windows per real frame without contacting a device."""

import argparse
import re
from pathlib import Path

FIELDS = (
    "maxwell3d_methods_cpu_ms",
    "texture_cache_update_cpu_ms",
    "staging_collect_cpu_ms",
    "between_draws_cpu_ms",
    "fence_wait_wall_ms",
    "submit_cpu_ms",
    "draw_recording_cpu_ms",
    "descriptor_setup_cpu_ms",
    "barriers_cpu_ms",
    "gpu_busy_ms",
    "ExecuteCommandLists",
    "forced_submits_present",
    "fg_generated_service_ms",
    "fg_real_present_ms",
    "fg_fence_ms",
)


def read_windows(path, first, count):
    lines = path.read_text().splitlines()
    profiles = [
        (
            number,
            {key: float(value) for key, value in re.findall(r"(\w+)=(-?[0-9.]+)(?= |$)", line)},
        )
        for number, line in enumerate(lines, 1)
        if line.startswith("D3D12_FRAME_PROFILE ")
    ][first : first + count]
    if len(profiles) != count or any(row["frames"] <= 0 for _, row in profiles):
        raise ValueError(f"{path}: requested windows must exist and contain real frames")
    frames = sum(row["frames"] for _, row in profiles)
    values = {field: sum(row.get(field, 0) for _, row in profiles) / frames for field in FIELDS}
    # Thread CPU percentage is measured over wall time; five seconds is the
    # nominal report period, not an exact timestamp recovered from the artifact.
    values["thread_cpu_ms_approx"] = (
        sum(row["gpu_thread_cpu_pct"] * 50 for _, row in profiles) / frames
    )
    values["recorder_busy_ms"] = (
        sum(row["recorder_busy_ms_per_frame"] * row["frames"] for _, row in profiles) / frames
    )
    print(
        f"{path.parent.name}: lines {profiles[0][0]}–{profiles[-1][0]}, "
        f"{count} windows, {int(frames)} real frames, "
        f"THREAD_PROFILE records={sum(line.startswith('THREAD_PROFILE ') for line in lines)}"
    )
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--first", type=int, default=12, help="zero-based profile index")
    parser.add_argument("--count", type=int, default=22)
    args = parser.parse_args()
    if args.first < 0 or args.count <= 0:
        parser.error("first must be nonnegative and count must be positive")
    before = read_windows(args.baseline, args.first, args.count)
    after = read_windows(args.candidate, args.first, args.count)
    print("\n| Field / real frame | Baseline | Candidate | Delta |")
    print("| --- | ---: | ---: | ---: |")
    for field in ("thread_cpu_ms_approx", "recorder_busy_ms", *FIELDS):
        print(
            f"| {field} | {before[field]:.4f} | {after[field]:.4f} | "
            f"{after[field] - before[field]:+.4f} |"
        )
    print("\nAbsent FG-only baseline fields are shown as zero (feature disabled).")
    print("Phase *_cpu_ms fields use wall time; do not sum them with thread CPU time.")


if __name__ == "__main__":
    main()
