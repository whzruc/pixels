#!/usr/bin/env python3
"""Parse q24 profiler logs and generate comparison figures."""

import argparse
import csv
import re
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


THREADS = [12, 24, 48]
COLORS = plt.get_cmap("tab20").colors
ENGINE_NAME = {"pixels": "Pixels", "parquet": "Parquet", "spdk": "SPDK"}


def latest(root: Path, prefix: str) -> Path:
    matches = sorted(p for p in root.glob(prefix + "-*") if p.is_dir())
    if not matches:
        raise FileNotFoundError(f"no suite matching {prefix}-* under {root}")
    return matches[-1]


def all_suites(root: Path, prefix: str):
    return sorted(p for p in root.glob(prefix + "-*") if p.is_dir())


def read_wall_time(suite: Path):
    with (suite / "summary_wall_time.csv").open(newline="") as f:
        return list(csv.DictReader(f))


def parse_profile(log: Path):
    values = {}
    section = ""
    for line in log.read_text(errors="replace").splitlines():
        match = re.match(r"^=== (.+ Profile Summary) ===$", line)
        if match:
            section = match.group(1)
            continue
        match = re.match(r"^([^,]+),([0-9.]+),([0-9.]+)$", line)
        if match and match.group(1) not in {"label", "base_total"}:
            values[match.group(1)] = {
                "seconds": float(match.group(2)),
                "ratio": float(match.group(3)),
                "section": section,
            }
    return values


def parse_perf_stat(path: Path):
    """Average the repeated perf-stat runs into derived, comparable metrics."""
    runs = []
    current = {}
    number = r"([0-9][0-9,]*(?:\.[0-9]+)?)"
    for line in path.read_text(errors="replace").splitlines():
        task_clock = re.match(rf"\s*{number}\s+msec task-clock", line)
        if task_clock:
            current["task-clock"] = float(task_clock.group(1).replace(",", ""))
            continue
        match = re.match(rf"\s*{number}\s+([A-Za-z0-9_-]+)", line)
        if match:
            current[match.group(2)] = float(match.group(1).replace(",", ""))
        elapsed = re.match(rf"\s*{number}\s+seconds time elapsed", line)
        if elapsed:
            current["elapsed_s"] = float(elapsed.group(1).replace(",", ""))
            runs.append(current)
            current = {}
    if not runs:
        return {}
    def mean(key):
        values = [run[key] for run in runs if key in run]
        return float(np.mean(values)) if values else np.nan
    cycles, instructions = mean("cycles"), mean("instructions")
    cache_refs, cache_misses = mean("cache-references"), mean("cache-misses")
    elapsed, task_clock = mean("elapsed_s"), mean("task-clock")
    return {
        "repeats": len(runs),
        "elapsed_s": elapsed,
        "ipc": instructions / cycles,
        "cache_miss_pct": cache_misses / cache_refs * 100.0,
        "cpus_utilized": task_clock / (elapsed * 1000.0),
        "context_switches_per_s": mean("context-switches") / elapsed,
        "page_faults_per_s": mean("page-faults") / elapsed,
        "cpu_migrations_per_s": mean("cpu-migrations") / elapsed,
    }


def collect_profiles(suite: Path, engine: str, ssd_mode: str):
    rows = []
    for log in sorted(suite.glob(f"q24_{ssd_mode}_*_t*/duckdb.log")):
        match = re.match(rf"q24_{re.escape(ssd_mode)}_(.+)_t(\d+)$", log.parent.name)
        if not match:
            continue
        mode, threads = match.group(1), int(match.group(2))
        values = parse_profile(log)
        for label, value in values.items():
            rows.append(
                {
                    "engine": engine,
                    "mode": mode,
                    "threads": threads,
                    "label": label,
                    **value,
                }
            )
    return rows


def save_csv(path: Path, rows, fields):
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def wall_index(rows):
    return {(r["engine"], r["kind"], r["mode"], int(r["threads"])): float(r["wall_time_s"]) for r in rows}


def plot_wall_time(output: Path, wall_rows, ssd_mode: str):
    by_engine = {
        "Pixels": ["singlebuffer", "doublebuffer", "nonfixed", "pread-singlebuffer", "pread-doublebuffer"],
        "SPDK": ["spdk", "spdk-doublebuffer"],
        "Parquet": ["pq-async-singlebuffer", "pq-async-doublebuffer", "pq-pread"],
    }
    idx = wall_index(wall_rows)
    fig, axes = plt.subplots(1, 3, figsize=(16, 4.8), sharex=True)
    for ax, (engine, modes) in zip(axes, by_engine.items()):
        for mode in modes:
            y = [idx.get((engine, "profiler", mode, t), np.nan) for t in THREADS]
            ax.plot(THREADS, y, marker="o", linewidth=2, label=mode)
        ax.set_title(engine)
        ax.set_xlabel("Threads")
        ax.grid(alpha=0.25)
        ax.legend(fontsize=8)
    axes[0].set_ylabel("Wall time (s), lower is better")
    fig.suptitle(f"Q24 / {ssd_mode.removesuffix('ssd')} SSD: runtime with profiler enabled")
    fig.tight_layout()
    fig.savefig(output / "01_profiler_wall_time.png", dpi=180)
    plt.close(fig)


def plot_best_path(output: Path, wall_rows):
    idx = wall_index(wall_rows)
    selected = [
        ("Pixels doublebuffer", "Pixels", "doublebuffer"),
        ("SPDK doublebuffer", "SPDK", "spdk-doublebuffer"),
        ("Parquet async doublebuffer", "Parquet", "pq-async-doublebuffer"),
    ]
    fig, ax = plt.subplots(figsize=(8.5, 5))
    for label, engine, mode in selected:
        ax.plot(
            THREADS,
            [idx[(engine, "profiler", mode, t)] for t in THREADS],
            marker="o",
            linewidth=2.4,
            label=label,
        )
    ax.set(title="Best buffered path per engine", xlabel="Threads", ylabel="Wall time (s)")
    ax.grid(alpha=0.25)
    ax.legend()
    fig.tight_layout()
    fig.savefig(output / "02_best_path_comparison.png", dpi=180)
    plt.close(fig)


def profile_index(rows):
    return {(r["engine"], r["mode"], int(r["threads"]), r["label"]): float(r["seconds"]) for r in rows}


def stacked_stage_plot(output, rows, engine, modes, labels, names, filename, title):
    idx = profile_index(rows)
    columns = [(mode, t) for mode in modes for t in THREADS]
    x = np.arange(len(columns))
    bottom = np.zeros(len(columns))
    fig, ax = plt.subplots(figsize=(max(10, len(columns) * 0.85), 5.5))
    for i, (label, name) in enumerate(zip(labels, names)):
        vals = np.array([idx.get((engine, mode, t, label), 0.0) for mode, t in columns])
        ax.bar(x, vals, bottom=bottom, label=name, color=COLORS[i])
        bottom += vals
    ax.set_xticks(x, [f"{m}\n{t}t" for m, t in columns], rotation=30, ha="right")
    ax.set_ylabel("Aggregated thread time (s)")
    ax.set_title(title)
    ax.grid(axis="y", alpha=0.2)
    ax.legend(fontsize=8, ncol=2)
    fig.tight_layout()
    fig.savefig(output / filename, dpi=180)
    plt.close(fig)


def plot_overhead(output: Path, wall_rows):
    idx = wall_index(wall_rows)
    items = sorted({(r["engine"], r["mode"], int(r["threads"])) for r in wall_rows})
    labels, values, colors = [], [], []
    engine_colors = {"Pixels": "#4c78a8", "SPDK": "#f58518", "Parquet": "#54a24b"}
    for engine, mode, threads in items:
        base = idx.get((engine, "baseline", mode, threads))
        prof = idx.get((engine, "profiler", mode, threads))
        if base is None or prof is None:
            continue
        labels.append(f"{mode}\n{threads}t")
        values.append((prof / base - 1.0) * 100.0)
        colors.append(engine_colors[engine])
    fig, ax = plt.subplots(figsize=(16, 5.5))
    ax.bar(np.arange(len(values)), values, color=colors)
    ax.axhline(0, color="black", linewidth=0.8)
    ax.set_xticks(np.arange(len(labels)), labels, rotation=50, ha="right", fontsize=8)
    ax.set(ylabel="Profiler run vs baseline (%)", title="Observed profiler overhead / run-to-run variance")
    ax.grid(axis="y", alpha=0.2)
    fig.tight_layout()
    fig.savefig(output / "05_profiler_overhead.png", dpi=180)
    plt.close(fig)


def plot_perf_metrics(output: Path, rows):
    metrics = [
        ("ipc", "Instructions per cycle"),
        ("cache_miss_pct", "Cache misses / references (%)"),
        ("cpus_utilized", "Average CPUs utilized"),
        ("context_switches_per_s", "Context switches / s"),
    ]
    fig, axes = plt.subplots(2, 2, figsize=(14, 9))
    styles = {"Pixels": "o", "SPDK": "s", "Parquet": "^"}
    for ax, (metric, title) in zip(axes.flat, metrics):
        for row in rows:
            ax.scatter(row["threads"], row[metric], marker=styles[row["engine"]],
                       label=f'{row["engine"]} {row["mode"]}', alpha=.8)
        ax.set(title=title, xlabel="Threads")
        ax.set_xticks(THREADS)
        ax.grid(alpha=.2)
    handles, labels = axes.flat[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="center right", fontsize=7)
    fig.tight_layout(rect=(0, 0, .82, 1))
    fig.savefig(output / "08_perf_stat_metrics.png", dpi=180)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).parent / "results/performance-comparsion")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--comparison-root", type=Path,
                        help="run_all_comparison.sh output containing *-pixels/*-parquet/*-spdk")
    parser.add_argument("--ssd-mode", default="6ssd")
    args = parser.parse_args()
    root = args.root.resolve()
    output = (args.output or root / "profiler-analysis").resolve()
    output.mkdir(parents=True, exist_ok=True)

    # Merge profiler suites so a targeted rerun (for example only pread modes)
    # augments, rather than replaces, the earlier complete suite. Newer suite
    # values win when the same mode/thread combination is present.
    wall_by_key = {}
    profile_by_key = {}
    perf_rows = []
    for engine in ("pixels", "parquet", "spdk"):
        if args.comparison_root:
            comparison_root = args.comparison_root.resolve()
            suites = sorted(comparison_root.glob(f"*-{engine}"))
            if not suites:
                raise FileNotFoundError(f"no *-{engine} suite under {comparison_root}")
            grouped = [("profiler", suites)]
        else:
            grouped = [
                ("baseline", [latest(root, f"quick-{engine}-q24-{args.ssd_mode}")]),
                ("profiler", all_suites(root, f"profiler-{engine}-q24-{args.ssd_mode}")),
            ]
        for kind, suites in grouped:
            for suite in suites:
                for row in read_wall_time(suite):
                    item = {"engine": ENGINE_NAME[engine], "kind": kind, "suite": suite.name, **row}
                    wall_by_key[(item["engine"], kind, item["mode"], int(item["threads"]))] = item
                if kind == "profiler":
                    for item in collect_profiles(suite, ENGINE_NAME[engine], args.ssd_mode):
                        profile_by_key[
                            (item["engine"], item["mode"], item["threads"], item["label"])
                        ] = item
                    for row in read_wall_time(suite):
                        case = suite / f'{row["query"]}_{row["ssd_mode"]}_{row["mode"]}_t{row["threads"]}'
                        perf_file = case / "perf_stat.txt"
                        if perf_file.is_file():
                            perf_rows.append({
                                "engine": ENGINE_NAME[engine], "mode": row["mode"],
                                "threads": int(row["threads"]), "case": case.name,
                                **parse_perf_stat(perf_file),
                            })
    wall_rows = list(wall_by_key.values())
    profile_rows = list(profile_by_key.values())

    save_csv(
        output / "wall_time_long.csv",
        wall_rows,
        [
            "engine", "kind", "suite", "query", "ssd_mode", "mode", "threads",
            "wall_time_s", "shell_wall_time_s", "note",
        ],
    )
    if perf_rows:
        save_csv(
            output / "perf_stat_summary.csv", perf_rows,
            ["engine", "mode", "threads", "case", "repeats", "elapsed_s", "ipc",
             "cache_miss_pct", "cpus_utilized", "context_switches_per_s",
             "page_faults_per_s", "cpu_migrations_per_s"],
        )
    save_csv(
        output / "profiler_stages_long.csv",
        profile_rows,
        ["engine", "mode", "threads", "label", "seconds", "ratio", "section"],
    )

    plot_wall_time(output, wall_rows, args.ssd_mode)
    plot_best_path(output, wall_rows)
    stacked_stage_plot(
        output,
        profile_rows,
        "Parquet",
        ["pq-async-singlebuffer", "pq-async-doublebuffer", "pq-pread"],
        [
            "Parquet.Decode.ReadNext",
            "Parquet.Convert.ArrowToDuckDB",
            "Parquet.Decode.ExportBatch",
            "Parquet.StateTransition.Total",
        ],
        ["Decode ReadNext", "Arrow→DuckDB", "ExportBatch", "I/O + reader transition"],
        "03_parquet_stage_breakdown.png",
        "Parquet pipeline: aggregated thread-time breakdown",
    )
    stacked_stage_plot(
        output,
        profile_rows,
        "SPDK",
        ["spdk", "spdk-doublebuffer"],
        [
            "Spdk.SyncRead.Poll",
            "Spdk.AsyncComplete.Poll",
            "Spdk.BufferPool.Initialize.Allocate",
            "Spdk.SyncRead.DmaAllocate",
            "Spdk.SyncRead.DmaFree",
            "Spdk.SyncRead.Copy",
            "Spdk.AsyncRead.Submit",
        ],
        ["Sync poll", "Async completion poll", "Pool allocation", "DMA alloc", "DMA free", "Copy", "Async submit"],
        "04_spdk_stage_breakdown.png",
        "SPDK I/O: selected leaf-stage thread times",
    )
    pixels_mode_order = [
        "singlebuffer",
        "doublebuffer",
        "nonfixed",
        "pread-singlebuffer",
        "pread-doublebuffer",
    ]
    pixels_available = {
        r["mode"]
        for r in profile_rows
        if r["engine"] == "Pixels"
        and r["label"] == "PixelsScanFunction.PixelsScanImplementation.Total"
    }
    stacked_stage_plot(
        output,
        profile_rows,
        "Pixels",
        [mode for mode in pixels_mode_order if mode in pixels_available],
        [
            "PixelsScanFunction.Stage.HandleFileBoundary",
            "PixelsScanFunction.Stage.AcquireBatch",
            "PixelsScanFunction.Stage.TransformOutput",
            "PixelsScanFunction.Stage.ApplyFilter",
            "PixelsScanFunction.Stage.AdvanceState",
        ],
        ["File/state transition", "Acquire batch", "Transform output", "Apply filter", "Advance state"],
        "06_pixels_stage_breakdown.png",
        "Pixels pipeline: aggregated thread-time breakdown",
    )
    stacked_stage_plot(
        output,
        profile_rows,
        "Pixels",
        ["pread-singlebuffer", "pread-doublebuffer"],
        [
            "Pixels.Pread.ReadFullyReuse.DirectIO",
            "Pixels.Pread.ReadFully.DirectIO",
            "Pixels.Pread.Metadata.DirectIO",
            "Pixels.Pread.ReadFullyReuse.Syscall",
            "Pixels.Pread.ReadFully.Syscall",
            "Pixels.Pread.Metadata.Syscall",
            "Pixels.Pread.ReadFully.Allocate",
        ],
        [
            "Reuse-buffer direct I/O",
            "Fresh-buffer direct I/O",
            "Metadata direct I/O",
            "Reuse-buffer pread",
            "Fresh-buffer pread",
            "Metadata pread",
            "Allocate",
        ],
        "07_pixels_pread_io_breakdown.png",
        "Pixels synchronous read: selected leaf-stage thread times",
    )
    plot_overhead(output, wall_rows)
    if perf_rows:
        plot_perf_metrics(output, perf_rows)
    print(f"Generated analysis in {output}")


if __name__ == "__main__":
    main()
