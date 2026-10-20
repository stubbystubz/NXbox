#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rank frame-weighted GPU pass samples from NXbox eden.txt logs."""

import argparse
import json
import math
import re
from collections import defaultdict
from pathlib import Path

MARKER = "NXBOX GPU_PASS "
KINDS = {"draw", "compute", "copy", "clear", "blit"}


def number(fields, key):
    value = float(fields[key])
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"invalid {key}")
    return value


def summarize(lines):
    """Consume complete windows; absent top-15 signatures contribute zero (lower bound)."""
    pending = []
    sums = defaultdict(lambda: {"ms": 0.0, "draws": 0.0, "calls/frame": 0.0})
    totals = dict.fromkeys(("total_ms", "frame_gpu_ms", "idle_ms"), 0.0)
    frames = windows = malformed = dropped = truncated = 0
    for line in lines:
        if MARKER not in line:
            continue
        fields = dict(
            token.split("=", 1) for token in line.split(MARKER, 1)[1].split() if "=" in token
        )
        try:
            if "rank" in fields:
                if int(fields["rank"]) < 1 or fields["kind"] not in KINDS:
                    raise ValueError("invalid pass")
                if not re.fullmatch(r"\d+x\d+", fields["rt"]):
                    raise ValueError("invalid extent")
                if not re.fullmatch(r"[0-9a-fA-F]{1,16}", fields["shader_hash"]):
                    raise ValueError("invalid hash")
                key = tuple(fields[name] for name in ("kind", "rt", "fmt", "depth")) + (
                    f"{int(fields['shader_hash'], 16):016x}",
                )
                pending.append(
                    (key, {name: number(fields, name) for name in ("ms", "draws", "calls/frame")})
                )
            elif "total_ms" in fields:
                weight = int(fields.get("frames", "1"))
                if weight < 1:
                    raise ValueError("invalid frames")
                values = {name: number(fields, name) for name in totals}
                lost = int(fields.get("dropped_submissions", "0"))
                limited = int(fields.get("truncated_passes", "0"))
                if min(lost, limited) < 0:
                    raise ValueError("invalid loss count")
                for key, sample in pending:
                    for name, value in sample.items():
                        sums[key][name] += value * weight
                for name, value in values.items():
                    totals[name] += value * weight
                frames += weight
                windows += 1
                dropped += lost
                truncated += limited
                pending.clear()
            else:
                malformed += 1
        except (KeyError, ValueError, OverflowError):
            malformed += 1
    ranked = []
    for key, values in sums.items():
        item = dict(zip(("kind", "rt", "fmt", "depth", "shader_hash"), key))
        item.update({name: value / frames for name, value in values.items()})
        ranked.append(item)
    ranked.sort(key=lambda item: (-item["ms"], item["shader_hash"]))
    return {
        "windows": windows,
        "frames": frames,
        "malformed_lines": malformed,
        "incomplete_pass_lines": len(pending),
        "dropped_submissions": dropped,
        "truncated_passes": truncated,
        "totals": {name: value / frames if frames else 0 for name, value in totals.items()},
        "passes": ranked,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="eden.txt log")
    parser.add_argument("--top", type=int, default=15)
    parser.add_argument("--json", action="store_true", help="emit machine-readable results")
    args = parser.parse_args()
    if args.top < 1:
        parser.error("--top must be positive")
    with args.log.open(encoding="utf-8", errors="replace") as stream:
        result = summarize(stream)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(
            f"windows={result['windows']} frames={result['frames']} "
            f"dropped_submissions={result['dropped_submissions']} "
            f"truncated_passes={result['truncated_passes']} "
            f"malformed_lines={result['malformed_lines']} "
            f"incomplete_pass_lines={result['incomplete_pass_lines']}"
        )
        for rank, item in enumerate(result["passes"][: args.top], 1):
            print(
                f"NXBOX GPU_PASS rank={rank} ms={item['ms']:.4f} kind={item['kind']} "
                f"draws={item['draws']:.3f} rt={item['rt']} fmt={item['fmt']} "
                f"depth={item['depth']} shader_hash={item['shader_hash']} "
                f"calls/frame={item['calls/frame']:.3f}"
            )
        print(
            "NXBOX GPU_PASS "
            + " ".join(f"{name}={value:.4f}" for name, value in result["totals"].items())
        )
        print("Ranked values are lower bounds: each source window logs only its top 15 passes.")
    return 0 if result["windows"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
