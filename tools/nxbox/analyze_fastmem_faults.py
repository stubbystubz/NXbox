#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 NXbox contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarize every recorded first-chance AV in NXbox console captures."""

import argparse
import collections
import json
import re
from pathlib import Path


def analyze(directory):
    crash = directory / "crash.txt"
    if not crash.exists():
        return {"capture": directory.name, "error": "crash.txt unavailable"}
    eden = directory / "eden.txt"
    memory = re.search(
        r"backing_base=(0x[0-9a-f]+) backing_size=(0x[0-9a-f]+) arena_base=(0x[0-9a-f]+)",
        eden.read_text(errors="replace") if eden.exists() else "",
    )
    arena = int(memory[3], 16) if memory else None
    records = []
    terminal = []
    lines = crash.read_text(errors="replace").splitlines()
    for number, line in enumerate(lines, 1):
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
        if fields.get("event") in {"unhandled", "active_exception", "SIGABRT", "terminate"}:
            terminal.append({"line": number, "text": line})
        if fields.get("event") != "first_chance" or fields.get("code") != "0xc0000005":
            continue
        records.append(
            {
                "line": number,
                "tick": int(fields["tick_ms"]),
                "pc": int(fields["pc"], 16),
                "target": int(fields["target"], 16),
                "access": fields["access"],
                "role": fields["role"],
            }
        )
    pcs = collections.Counter(hex(record["pc"]) for record in records)
    pages = collections.Counter(hex(record["target"] & ~4095) for record in records)
    targets = collections.Counter(hex(record["target"]) for record in records)
    pairs = collections.Counter((record["pc"], record["target"]) for record in records)
    return {
        "capture": directory.name,
        "lines_read": len(lines),
        "memory": dict(zip(("backing_base", "backing_size", "arena_base"), memory.groups()))
        if memory
        else None,
        "avs": len(records),
        "inside_512_gib_arena": sum(
            arena <= record["target"] < arena + (1 << 39) for record in records
        )
        if arena is not None
        else None,
        "roles": collections.Counter(record["role"] for record in records),
        "access": collections.Counter(record["access"] for record in records),
        "first_tick": records[0]["tick"] if records else None,
        "last_tick": records[-1]["tick"] if records else None,
        "unique_pcs": len(pcs),
        "unique_targets": len(targets),
        "unique_pages": len(pages),
        "max_identical_pc_target_count": max(pairs.values(), default=0),
        "pc_counts": pcs,
        "page_counts": pages,
        "top_targets": targets.most_common(10),
        "terminal_records": terminal,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps([analyze(directory) for directory in args.captures], indent=2))


if __name__ == "__main__":
    main()
