#!/usr/bin/env python3
"""Generate reproducible CSV tables and paper-style figures for selective V1."""
import argparse
import collections
import csv
import json
import math
from pathlib import Path
import re
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

MODES = ["legacy", "dynamic", "selective-0", "selective-1",
         "selective-2", "selective-4", "static"]
SELECTIVE = ["selective-0", "selective-1", "selective-2", "selective-4"]
COLORS = {
    "legacy": "#56657a", "dynamic": "#c44e52", "selective-0": "#8da0b6",
    "selective-1": "#4c78a8", "selective-2": "#2a9d8f",
    "selective-4": "#157a6e", "static": "#e09f3e",
}


def latest_complete(base):
    candidates = []
    for success in base.glob("*/SUCCESS"):
        manifest = success.parent / "manifest.json"
        if manifest.exists():
            args = json.loads(manifest.read_text())["args"]
            if args.get("ssds") == 16 and len(args.get("queries", [])) == 21:
                candidates.append((success.stat().st_mtime, success.parent))
    if not candidates:
        raise RuntimeError("no completed 16-SSD, 21-query result found")
    return max(candidates)[1]


def median_rows(root):
    groups = collections.defaultdict(list)
    for row in csv.DictReader((root / "summary.csv").open()):
        groups[row["query"], row["mode"]].append(row)
    result = {}
    for key, rows in groups.items():
        item = {
            "seconds": statistics.median(float(row["query_seconds"]) for row in rows),
            "rss_gib": statistics.median(int(row["rss_kib"]) / 2**20 for row in rows),
            "min_seconds": min(float(row["query_seconds"]) for row in rows),
            "max_seconds": max(float(row["query_seconds"]) for row in rows),
        }
        parsed = []
        for row in rows:
            values = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", row["selective_stats"])}
            if values:
                parsed.append(values)
        if parsed:
            for field in ("claimed", "transferred", "dequeued", "local_fit", "local_grow",
                          "queue_high_water", "observed_buffer_bytes", "allocations",
                          "growths", "growth_bytes"):
                item[field] = statistics.median(values[field] for values in parsed)
        result[key] = item
    return result


def folded_hotspots(root):
    output = {}
    terms = {
        "grow": "DynamicBufferPool::GrowBuffer",
        "register": "io_uring_register_buffers_update",
        "demand": "prepareBufferDemand",
        "unknown": "[unknown]",
    }
    for path in root.glob("*/stacks.folded"):
        case = path.parent.name
        query, mode = case[:3], case[4:-3]
        total = 0
        hits = collections.Counter()
        with path.open(errors="replace") as source:
            for line in source:
                try:
                    stack, weight = line.rsplit(" ", 1)
                    weight = int(weight)
                except ValueError:
                    continue
                total += weight
                for name, term in terms.items():
                    if term in stack:
                        hits[name] += weight
        output[query, mode] = {name: 100 * hits[name] / total for name in terms}
    return output


def save(fig, output, name):
    fig.savefig(output / f"{name}.svg", bbox_inches="tight")
    fig.savefig(output / f"{name}.png", dpi=220, bbox_inches="tight")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.results or latest_complete(Path("testcase/buffer-pool/results"))
    root = root.resolve()
    if not (root / "SUCCESS").exists():
        parser.error("results do not contain SUCCESS")
    manifest = json.loads((root / "manifest.json").read_text())
    queries = manifest["args"]["queries"]
    med = median_rows(root)
    expected = {(q, mode) for q in queries for mode in MODES}
    if set(med) != expected:
        parser.error(f"incomplete matrix: expected {len(expected)}, got {len(med)}")
    args.output.mkdir(parents=True, exist_ok=True)

    with (args.output / "per-query.csv").open("w", newline="") as target:
        fields = ["query", "mode", "median_seconds", "min_seconds", "max_seconds",
                  "median_rss_gib", "vs_dynamic", "vs_legacy", "transferred",
                  "growths", "growth_gib", "observed_buffer_gib"]
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        for query in queries:
            for mode in MODES:
                item = med[query, mode]
                writer.writerow({
                    "query": query, "mode": mode,
                    "median_seconds": f'{item["seconds"]:.6f}',
                    "min_seconds": f'{item["min_seconds"]:.6f}',
                    "max_seconds": f'{item["max_seconds"]:.6f}',
                    "median_rss_gib": f'{item["rss_gib"]:.6f}',
                    "vs_dynamic": f'{item["seconds"] / med[query, "dynamic"]["seconds"]:.6f}',
                    "vs_legacy": f'{item["seconds"] / med[query, "legacy"]["seconds"]:.6f}',
                    "transferred": item.get("transferred", ""),
                    "growths": item.get("growths", ""),
                    "growth_gib": f'{item.get("growth_bytes", 0) / 2**30:.6f}' if mode in SELECTIVE else "",
                    "observed_buffer_gib": f'{item.get("observed_buffer_bytes", 0) / 2**30:.6f}' if mode in SELECTIVE else "",
                })

    # Figure 1: all-query runtime ratios. Keep the manifest order used by the experiment.
    shown = ["legacy", "selective-0", "selective-1", "selective-2", "selective-4", "static"]
    matrix = np.array([[med[q, mode]["seconds"] / med[q, "dynamic"]["seconds"]
                        for q in queries] for mode in shown])
    fig, ax = plt.subplots(figsize=(15.4, 4.9))
    image = ax.imshow(matrix, cmap="RdYlGn_r", vmin=.55, vmax=1.30, aspect="auto")
    for row in range(len(shown)):
        for col in range(len(queries)):
            value = matrix[row, col]
            ax.text(col, row, f"{value:.2f}", ha="center", va="center", fontsize=7.2,
                    color="white" if value < .66 or value > 1.19 else "#172033")
    ax.set_xticks(range(len(queries)), queries, rotation=45, ha="right")
    ax.set_yticks(range(len(shown)), ["Legacy", "Selective-0", "Selective-1",
                                              "Selective-2", "Selective-4", "Static"])
    ax.set_title("Median query time normalized to Dynamic (lower is better)", weight="bold")
    bar = fig.colorbar(image, ax=ax, fraction=.025, pad=.02)
    bar.set_label("× Dynamic")
    fig.tight_layout()
    save(fig, args.output, "runtime-heatmap")

    # Figure 2: aggregate selective trade-off, normalized to Selective-0.
    metrics = {
        "Query time": [sum(med[q, m]["seconds"] for q in queries) for m in SELECTIVE],
        "Observed buffer": [sum(med[q, m]["observed_buffer_bytes"] for q in queries) for m in SELECTIVE],
        "Process RSS": [sum(med[q, m]["rss_gib"] for q in queries) for m in SELECTIVE],
        "Growth count": [sum(med[q, m]["growths"] for q in queries) for m in SELECTIVE],
    }
    fig, ax = plt.subplots(figsize=(9.2, 4.8))
    x = np.arange(3)
    width = .18
    for index, (name, values) in enumerate(metrics.items()):
        normalized = np.array(values[1:]) / values[0]
        ax.bar(x + (index - 1.5) * width, normalized, width, label=name,
               color=["#4c78a8", "#2a9d8f", "#e09f3e", "#8f6bb3"][index])
        for xpos, value in zip(x + (index - 1.5) * width, normalized):
            ax.text(xpos, value + .012, f"{value:.2f}", ha="center", va="bottom", fontsize=8)
    ax.axhline(1, color="#526077", linewidth=1, linestyle="--")
    ax.set_xticks(x, ["Queue 1", "Queue 2", "Queue 4"])
    ax.set_ylabel("Normalized to Selective-0")
    ax.set_ylim(0.68, 1.04)
    ax.set_title("Aggregate effect of task routing", weight="bold")
    ax.legend(ncol=4, loc="lower center", bbox_to_anchor=(.5, -0.28), frameon=False)
    ax.grid(axis="y", alpha=.2)
    fig.tight_layout()
    save(fig, args.output, "aggregate-routing-effect")

    # Figure 3: per-query memory reduction for the most growth-heavy queries.
    heavy = [q for q in queries if med[q, "selective-0"].get("growths", 0) > 0]
    buffer_reduction = [100 * (1 - med[q, "selective-4"]["observed_buffer_bytes"] /
                               med[q, "selective-0"]["observed_buffer_bytes"]) for q in heavy]
    rss_reduction = [100 * (1 - med[q, "selective-4"]["rss_gib"] /
                            med[q, "selective-0"]["rss_gib"]) for q in heavy]
    fig, ax = plt.subplots(figsize=(12, 4.8))
    x = np.arange(len(heavy)); width = .37
    ax.bar(x - width/2, buffer_reduction, width, label="Observed buffer", color="#2a9d8f")
    ax.bar(x + width/2, rss_reduction, width, label="Process RSS", color="#4c78a8")
    ax.axhline(0, color="#526077", linewidth=1)
    ax.set_xticks(x, heavy)
    ax.set_ylabel("Reduction: Selective-4 vs Selective-0 (%)")
    ax.set_title("Memory reduction on queries that trigger buffer growth", weight="bold")
    ax.legend(frameon=False)
    ax.grid(axis="y", alpha=.2)
    fig.tight_layout()
    save(fig, args.output, "memory-reduction")

    # Figure 4: known flamegraph hotspot. Percentages are weighted stack presence.
    hot = folded_hotspots(root)
    severe = ["q21", "q22", "q23", "q24", "q28", "q37", "q39", "q40"]
    fig, ax = plt.subplots(figsize=(11.5, 4.8))
    x = np.arange(len(severe)); width = .37
    ax.bar(x - width/2, [hot[q, "dynamic"]["grow"] for q in severe], width,
           label="Dynamic", color=COLORS["dynamic"])
    ax.bar(x + width/2, [hot[q, "selective-4"]["grow"] for q in severe], width,
           label="Selective-4", color=COLORS["selective-4"])
    ax.set_xticks(x, severe)
    ax.set_ylabel("Weighted stacks containing GrowBuffer (%)")
    ax.set_title("GrowBuffer hotspot in whole-process on-CPU profiles", weight="bold")
    ax.legend(frameon=False)
    ax.grid(axis="y", alpha=.2)
    fig.tight_layout()
    save(fig, args.output, "growbuffer-hotspot")

    summary = {
        "results": str(root), "queries": len(queries), "modes": len(MODES),
        "timed_runs": len(queries) * len(MODES) * 3,
        "perf_profiles": len(list(root.glob("*/perf-stat.csv"))),
        "unknown_stack_weight_median_percent": statistics.median(
            hot[key]["unknown"] for key in hot),
    }
    (args.output / "analysis-metadata.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
