#!/usr/bin/env python3
"""
解析 buffer-perf-concurrency-suite 单次运行目录，汇总 wall time + perf stat。

用法:
  python3 parse_results.py /path/to/results/20260506_000028
  python3 parse_results.py --latest              # 选 results/ 下最新子目录
  python3 parse_results.py RUN_DIR -o out.csv  # 指定输出 CSV

默认写出:
  <RUN_DIR>/parsed_summary.csv
  <RUN_DIR>/parsed_summary.md   （与 CSV 同主干：默认 parsed_summary.md；`-o x.csv` 则写出 x.md）

套件目录结构可为「嵌套」如 q01_24ssd_singlebuffer_t24/，或为旧版扁平 singlebuffer_t24/。
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path
from typing import Any


def read_meta(run_dir: Path) -> dict[str, str]:
    meta_path = run_dir / "RUN_META.txt"
    out: dict[str, str] = {}
    if not meta_path.is_file():
        return out
    for line in meta_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if "=" in line:
            k, _, v = line.partition("=")
            out[k.strip()] = v.strip()
    return out


def read_wall_csv(run_dir: Path) -> dict[tuple[str, str, str, int], float]:
    """(query, ssd_mode, mode, threads) -> primary query execution time.

    新版 CSV 的 wall_time_s 是 EXPLAIN ANALYZE Total Time；旧版则可能是
    DuckDB shell Run Time real。兼容旧版仅有 mode,threads 的 CSV，其键为
    ("", "", mode, threads)。
    """
    p = run_dir / "summary_wall_time.csv"
    m: dict[tuple[str, str, str, int], float] = {}
    if not p.is_file():
        return m
    with p.open(newline="", encoding="utf-8") as f:
        r = csv.DictReader(f)
        fieldnames = set(r.fieldnames or [])
        has_query = "query" in fieldnames
        for row in r:
            try:
                mode = row.get("mode", "").strip()
                threads = int(row.get("threads", "0"))
                wt = float(row.get("wall_time_s", "nan"))
                if not mode:
                    continue
                if has_query:
                    q = row.get("query", "").strip()
                    ssd = row.get("ssd_mode", "").strip()
                    m[(q, ssd, mode, threads)] = wt
                else:
                    m[("", "", mode, threads)] = wt
            except (TypeError, ValueError):
                continue
    return m


def parse_duckdb_wall(log_text: str) -> tuple[float | None, float | None]:
    """返回 (total_time_explain_s, run_time_real_s)"""
    total = None
    m = re.search(r"Total Time:\s*([\d.]+)\s*s", log_text)
    if m:
        total = float(m.group(1))
    real_t = None
    m = re.search(r"Run Time \(s\):\s*real\s+([\d.]+)", log_text)
    if m:
        real_t = float(m.group(1))
    return total, real_t


def parse_perf_stat(text: str) -> dict[str, Any]:
    """
    解析 perf stat -ddd 文本块。
    数值行形如: '    71,555,378,774      cycles    ...'
    """
    d: dict[str, Any] = {}

    # time elapsed: "36.82 +- 10.18 seconds time elapsed"
    m = re.search(
        r"([\d.]+)\s*\+-\s*([\d.]+)\s+seconds\s+time\s+elapsed",
        text,
        re.I,
    )
    if m:
        d["elapsed_mean_s"] = float(m.group(1))
        d["elapsed_std_s"] = float(m.group(2))

    # task-clock: "22,309.37 msec task-clock"
    m = re.search(r"([\d,]+(?:\.\d+)?)\s+msec\s+task-clock", text)
    if m:
        d["task_clock_ms"] = float(m.group(1).replace(",", ""))

    ipc_hint = None
    for line in text.splitlines():
        ls = line.strip()
        if ls.startswith("#") or not ls:
            continue
        if ls.startswith("<not supported>"):
            continue
        if "task-clock" in ls:
            continue
        if "instructions" in ls and "insn per cycle" in ls:
            mip = re.search(r"#\s*([\d.]+)\s+insn\s+per\s+cycle", line)
            if mip:
                ipc_hint = float(mip.group(1))

        # leading number(s) + metric name (first token after gaps)
        m = re.match(
            r"^([\d,]+(?:\.\d+)?)\s+([\w\-]+)\s",
            ls,
        )
        if not m:
            continue
        val_raw, name = m.group(1), m.group(2)
        try:
            val = float(val_raw.replace(",", ""))
        except ValueError:
            continue
        # normalize counter keys（勿覆盖已由上文解析的摘要字段）
        key = name.replace("-", "_")
        skip = {"elapsed_mean_s", "elapsed_std_s", "ipc", "branch_miss_pct", "cache_miss_pct"}
        if key in skip:
            continue
        if key not in d or key in ("cycles", "instructions"):
            d[key] = val

    if ipc_hint is not None:
        d["ipc"] = ipc_hint
    elif "cycles" in d and "instructions" in d and d["cycles"]:
        d["ipc"] = round(d["instructions"] / d["cycles"], 4)

    # branch-misses line sometimes has % in comment — extract if useful
    m = re.search(r"branch-misses\s+.*?#\s*([\d.]+)%\s+of\s+all\s+branches", text)
    if m:
        d["branch_miss_pct"] = float(m.group(1))

    m = re.search(r"cache-misses\s+.*?#\s*([\d.]+)%\s+of\s+all\s+cache\s+refs", text)
    if m:
        d["cache_miss_pct"] = float(m.group(1))

    return d


def discover_runs(run_dir: Path) -> list[tuple[str, str, str, int, Path]]:
    """返回 [(query, ssd_mode, mode, threads, case_dir), ...]

    新版套件目录名: q01_24ssd_singlebuffer_t24
    旧版扁平目录名: singlebuffer_t24（query/ssd 为空串）
    """
    pat_nested = re.compile(
        r"^(q\d+)_([\w-]+)_(pread-singlebuffer|singlebuffer|doublebuffer)_t(\d+)$"
    )
    pat_flat = re.compile(r"(pread-singlebuffer|singlebuffer|doublebuffer)_t(\d+)$")
    rows: list[tuple[str, str, str, int, Path]] = []
    for child in sorted(run_dir.iterdir()):
        if not child.is_dir():
            continue
        m = pat_nested.fullmatch(child.name)
        if m:
            rows.append(
                (
                    m.group(1),
                    m.group(2),
                    m.group(3),
                    int(m.group(4)),
                    child,
                )
            )
            continue
        m = pat_flat.fullmatch(child.name)
        if m:
            rows.append(("", "", m.group(1), int(m.group(2)), child))
    rows.sort(key=lambda x: (x[0], x[1], x[2], x[3]))
    return rows


def ratio(num: float | None, den: float | None) -> str:
    if num is None or den is None or den == 0:
        return ""
    return f"{num / den:.4f}"


def fmt_num(x: Any) -> str:
    if x is None:
        return ""
    if isinstance(x, float):
        if abs(x) >= 1e9:
            return f"{x:.4g}"
        return f"{x:.6g}".rstrip("0").rstrip(".")
    return str(x)


def main() -> int:
    ap = argparse.ArgumentParser(description="Parse buffer-perf-concurrency-suite run directory.")
    ap.add_argument(
        "run_dir",
        nargs="?",
        default=None,
        help="单次运行目录（含 RUN_META.txt）；省略时用 --latest",
    )
    ap.add_argument("--latest", action="store_true", help="使用 suite 下 results/ 中最新子目录")
    ap.add_argument("-o", "--csv-out", help="写出 CSV 路径（默认 RUN_DIR/parsed_summary.csv）")
    ap.add_argument("--no-md", action="store_true", help="不写 parsed_summary.md")
    args = ap.parse_args()

    suite_root = Path(__file__).resolve().parent
    results_root = suite_root / "results"

    run_dir: Path | None = None
    if args.latest:
        if not results_root.is_dir():
            print("No results/ directory next to parse_results.py", file=sys.stderr)
            return 1
        candidates = [p for p in results_root.iterdir() if p.is_dir()]
        if not candidates:
            print("results/ is empty", file=sys.stderr)
            return 1
        run_dir = max(candidates, key=lambda p: p.stat().st_mtime)
    elif args.run_dir:
        run_dir = Path(args.run_dir).expanduser().resolve()
    else:
        ap.print_help()
        print("\nProvide RUN_DIR or --latest", file=sys.stderr)
        return 1

    if not run_dir.is_dir():
        print(f"Not a directory: {run_dir}", file=sys.stderr)
        return 1

    meta = read_meta(run_dir)
    wall_map = read_wall_csv(run_dir)
    runs = discover_runs(run_dir)

    rows_out: list[dict[str, Any]] = []
    for query, ssd_mode, mode, threads, case_dir in runs:
        perf_path = case_dir / "perf_stat.txt"
        duck_path = case_dir / "duckdb.log"

        perf_fields: dict[str, Any] = {}
        if perf_path.is_file():
            perf_fields = parse_perf_stat(perf_path.read_text(encoding="utf-8", errors="replace"))

        wt = wall_map.get((query, ssd_mode, mode, threads))
        duck_total, duck_real = None, None
        if duck_path.is_file():
            duck_total, duck_real = parse_duckdb_wall(
                duck_path.read_text(encoding="utf-8", errors="replace")
            )

        row: dict[str, Any] = {
            "suite_tag": meta.get("SUITE_TAG", run_dir.name),
            "query": query,
            "ssd_mode": ssd_mode,
            "mode": mode,
            "threads": threads,
            "wall_time_s_csv": wt,
            "duckdb_total_time_s": duck_total,
            "duckdb_run_real_s": duck_real,
        }
        row.update({f"perf_{k}": v for k, v in perf_fields.items()})
        rows_out.append(row)

    # 对比：同一 query + ssd + 线程数下 double vs single wall_time（>1 表示 double 更慢）
    single_wall: dict[tuple[str, str, int], Any] = {}
    for r in rows_out:
        if r["mode"] == "singlebuffer":
            single_wall[(r["query"], r["ssd_mode"], r["threads"])] = r.get("wall_time_s_csv")
    for r in rows_out:
        ws = single_wall.get((r["query"], r["ssd_mode"], r["threads"]))
        wd = r.get("wall_time_s_csv") if r["mode"] == "doublebuffer" else None
        if r["mode"] == "doublebuffer" and ws is not None and wd is not None and ws > 0:
            r["wall_ratio_double_over_single"] = round(float(wd) / float(ws), 4)
        else:
            r["wall_ratio_double_over_single"] = ""

    csv_out = Path(args.csv_out) if args.csv_out else run_dir / "parsed_summary.csv"
    fieldnames = sorted({k for row in rows_out for k in row.keys()})
    # 稳定列顺序：常用字段靠前
    preferred = [
        "suite_tag",
        "query",
        "ssd_mode",
        "mode",
        "threads",
        "wall_time_s_csv",
        "duckdb_total_time_s",
        "duckdb_run_real_s",
        "wall_ratio_double_over_single",
        "perf_elapsed_mean_s",
        "perf_elapsed_std_s",
        "perf_task_clock_ms",
        "perf_cycles",
        "perf_instructions",
        "perf_ipc",
        "perf_cache_miss_pct",
        "perf_branch_miss_pct",
        "perf_page_faults",
        "perf_context_switches",
        "perf_cpu_migrations",
    ]
    ordered = [c for c in preferred if c in fieldnames]
    ordered += sorted(c for c in fieldnames if c not in ordered)

    with csv_out.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=ordered, extrasaction="ignore")
        w.writeheader()
        for row in rows_out:
            w.writerow({k: fmt_num(row.get(k)) if row.get(k) is not None else "" for k in ordered})

    print(f"Wrote {csv_out} ({len(rows_out)} rows)")

    if not args.no_md:
        md_path = csv_out.with_suffix(".md")
        lines = [
            f"# Parsed summary — `{run_dir.name}`",
            "",
            "## RUN_META",
            "",
        ]
        for k in sorted(meta.keys()):
            lines.append(f"- **{k}**: `{meta[k]}`")
        lines.extend(["", "## Wall time & perf (merged)", ""])

        table_cols = [
            "query",
            "ssd_mode",
            "mode",
            "threads",
            "wall_time_s_csv",
            "perf_elapsed_mean_s",
            "perf_ipc",
            "perf_page_faults",
            "perf_context_switches",
            "wall_ratio_double_over_single",
        ]
        header = "| " + " | ".join(table_cols) + " |"
        sep = "| " + " | ".join(["---"] * len(table_cols)) + " |"
        lines.append(header)
        lines.append(sep)
        for row in rows_out:
            cells = [fmt_num(row.get(c)) for c in table_cols]
            lines.append("| " + " | ".join(cells) + " |")
        lines.append("")
        try:
            md_path.write_text("\n".join(lines), encoding="utf-8")
            print(f"Wrote {md_path}")
        except OSError as e:
            print(f"Skipped markdown ({md_path}): {e}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
