#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rank exclusive Maxwell method time by median over FrameProfile windows."""

import argparse
import json
import math
import re
import statistics
from pathlib import Path

MARKER = "NXBOX METHOD_PROFILE "
KEY = re.compile(r"[a-z][a-z0-9_]*")


def summarize(lines):
    """Missing top-16 registers contribute zero; register medians are lower bounds."""
    windows = []
    malformed = 0
    for line in lines:
        if MARKER not in line:
            continue
        try:
            window = {}
            for token in line.split(MARKER, 1)[1].split():
                key, value = token.split("=", 1)
                if key == "frames":
                    if int(value) <= 0:
                        raise ValueError("empty window")
                    continue
                if not KEY.fullmatch(key) or key in window:
                    raise ValueError("invalid or duplicate class")
                pair = tuple(map(float, value.split(",")))
                if len(pair) != 2 or any(not math.isfinite(v) or v < 0 for v in pair):
                    raise ValueError("invalid measurement")
                window[key] = pair
            if not window:
                raise ValueError("empty line")
            windows.append(window)
        except (ValueError, OverflowError):
            malformed += 1
    keys = set().union(*(window.keys() for window in windows))
    rows = []
    for key in keys:
        values = [window.get(key, (0.0, 0.0)) for window in windows]
        rows.append(
            {
                "class": key,
                "ms/frame": statistics.median(value[0] for value in values),
                "count/frame": statistics.median(value[1] for value in values),
                "windows_present": sum(key in window for window in windows),
            }
        )
    rows.sort(key=lambda row: (-row["ms/frame"], row["class"]))
    return {"windows": len(windows), "malformed": malformed, "ranking": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    with args.log.open(errors="replace") as stream:
        result = summarize(stream)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"windows={result['windows']} malformed={result['malformed']}")
        print("class                         ms/frame    count/frame")
        for row in result["ranking"][: max(0, args.top)]:
            print(f"{row['class']:<29} {row['ms/frame']:9.4f} {row['count/frame']:14.3f}")
        print("other_0x registers detail 'other'; do not add them to class totals.")
    return 0 if result["windows"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
