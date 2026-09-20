#!/usr/bin/env python3
"""
Subtract GlobalStaticBufferPool init overhead from perf stat results.

Run a "init-only" baseline perf stat (buffer pool init + trivial query, minimal
actual work) and subtract its counter values from the full-run perf stat to get
corrected numbers that exclude the one-time initialization overhead.

Usage:
    python3 correct_perf_stat.py <full_stat.txt> <baseline_stat.txt> <corrected_stat.txt>
"""

import re
import sys
from pathlib import Path

# Matches:   "    70,478,728,193      cycles     #  ..."
#        or: "        22,121.39 msec task-clock  #  ..."
# Groups: (leading_ws)(value)(optional " msec ")(counter_name)(rest)
_COUNTER_RE = re.compile(
    r'^(\s+)([\d,]+(?:\.\d+)?)'
    r'(\s+(?:msec\s+)?)'
    r'(\S[\S -]*?)'
    r'(\s+#.*)?$'
)
# "         38.32 +- 14.67 seconds time elapsed  ( +- 38.30% )"
_ELAPSED_RE = re.compile(
    r'^(\s+)([\d.]+)(\s+\+-\s+[\d.]+\s+seconds time elapsed.*)'
)


def _parse_file(path: str) -> tuple[dict[str, float], float | None]:
    """Return (counters, elapsed_s).

    counters maps counter_name -> numeric value (average over -r runs).
    elapsed_s is the wall time in seconds, or None if not found.
    """
    counters: dict[str, float] = {}
    elapsed: float | None = None

    for line in Path(path).read_text().splitlines():
        if '<not supported>' in line:
            continue

        m = _ELAPSED_RE.match(line)
        if m:
            elapsed = float(m.group(2))
            continue

        m = _COUNTER_RE.match(line)
        if not m:
            continue
        val_str = m.group(2).replace(',', '')
        name = m.group(4).strip()
        try:
            val = float(val_str)
            if val_str.isdigit() or re.fullmatch(r'\d[\d,]*', m.group(2)):
                val = int(val_str.replace(',', ''))
        except ValueError:
            continue
        counters[name] = val

    return counters, elapsed


def _fmt_delta(delta: float) -> str:
    if isinstance(delta, int) or delta == int(delta):
        d = int(delta)
        sign = '+' if d >= 0 else ''
        return f'({sign}{d:,})'
    return f'({delta:+.2f})'


def main() -> None:
    if len(sys.argv) != 4:
        print(f'Usage: {sys.argv[0]} <full_stat.txt> <baseline_stat.txt> <corrected_stat.txt>')
        sys.exit(1)

    full_path, baseline_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]

    if not Path(baseline_path).exists():
        print(f'[warn] baseline not found: {baseline_path}  (skipping correction)', file=sys.stderr)
        sys.exit(0)
    if not Path(full_path).exists():
        print(f'[warn] full stat not found: {full_path}', file=sys.stderr)
        sys.exit(0)

    baseline, base_elapsed = _parse_file(baseline_path)
    full, full_elapsed = _parse_file(full_path)

    lines: list[str] = []
    lines.append(f'# perf stat corrected: BufferPool init baseline subtracted\n')
    lines.append(f'# full run:  {full_elapsed:.2f}s elapsed\n' if full_elapsed else '')
    lines.append(f'# baseline:  {base_elapsed:.2f}s elapsed  (init-only)\n' if base_elapsed else '')
    if full_elapsed is not None and base_elapsed is not None:
        corrected_elapsed = max(0.0, full_elapsed - base_elapsed)
        lines.append(f'# corrected: {corrected_elapsed:.2f}s elapsed\n')
    lines.append('#\n')
    lines.append(f'# {"Counter":<35} {"Full":>18}   {"Baseline":>18}   {"Corrected":>18}   Delta\n')
    lines.append(f'# {"-"*35} {"-"*18}   {"-"*18}   {"-"*18}   -----\n')

    all_keys = list(full.keys())
    for key in all_keys:
        fval = full[key]
        bval = baseline.get(key, 0)
        if isinstance(fval, int) and isinstance(bval, (int, float)):
            bval = int(bval)
            cval = max(0, fval - bval)
            delta = _fmt_delta(cval - fval)
            lines.append(
                f'  {key:<35} {fval:>18,}   {bval:>18,}   {cval:>18,}   {delta}\n'
            )
        else:
            cval = max(0.0, fval - bval)
            delta = _fmt_delta(cval - fval)
            lines.append(
                f'  {key:<35} {fval:>18.2f}   {bval:>18.2f}   {cval:>18.2f}   {delta}\n'
            )

    if full_elapsed is not None and base_elapsed is not None:
        cval = max(0.0, full_elapsed - base_elapsed)
        delta = _fmt_delta(cval - full_elapsed)
        lines.append(
            f'  {"time elapsed (s)":<35} {full_elapsed:>18.2f}   '
            f'{base_elapsed:>18.2f}   {cval:>18.2f}   {delta}\n'
        )

    Path(out_path).write_text(''.join(l for l in lines if l))
    print(f'  → corrected: {out_path}')


if __name__ == '__main__':
    main()
