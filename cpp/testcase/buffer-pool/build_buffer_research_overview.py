#!/usr/bin/env python3
"""Build the compact buffer research overview figures from archived evidence."""

from __future__ import annotations

import argparse
import csv
import re
import tarfile
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch, Rectangle


ARCHIVE_REPORT = (
    "testcase/performance-test/"
    "comparison_spdk_20260715_vs_iouring_20260716/EXPERIMENT_REPORT.md"
)


def read_archive_report(archive: Path) -> str:
    with tarfile.open(archive, "r:gz") as bundle:
        member = bundle.getmember(ARCHIVE_REPORT)
        stream = bundle.extractfile(member)
        if stream is None:
            raise RuntimeError(f"cannot read {ARCHIVE_REPORT} from {archive}")
        return stream.read().decode("utf-8")


def parse_archive_evidence(text: str):
    q24_pattern = re.compile(
        r"double buffer在12/24/48线程下相对single的加速分别为："
        r"Pixels ([\d.]+)/([\d.]+)/([\d.]+)×，"
        r"SPDK ([\d.]+)/([\d.]+)/([\d.]+)×，"
        r"Parquet ([\d.]+)/([\d.]+)/([\d.]+)×"
    )
    match = q24_pattern.search(text)
    if not match:
        raise RuntimeError("q24 cross-backend speedup sentence is missing from archive report")
    values = [float(value) for value in match.groups()]
    q24 = {
        "Pixels / io_uring": values[0:3],
        "SPDK": values[3:6],
        "Parquet / io_uring": values[6:9],
    }

    broad = {}
    for backend in ("SPDK", "io_uring", "pread"):
        row = re.search(
            rf"\| {re.escape(backend)} \| "
            r"([\d.]+)× \| ([\d.]+)× \| ([\d.]+)× \| ([\d.]+)× \| ([\d.]+)× \|",
            text,
        )
        if not row:
            raise RuntimeError(f"44-query {backend} row is missing from archive report")
        broad[backend] = [float(value) for value in row.groups()]
    return q24, broad


def load_pool_summary(path: Path):
    with path.open(newline="") as stream:
        return {row["mode"]: row for row in csv.DictReader(stream)}


def save(fig, output: Path, stem: str):
    fig.savefig(output / f"{stem}.png", dpi=240, bbox_inches="tight")
    fig.savefig(output / f"{stem}.svg", bbox_inches="tight")
    plt.close(fig)


def plot_backend_evidence(q24, broad, output: Path):
    colors = {
        "Pixels / io_uring": "#2878B5",
        "SPDK": "#D95F02",
        "Parquet / io_uring": "#2A9D8F",
        "io_uring": "#2878B5",
    }
    fig, axes = plt.subplots(1, 2, figsize=(10.8, 3.8), constrained_layout=True)

    threads_q24 = [12, 24, 48]
    for backend, ratios in q24.items():
        axes[0].plot(
            threads_q24, ratios, marker="o", linewidth=2.2, markersize=6,
            color=colors[backend], label=backend,
        )
    axes[0].axhline(1.0, color="#666666", linewidth=1, linestyle="--")
    axes[0].set_title("(a) q24, 24 SSD (profiler run)")
    axes[0].set_xlabel("Worker threads")
    axes[0].set_ylabel("Speedup: single / double")
    axes[0].set_xticks(threads_q24)
    axes[0].set_ylim(0.99, 1.24)
    axes[0].grid(axis="y", alpha=0.22)
    axes[0].legend(frameon=False, fontsize=8)

    threads_all = [4, 8, 12, 24, 48]
    for backend in ("SPDK", "io_uring"):
        axes[1].plot(
            threads_all, broad[backend], marker="o", linewidth=2.2, markersize=6,
            color=colors[backend], label=backend,
        )
    axes[1].axhline(1.0, color="#666666", linewidth=1, linestyle="--")
    axes[1].set_title("(b) 44-query geometric mean, 24 SSD")
    axes[1].set_xlabel("Worker threads")
    axes[1].set_ylabel("Speedup: single / double")
    axes[1].set_xticks(threads_all)
    axes[1].set_ylim(0.99, 1.20)
    axes[1].grid(axis="y", alpha=0.22)
    axes[1].legend(frameon=False, fontsize=8)

    fig.suptitle("Double buffering benefits asynchronous scan paths", fontsize=13, weight="bold")
    save(fig, output, "01-doublebuffer-cross-backend")


def rounded_box(ax, xy, width, height, text, face, edge="#34495E", size=9):
    box = FancyBboxPatch(
        xy, width, height, boxstyle="round,pad=0.018,rounding_size=0.025",
        linewidth=1.2, edgecolor=edge, facecolor=face,
    )
    ax.add_patch(box)
    ax.text(xy[0] + width / 2, xy[1] + height / 2, text,
            ha="center", va="center", fontsize=size)
    return box


def arrow(ax, start, end, color="#4D4D4D", style="-|>"):
    ax.add_patch(FancyArrowPatch(start, end, arrowstyle=style,
                                mutation_scale=12, linewidth=1.25, color=color))


def plot_architecture(output: Path):
    fig, ax = plt.subplots(figsize=(11.2, 4.5), constrained_layout=True)
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis("off")

    # Timeline: why two buffers help.
    ax.text(0.02, 0.93, "A. Pipeline overlap", fontsize=11, weight="bold")
    ax.text(0.02, 0.79, "Single", fontsize=9, ha="left", va="center")
    ax.text(0.02, 0.66, "Double", fontsize=9, ha="left", va="center")
    x0, unit, height = 0.10, 0.095, 0.075
    for i, (label, color) in enumerate((("I/O 1", "#78B7C5"), ("CPU 1", "#E6A157"),
                                        ("I/O 2", "#78B7C5"), ("CPU 2", "#E6A157"))):
        ax.add_patch(Rectangle((x0 + i * unit, 0.75), unit, height,
                               facecolor=color, edgecolor="white"))
        ax.text(x0 + (i + .5) * unit, 0.787, label, ha="center", va="center", fontsize=8)
    half = height * 0.58
    ax.add_patch(Rectangle((x0, 0.63), unit, half, facecolor="#78B7C5", edgecolor="white"))
    ax.add_patch(Rectangle((x0 + unit, 0.66), unit, half, facecolor="#E6A157", edgecolor="white"))
    ax.add_patch(Rectangle((x0 + unit, 0.60), unit, half, facecolor="#78B7C5", edgecolor="white", hatch="//"))
    ax.add_patch(Rectangle((x0 + 2 * unit, 0.66), unit, half, facecolor="#E6A157", edgecolor="white"))
    ax.text(x0 + .5 * unit, 0.651, "I/O 1", ha="center", va="center", fontsize=8)
    ax.text(x0 + 1.5 * unit, 0.681, "CPU 1", ha="center", va="center", fontsize=8)
    ax.text(x0 + 1.5 * unit, 0.621, "prefetch 2", ha="center", va="center", fontsize=7)
    ax.text(x0 + 2.5 * unit, 0.681, "CPU 2", ha="center", va="center", fontsize=8)
    ax.text(0.10, 0.54, "Two slots expose I/O–decode/compute overlap", fontsize=8, color="#555555")

    # Selective routing: how to retain overlap without universal growth.
    ax.text(0.52, 0.93, "B. Capacity-aware selective routing", fontsize=11, weight="bold")
    rounded_box(ax, (0.53, 0.73), 0.15, 0.12, "Metadata\nchunk demand", "#EAF2F8")
    rounded_box(ax, (0.74, 0.73), 0.20, 0.12, "Capacity + queue\nscheduler", "#FFF2CC")
    arrow(ax, (0.68, 0.79), (0.74, 0.79))

    worker_x = [0.53, 0.69, 0.85]
    labels = ["Worker 0\nsmall slots", "Worker 1\nlarge slots", "Worker 2\nmedium slots"]
    faces = ["#FDEDEC", "#E8F8F5", "#F4ECF7"]
    for x, label, face in zip(worker_x, labels, faces):
        rounded_box(ax, (x, 0.47), 0.13, 0.13, label, face, size=8)
        arrow(ax, (0.84, 0.73), (x + 0.065, 0.60), color="#7F8C8D")
    ax.text(0.735, 0.39, "Reuse a compatible worker; grow locally only when routing is costly",
            ha="center", fontsize=8, color="#555555")

    # Backend seam.
    ax.plot([0.04, 0.96], [0.29, 0.29], color="#AAB7B8", linewidth=1)
    ax.text(0.04, 0.24, "Backend-neutral policy contract", fontsize=9, weight="bold")
    rounded_box(ax, (0.22, 0.16), 0.18, 0.09, "async read / completion", "#F7F9F9", size=8)
    rounded_box(ax, (0.43, 0.16), 0.16, 0.09, "buffer demand", "#F7F9F9", size=8)
    rounded_box(ax, (0.62, 0.16), 0.16, 0.09, "buffer ownership", "#F7F9F9", size=8)
    ax.text(0.11, 0.075, "Reader / I/O adapters", ha="center", va="center",
            fontsize=8, color="#555555")
    for x, text_value in ((0.31, "Pixels / io_uring"), (0.52, "Pixels / SPDK"),
                          (0.73, "Parquet / io_uring")):
        ax.text(x, 0.075, text_value, ha="center", va="center", fontsize=9,
                bbox=dict(boxstyle="round,pad=.3", facecolor="#D6EAF8", edgecolor="#5D6D7E"))
    ax.text(0.91, 0.075, "same policy", ha="center", va="center", fontsize=9,
            color="#1B4F72", weight="bold")
    arrow(ax, (0.82, 0.075), (0.86, 0.075), color="#1B4F72")

    save(fig, output, "02-design-overview")


def plot_tradeoff(summary, output: Path):
    chosen = {
        "legacy": ("Legacy (oracle)", "#7F8C8D", "s"),
        "dynamic": ("Dynamic", "#D95F02", "o"),
        "selective-4": ("Selective V1", "#2A9D8F", "D"),
        "static-lockfree": ("Static (preallocated)", "#6C5CE7", "^"),
    }
    fig, ax = plt.subplots(figsize=(7.2, 4.5), constrained_layout=True)
    for mode, (label, color, marker) in chosen.items():
        row = summary[mode]
        x = float(row["median_rss_gib"])
        y = 1.0 / float(row["geomean_vs_dynamic"])
        ax.scatter(x, y, s=105, marker=marker, color=color, edgecolor="white", linewidth=0.8, zorder=3)
        dx, dy = {
            "legacy": (0.45, -0.012), "dynamic": (0.45, -0.012),
            "selective-4": (0.45, 0.014), "static-lockfree": (-8.4, -0.014),
        }[mode]
        ax.annotate(label, (x, y), xytext=(x + dx, y + dy), fontsize=9)
    ax.axhline(1.0, color="#777777", linestyle="--", linewidth=1)
    ax.set_xlabel("Median peak process RSS across 44 queries (GiB)")
    ax.set_ylabel("Query speedup vs Dynamic")
    ax.set_title("Performance–memory trade-off (16 SSD, 48 workers)", weight="bold")
    ax.grid(alpha=0.22)
    ax.set_xlim(5.5, 34)
    ax.set_ylim(0.96, 1.38)
    ax.text(6.2, 0.977, "lower memory", color="#555555", fontsize=8)
    ax.text(29.0, 1.355, "faster", color="#555555", fontsize=8)
    save(fig, output, "03-buffer-pool-tradeoff")


def write_evidence_csv(q24, broad, output: Path, archive: Path):
    with (output / "doublebuffer-archive-evidence.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["scope", "backend", "threads", "single_over_double", "archive", "member"])
        for backend, values in q24.items():
            for threads, value in zip((12, 24, 48), values):
                writer.writerow(["q24-profiler", backend, threads, value, archive, ARCHIVE_REPORT])
        for backend, values in broad.items():
            for threads, value in zip((4, 8, 12, 24, 48), values):
                writer.writerow(["44-query-geomean", backend, threads, value, archive, ARCHIVE_REPORT])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument(
        "--pool-summary", type=Path,
        default=Path("docs/buffer-pool/figures/selective-v2-16ssd-results/aggregate-mode-summary.csv"),
    )
    parser.add_argument(
        "--output", type=Path,
        default=Path("docs/buffer-pool/figures/buffer-research-overview"),
    )
    args = parser.parse_args()
    if not args.archive.is_file():
        raise FileNotFoundError(args.archive)
    args.output.mkdir(parents=True, exist_ok=True)

    report = read_archive_report(args.archive)
    q24, broad = parse_archive_evidence(report)
    summary = load_pool_summary(args.pool_summary)
    plot_backend_evidence(q24, broad, args.output)
    plot_architecture(args.output)
    plot_tradeoff(summary, args.output)
    write_evidence_csv(q24, broad, args.output, args.archive)


if __name__ == "__main__":
    main()
