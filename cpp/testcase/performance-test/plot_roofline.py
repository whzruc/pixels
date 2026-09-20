#!/usr/bin/env python3
"""
从 buffer-perf-concurrency-suite 结果目录计算 Roofline 数值并绘图。

用法:
  python3 plot_roofline.py results/jemalloc_offcpu
  python3 plot_roofline.py results/jemalloc_offcpu -o /tmp/roofline.png

输出:
  <run_dir>/roofline.png
  <run_dir>/roofline_metrics.csv
  <run_dir>/roofline_metrics.md
"""

from __future__ import annotations

import argparse
import csv
import re
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


ROWS_SCANNED = 1_299_967_461
STATIC_POOL_MB = 30713.6
PAGE_SIZE = 4096
IO_MODES = ("singlebuffer", "doublebuffer")
THREADS = (12, 24, 48)


@dataclass
class CaseMetrics:
    query: str
    ssd_mode: str
    mode: str
    threads: int
    wall_s: float
    instructions: float | None
    task_clock_ms: float | None
    cpu_util: float | None
    page_faults: float | None
    rows: int
    bytes_read: float
    bandwidth_gbs: float
    rows_per_s: float
    oi_inst_per_byte: float | None
    inst_rate_gs: float | None
    pct_of_beta_roof: float | None


def parse_perf_stat(text: str) -> dict[str, float]:
    out: dict[str, float] = {}
    m = re.search(r"([\d,]+(?:\.\d+)?)\s+msec\s+task-clock", text)
    if m:
        out["task_clock_ms"] = float(m.group(1).replace(",", ""))
    m = re.search(r"#\s*([\d.]+)\s+CPUs\s+utilized", text)
    if m:
        out["cpu_util"] = float(m.group(1))
    for line in text.splitlines():
        ls = line.strip()
        if ls.startswith("#") or not ls or ls.startswith("<"):
            continue
        m = re.match(r"^([\d,]+(?:\.\d+)?)\s+([\w\-]+)\s", ls)
        if not m:
            continue
        key = m.group(2).replace("-", "_")
        val = float(m.group(1).replace(",", ""))
        if key in ("instructions", "page_faults", "cycles"):
            out[key] = val
    return out


def read_wall_csv(run_dir: Path) -> dict[tuple[str, str, int], float]:
    p = run_dir / "summary_wall_time.csv"
    out: dict[tuple[str, str, int], float] = {}
    with p.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            mode = row.get("mode", "").strip()
            if mode not in IO_MODES:
                continue
            out[(row["query"].strip(), row["ssd_mode"].strip(), mode, int(row["threads"]))] = float(
                row["wall_time_s"]
            )
    return out


def discover_case_dir(run_dir: Path, query: str, ssd: str, mode: str, threads: int) -> Path | None:
    name = f"{query}_{ssd}_{mode}_t{threads}"
    p = run_dir / name
    return p if p.is_dir() else None


def estimate_bytes_read(page_faults: float | None) -> float:
    if page_faults is not None:
        return page_faults * PAGE_SIZE
    return STATIC_POOL_MB * 1024 * 1024


def compute_metrics(run_dir: Path) -> list[CaseMetrics]:
    walls = read_wall_csv(run_dir)
    rows: list[CaseMetrics] = []
    for (query, ssd, mode, threads), wall_s in sorted(walls.items()):
        case_dir = discover_case_dir(run_dir, query, ssd, mode, threads)
        perf: dict[str, float] = {}
        if case_dir and (case_dir / "perf_stat.txt").is_file():
            perf = parse_perf_stat((case_dir / "perf_stat.txt").read_text(encoding="utf-8", errors="replace"))
        instructions = perf.get("instructions")
        page_faults = perf.get("page_faults")
        task_clock_ms = perf.get("task_clock_ms")
        cpu_util = perf.get("cpu_util")
        if cpu_util is None and task_clock_ms is not None and wall_s > 0:
            cpu_util = (task_clock_ms / 1000.0) / wall_s

        bytes_read = estimate_bytes_read(page_faults)
        bandwidth_gbs = bytes_read / wall_s / 1e9
        rows_per_s = ROWS_SCANNED / wall_s
        oi = (instructions / bytes_read) if instructions is not None else None
        inst_rate_gs = (instructions / wall_s / 1e9) if instructions is not None else None
        rows.append(
            CaseMetrics(
                query=query,
                ssd_mode=ssd,
                mode=mode,
                threads=threads,
                wall_s=wall_s,
                instructions=instructions,
                task_clock_ms=task_clock_ms,
                cpu_util=cpu_util,
                page_faults=page_faults,
                rows=ROWS_SCANNED,
                bytes_read=bytes_read,
                bandwidth_gbs=bandwidth_gbs,
                rows_per_s=rows_per_s,
                oi_inst_per_byte=oi,
                inst_rate_gs=inst_rate_gs,
                pct_of_beta_roof=None,
            )
        )
    beta_roof = max(r.bandwidth_gbs for r in rows if r.mode == "doublebuffer")
    for r in rows:
        r.pct_of_beta_roof = 100.0 * r.bandwidth_gbs / beta_roof if beta_roof > 0 else None
    return rows


def estimate_roofs(metrics: list[CaseMetrics]) -> dict[str, float]:
    """估算 β（存储带宽屋顶）与 π（算力屋顶，GInst/s）。"""
    sat = max(
        (m for m in metrics if m.mode == "doublebuffer"),
        key=lambda m: m.bandwidth_gbs,
    )
    beta_gbs = sat.bandwidth_gbs

    # 用最高 CPU 利用率点反推「若 CPU 全满」的指令吞吐上限
    ref = max(
        (m for m in metrics if m.inst_rate_gs is not None and m.cpu_util),
        key=lambda m: m.inst_rate_gs / max(m.cpu_util or 1e-9, 1e-9),
    )
    pi_ginst_s = ref.inst_rate_gs / max(ref.cpu_util or 1.0, 1e-9)

    oi_mean = float(np.mean([m.oi_inst_per_byte for m in metrics if m.oi_inst_per_byte is not None]))
    ridge_oi = pi_ginst_s * 1e9 / (beta_gbs * 1e9)  # inst/s divided by bytes/s

    return {
        "beta_gbs": beta_gbs,
        "pi_ginst_s": pi_ginst_s,
        "oi_mean": oi_mean,
        "ridge_oi": ridge_oi,
        "sat_label": f"{sat.mode} t{sat.threads}",
    }


def write_metrics_csv(metrics: list[CaseMetrics], roofs: dict[str, float], path: Path) -> None:
    fields = [
        "query",
        "ssd_mode",
        "mode",
        "threads",
        "wall_s",
        "rows_per_s_M",
        "bandwidth_gbs",
        "pct_of_beta_roof",
        "oi_inst_per_byte",
        "inst_rate_ginst_s",
        "cpu_util",
        "instructions",
        "page_faults",
        "bytes_read_gib",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for m in metrics:
            w.writerow(
                {
                    "query": m.query,
                    "ssd_mode": m.ssd_mode,
                    "mode": m.mode,
                    "threads": m.threads,
                    "wall_s": f"{m.wall_s:.3f}",
                    "rows_per_s_M": f"{m.rows_per_s / 1e6:.2f}",
                    "bandwidth_gbs": f"{m.bandwidth_gbs:.3f}",
                    "pct_of_beta_roof": f"{m.pct_of_beta_roof:.1f}" if m.pct_of_beta_roof else "",
                    "oi_inst_per_byte": f"{m.oi_inst_per_byte:.3f}" if m.oi_inst_per_byte else "",
                    "inst_rate_ginst_s": f"{m.inst_rate_gs:.3f}" if m.inst_rate_gs else "",
                    "cpu_util": f"{m.cpu_util:.3f}" if m.cpu_util else "",
                    "instructions": f"{m.instructions:.0f}" if m.instructions else "",
                    "page_faults": f"{m.page_faults:.0f}" if m.page_faults else "",
                    "bytes_read_gib": f"{m.bytes_read / (1024**3):.2f}",
                }
            )


def write_metrics_md(metrics: list[CaseMetrics], roofs: dict[str, float], path: Path) -> None:
    lines = [
        "# Roofline 计算结果",
        "",
        "## 输入假设",
        f"- 扫描行数: {ROWS_SCANNED:,}",
        f"- 静态缓冲池: {STATIC_POOL_MB:.1f} MB",
        f"- 读取字节估算: page_faults × {PAGE_SIZE} B",
        "",
        "## 屋顶估算",
        f"- β 存储带宽屋顶: **{roofs['beta_gbs']:.3f} GB/s**（来自 {roofs['sat_label']}）",
        f"- π 算力屋顶: **{roofs['pi_ginst_s']:.3f} GInst/s**（由最高 CPU 利用率反推）",
        f"- 平均 OI: **{roofs['oi_mean']:.3f} inst/byte**",
        f"- Ridge Point OI\\*: **{roofs['ridge_oi']:.3f} inst/byte**",
        f"- 判定: OI={roofs['oi_mean']:.3f} < OI\\*={roofs['ridge_oi']:.3f} → **带宽限制区**",
        "",
        "## 各测点",
        "",
        "| mode | threads | wall(s) | rows/s(M) | BW(GB/s) | %β | OI | CPU util |",
        "|------|--------:|--------:|----------:|---------:|---:|---:|---------:|",
    ]
    for m in metrics:
        lines.append(
            f"| {m.mode} | {m.threads} | {m.wall_s:.3f} | {m.rows_per_s/1e6:.1f} | "
            f"{m.bandwidth_gbs:.3f} | {m.pct_of_beta_roof:.1f} | "
            f"{m.oi_inst_per_byte:.3f} | {m.cpu_util:.3f} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def plot_roofline(metrics: list[CaseMetrics], roofs: dict[str, float], out_png: Path) -> None:
    beta = roofs["beta_gbs"]
    pi = roofs["pi_ginst_s"]
    oi_mean = roofs["oi_mean"]
    ridge_oi = roofs["ridge_oi"]

    x_min, x_max = 0.05, max(5.0, ridge_oi * 3)
    x = np.logspace(np.log10(x_min), np.log10(x_max), 200)

    # y = GB/s; compute roof: inst/s = y * 1e9 * x  =>  y = pi*1e9 / (x*1e9) = pi/x
    y_mem = np.full_like(x, beta)
    y_comp = pi / x

    fig, ax = plt.subplots(figsize=(10, 6.5), dpi=150)

    ax.loglog(x, y_mem, "k-", lw=2.2, label=f"β roof = {beta:.2f} GB/s (storage)")
    ax.loglog(x, y_comp, color="#555555", ls="--", lw=2.0, label=f"π roof = {pi:.2f}/OI GB/s (compute)")
    ax.loglog(
        x,
        np.minimum(y_mem, y_comp),
        color="#888888",
        ls="-.",
        lw=1.5,
        alpha=0.8,
        label="Effective roof = min(β, π/OI)",
    )

    styles = {
        "doublebuffer": {"color": "#1f77b4", "marker": "o"},
        "singlebuffer": {"color": "#ff7f0e", "marker": "s"},
    }
    for m in metrics:
        if m.oi_inst_per_byte is None:
            continue
        st = styles[m.mode]
        ax.loglog(
            m.oi_inst_per_byte,
            m.bandwidth_gbs,
            marker=st["marker"],
            color=st["color"],
            ms=9,
            mew=1.2,
            mec="white",
            label=f"{m.mode} t{m.threads}",
        )
        ax.annotate(
            f"t{m.threads}",
            (m.oi_inst_per_byte, m.bandwidth_gbs),
            textcoords="offset points",
            xytext=(6, 4),
            fontsize=8,
            color=st["color"],
        )

    ax.axvline(oi_mean, color="#2ca02c", ls=":", lw=1.5, alpha=0.8, label=f"mean OI = {oi_mean:.2f}")
    ax.axvline(ridge_oi, color="#d62728", ls=":", lw=1.5, alpha=0.8, label=f"ridge OI* = {ridge_oi:.2f}")

    ax.set_xlabel("Operational Intensity (instructions / byte read)")
    ax.set_ylabel("Achieved Read Bandwidth (GB/s)")
    ax.set_title("Roofline: q45 / 24ssd / jemalloc (singlebuffer vs doublebuffer)")
    ax.grid(True, which="both", ls=":", alpha=0.35)
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(0.5, beta * 1.35)

    text = (
        f"Rows scanned: {ROWS_SCANNED/1e9:.3f}B\n"
        f"Bytes/read ≈ {metrics[0].bytes_read/(1024**3):.2f} GiB\n"
        f"β={beta:.2f} GB/s, π={pi:.2f} GInst/s\n"
        f"OI={oi_mean:.2f} inst/B < OI*={ridge_oi:.2f} → bandwidth-bound"
    )
    ax.text(
        0.02,
        0.98,
        text,
        transform=ax.transAxes,
        va="top",
        ha="left",
        fontsize=9,
        bbox=dict(boxstyle="round", facecolor="wheat", alpha=0.85),
    )

    handles, labels = ax.get_legend_handles_labels()
    # dedupe legend
    seen = set()
    h2, l2 = [], []
    for h, l in zip(handles, labels):
        if l in seen:
            continue
        seen.add(l)
        h2.append(h)
        l2.append(l)
    ax.legend(h2, l2, loc="lower left", fontsize=8, ncol=2)

    fig.tight_layout()
    fig.savefig(out_png, bbox_inches="tight")
    plt.close(fig)


def resolve_out_dir(run_dir: Path, out_dir: Path | None, out_png: Path | None) -> Path:
    if out_dir is not None:
        return out_dir.resolve()
    if out_png is not None:
        return out_png.resolve().parent
    candidate = run_dir / "roofline_out"
    try:
        candidate.mkdir(exist_ok=True)
        probe = candidate / ".write_probe"
        probe.write_text("ok", encoding="utf-8")
        probe.unlink(missing_ok=True)
        return candidate
    except OSError:
        fallback = Path("/tmp/pixels_roofline")
        fallback.mkdir(parents=True, exist_ok=True)
        return fallback


def main() -> None:
    ap = argparse.ArgumentParser(description="Plot Roofline from suite results")
    ap.add_argument("run_dir", type=Path, help="e.g. results/jemalloc_offcpu")
    ap.add_argument(
        "-o",
        "--output",
        type=Path,
        help="PNG path (default: <out_dir>/roofline.png)",
    )
    ap.add_argument(
        "--out-dir",
        type=Path,
        help="Directory for PNG/CSV/MD (default: <run_dir>/roofline_out or /tmp/pixels_roofline)",
    )
    args = ap.parse_args()
    run_dir = args.run_dir.resolve()
    if not (run_dir / "summary_wall_time.csv").is_file():
        raise SystemExit(f"missing summary_wall_time.csv in {run_dir}")

    metrics = compute_metrics(run_dir)
    if not metrics:
        raise SystemExit("no io_uring single/double buffer metrics found")

    roofs = estimate_roofs(metrics)
    out_dir = resolve_out_dir(run_dir, args.out_dir, args.output)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_png = args.output or (out_dir / "roofline.png")
    out_csv = out_dir / "roofline_metrics.csv"
    out_md = out_dir / "roofline_metrics.md"

    write_metrics_csv(metrics, roofs, out_csv)
    write_metrics_md(metrics, roofs, out_md)
    plot_roofline(metrics, roofs, out_png)

    print(f"Wrote {out_png}")
    print(f"Wrote {out_csv}")
    print(f"Wrote {out_md}")
    print()
    print("Roof summary:")
    for k, v in roofs.items():
        if k != "sat_label":
            print(f"  {k}: {v:.4f}" if isinstance(v, float) else f"  {k}: {v}")
        else:
            print(f"  {k}: {v}")
    print()
    print("Sample points (doublebuffer):")
    for m in metrics:
        if m.mode != "doublebuffer":
            continue
        print(
            f"  t{m.threads:2d}: {m.bandwidth_gbs:.3f} GB/s, "
            f"{m.rows_per_s/1e6:.1f} M rows/s, {m.pct_of_beta_roof:.1f}% of β"
        )


if __name__ == "__main__":
    main()
