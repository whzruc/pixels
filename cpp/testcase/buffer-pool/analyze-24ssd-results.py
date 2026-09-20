#!/usr/bin/env python3

import argparse
import csv
import math
import re
import statistics
from collections import defaultdict
from pathlib import Path


def read_csv(path):
    with path.open(newline="") as source:
        return list(csv.DictReader(source))


def median(values):
    return statistics.median(values) if values else math.nan


def geometric_mean(values):
    values = [value for value in values if value > 0 and math.isfinite(value)]
    return math.exp(sum(math.log(value) for value in values) / len(values)) if values else math.nan


def elapsed_seconds(value):
    fields = [float(part) for part in value.split(":")]
    if len(fields) == 3:
        return fields[0] * 3600 + fields[1] * 60 + fields[2]
    if len(fields) == 2:
        return fields[0] * 60 + fields[1]
    return fields[0]


def parse_perf(path):
    metrics = {}
    if not path.exists():
        return metrics
    with path.open(newline="") as source:
        for row in csv.reader(source):
            if len(row) < 3 or not row[0].strip() or row[0].startswith("#"):
                continue
            try:
                metrics[row[2]] = float(row[0].replace(" ", ""))
            except ValueError:
                continue
    return metrics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("result_dir", type=Path)
    args = parser.parse_args()
    result_dir = args.result_dir.resolve()

    query_rows = read_csv(result_dir / "query_times.csv")
    suite_rows = read_csv(result_dir / "suite_metrics.csv")
    cold_path = result_dir / "cold_metrics.csv"
    cold_rows = read_csv(cold_path) if cold_path.exists() else []
    buffer_metrics_path = result_dir / "buffer_pool_metrics.csv"
    buffer_rows = read_csv(buffer_metrics_path) if buffer_metrics_path.exists() else []
    grouped_queries = defaultdict(list)
    result_hashes = defaultdict(set)
    for row in query_rows:
        key = (row["mode"], int(row["threads"]), row["query"])
        grouped_queries[key].append(float(row["seconds"]))
        result_hashes[(int(row["threads"]), row["query"])].add(row["result_sha256"])

    query_summary_path = result_dir / "query_summary.csv"
    with query_summary_path.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("mode", "threads", "query", "runs", "median_seconds", "min_seconds", "max_seconds"))
        for (mode, threads, query), values in sorted(grouped_queries.items()):
            writer.writerow((mode, threads, query, len(values), median(values), min(values), max(values)))

    suite_groups = defaultdict(list)
    for row in suite_rows:
        suite_groups[(row["mode"], int(row["threads"]))].append(row)
    cold_groups = defaultdict(list)
    for row in cold_rows:
        cold_groups[(row["mode"], int(row["threads"]))].append(row)

    summary_path = result_dir / "summary.csv"
    summary_rows = []
    for key, rows in sorted(suite_groups.items()):
        mode, threads = key
        steady_medians = [median(values) for (m, t, query), values in grouped_queries.items()
                          if m == mode and t == threads and query.startswith("q") and query != "q00"]
        init_values = grouped_queries.get((mode, threads, "init"), [])
        warmup_values = grouped_queries.get((mode, threads, "q00"), [])
        perf_values = defaultdict(list)
        for row in rows:
            perf_path = Path(row["perf_file"])
            if not perf_path.is_absolute():
                perf_path = result_dir / perf_path
            for event, value in parse_perf(perf_path).items():
                perf_values[event].append(value)
        cold = cold_groups.get(key, [])
        cold_perf_values = defaultdict(list)
        for cold_row in cold:
            cold_perf_path = Path(cold_row["perf_file"])
            if not cold_perf_path.is_absolute():
                cold_perf_path = result_dir / cold_perf_path
            for event, value in parse_perf(cold_perf_path).items():
                cold_perf_values[event].append(value)
        summary_rows.append({
            "mode": mode,
            "threads": threads,
            "runs": len(rows),
            "median_init_seconds": median(init_values),
            "median_warmup_seconds": median(warmup_values),
            "median_cold_init_plus_q00_seconds": median([
                float(row["init_seconds"]) + float(row["warmup_seconds"]) for row in cold]),
            "median_cold_max_rss_gib": median([
                float(row["max_rss_kb"]) / 1024 / 1024 for row in cold]),
            "median_cold_minor_faults": median([float(row["minor_faults"]) for row in cold]),
            "median_cold_dtlb_load_misses": median(cold_perf_values["dTLB-load-misses"]),
            "sum_query_medians_seconds": sum(steady_medians),
            "median_suite_elapsed_seconds": median([elapsed_seconds(row["elapsed"]) for row in rows]),
            "median_max_rss_gib": median([float(row["max_rss_kb"]) / 1024 / 1024 for row in rows]),
            "median_minor_faults": median([float(row["minor_faults"]) for row in rows]),
            "median_cycles": median(perf_values["cycles"]),
            "median_instructions": median(perf_values["instructions"]),
            "median_dtlb_load_misses": median(perf_values["dTLB-load-misses"]),
            "median_cache_misses": median(perf_values["cache-misses"]),
        })

    fields = list(summary_rows[0]) if summary_rows else []
    with summary_path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summary_rows)

    speedups = []
    query_speedup_rows = []
    for row in summary_rows:
        mode, threads = row["mode"], row["threads"]
        if mode == "legacy":
            continue
        ratios = []
        for query_number in range(1, 44):
            query = f"q{query_number:02d}"
            baseline = median(grouped_queries.get(("legacy", threads, query), []))
            candidate = median(grouped_queries.get((mode, threads, query), []))
            if baseline > 0 and candidate > 0:
                ratios.append(baseline / candidate)
                query_speedup_rows.append((mode, threads, query, baseline, candidate, baseline / candidate))
        speedups.append((mode, threads, len(ratios), geometric_mean(ratios)))

    with (result_dir / "query_speedups.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("mode", "threads", "query", "legacy_median_seconds",
                         "candidate_median_seconds", "speedup"))
        writer.writerows(query_speedup_rows)

    buffer_fields = (
        "current_allocated_bytes", "peak_allocated_bytes", "current_registered_bytes",
        "peak_registered_bytes", "allocations", "frees", "registrations",
        "registration_updates", "reuses", "growths"
    )
    buffer_groups = defaultdict(list)
    query_buffer_groups = defaultdict(list)
    for row in buffer_rows:
        key = (row["mode"], int(row["threads"]), row["phase"])
        buffer_groups[key].append(row)
        if row["phase"] == "query":
            query_buffer_groups[(row["mode"], int(row["threads"]), row["query"])].append(row)

    with (result_dir / "buffer_pool_summary.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("mode", "threads", "phase", "runs",
                         *(f"median_{field}" for field in buffer_fields)))
        for (mode, threads, phase), rows in sorted(buffer_groups.items()):
            writer.writerow((mode, threads, phase, len(rows),
                             *(median([float(row[field]) for row in rows]) for field in buffer_fields)))

    with (result_dir / "query_buffer_pool_summary.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("mode", "threads", "query", "runs",
                         *(f"median_{field}" for field in buffer_fields)))
        for (mode, threads, query), rows in sorted(query_buffer_groups.items()):
            writer.writerow((mode, threads, query, len(rows),
                             *(median([float(row[field]) for row in rows]) for field in buffer_fields)))

    mismatches = [(threads, query, len(hashes)) for (threads, query), hashes in result_hashes.items()
                  if len(hashes) > 1]
    with (result_dir / "hash_mismatches.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("threads", "query", "distinct_hashes"))
        writer.writerows(sorted(mismatches))
    report = [
        "# 24-SSD ClickBench Buffer Pool Results",
        "",
        "## Geometric-mean speedup over legacy",
        "",
        "| Mode | Threads | Matched queries | Speedup |",
        "|---|---:|---:|---:|",
    ]
    report.extend(f"| {mode} | {threads} | {count} | {speedup:.4f}x |"
                  for mode, threads, count, speedup in speedups)
    report.extend((
        "",
        "## Allocation and memory metrics",
        "",
        "| Mode | Threads | Cold init+q00 (s) | Steady query sum (s) | Peak RSS (GiB) | Cold minor faults | Cold dTLB misses |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ))
    report.extend(
        f"| {row['mode']} | {row['threads']} | {row['median_cold_init_plus_q00_seconds']:.3f} | "
        f"{row['sum_query_medians_seconds']:.3f} | {row['median_max_rss_gib']:.2f} | "
        f"{row['median_cold_minor_faults']:.0f} | {row['median_cold_dtlb_load_misses']:.0f} |"
        for row in summary_rows
    )
    report.extend((
        "",
        "## Result hash check",
        "",
        f"Potential mismatches: {len(mismatches)}. Hash differences can also be caused by nondeterministic row order.",
        "",
        "## Files",
        "",
        "- `summary.csv`: configuration-level time, memory, fault, cache, and dTLB metrics.",
        "- `query_summary.csv`: per-query median/min/max latency.",
        "- `query_speedups.csv`: per-query speedup relative to legacy.",
        "- `hash_mismatches.csv`: result hashes that need correctness review.",
        "- `suite_metrics.csv`: raw process-level metrics.",
        "- `buffer_pool_metrics.csv`: raw process-lifetime Pixels buffer pool counters.",
        "- `buffer_pool_summary.csv`: median cold/suite/query buffer pool counters.",
        "- `query_process_metrics.csv`: isolated-query time, RSS, perf, and result hashes.",
        "- `query_buffer_pool_summary.csv`: per-query median buffer allocation, registration, reuse, and growth.",
        "- `raw/`: DuckDB output, `/usr/bin/time`, and `perf stat` output.",
    ))
    (result_dir / "report.md").write_text("\n".join(report) + "\n")
    print(f"Wrote {summary_path}")
    print(f"Wrote {query_summary_path}")
    print(f"Wrote {result_dir / 'report.md'}")


if __name__ == "__main__":
    main()
