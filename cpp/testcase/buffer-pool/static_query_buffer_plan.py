#!/usr/bin/env python3
"""Build a double-buffer capacity plan from a static query chunk trace.

The input is a CSV with the columns worker,sequence,column,bytes.  Sequence is
the zero-based file/row-group position seen by a worker.  Even and odd
sequences occupy different pipeline slots, so each (worker, slot, column)
only needs the largest chunk assigned to it.
"""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path
from typing import Iterable


REQUIRED_FIELDS = {"worker", "sequence", "column", "bytes"}


def align_up(value: int, alignment: int) -> int:
    if value < 0:
        raise ValueError("bytes must be non-negative")
    if alignment <= 0:
        raise ValueError("alignment must be positive")
    return ((value + alignment - 1) // alignment) * alignment


def build_plan(rows: Iterable[dict[str, str]], alignment: int = 4096) -> dict:
    slot_maxima: dict[tuple[str, int, str], int] = defaultdict(int)
    worker_columns: set[tuple[str, str]] = set()
    row_count = 0

    for row_number, row in enumerate(rows, start=2):
        missing = REQUIRED_FIELDS.difference(row)
        if missing:
            raise ValueError(f"missing CSV fields: {', '.join(sorted(missing))}")
        try:
            sequence = int(row["sequence"])
            requested = int(row["bytes"])
        except ValueError as exc:
            raise ValueError(f"row {row_number}: sequence and bytes must be integers") from exc
        if sequence < 0:
            raise ValueError(f"row {row_number}: sequence must be non-negative")
        worker = row["worker"].strip()
        column = row["column"].strip()
        if not worker or not column:
            raise ValueError(f"row {row_number}: worker and column must be non-empty")

        size = align_up(requested, alignment)
        slot = sequence % 2
        key = (worker, slot, column)
        slot_maxima[key] = max(slot_maxima[key], size)
        worker_columns.add((worker, column))
        row_count += 1

    # The conventional static pool gives both slots the maximum observed by
    # either slot.  This is the apples-to-apples baseline for the same trace.
    symmetric_bytes = 0
    for worker, column in worker_columns:
        maximum = max(
            slot_maxima.get((worker, 0, column), 0),
            slot_maxima.get((worker, 1, column), 0),
        )
        symmetric_bytes += maximum * 2

    planned_bytes = sum(slot_maxima.values())
    entries = [
        {"worker": worker, "slot": slot, "column": column, "bytes": size}
        for (worker, slot, column), size in sorted(slot_maxima.items())
    ]
    saved = symmetric_bytes - planned_bytes
    return {
        "alignment": alignment,
        "trace_rows": row_count,
        "planned_bytes": planned_bytes,
        "symmetric_bytes": symmetric_bytes,
        "saved_bytes": saved,
        "saved_ratio": (saved / symmetric_bytes) if symmetric_bytes else 0.0,
        "entries": entries,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="CSV: worker,sequence,column,bytes")
    parser.add_argument("-o", "--output", type=Path, help="write JSON here (default: stdout)")
    parser.add_argument("--alignment", type=int, default=4096)
    args = parser.parse_args()

    with args.trace.open(newline="", encoding="utf-8") as source:
        plan = build_plan(csv.DictReader(source), args.alignment)
    rendered = json.dumps(plan, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
