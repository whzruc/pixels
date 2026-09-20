#!/usr/bin/env python3

import argparse
import csv
import json
from pathlib import Path


def read_keys(path, fields):
    if not path.exists():
        return set()
    with path.open(newline="") as source:
        return {tuple(row[field] for field in fields) for row in csv.DictReader(source)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("result_dir", type=Path)
    args = parser.parse_args()
    root = args.result_dir.resolve()
    environment = json.loads((root / "environment.json").read_text())
    modes = environment["modes"]
    threads = [str(value) for value in environment["threads"]]
    repeats = range(1, int(environment["repeats"]) + 1)
    queries = environment["queries"]

    expected_suites = {(mode, thread, str(repeat)) for mode in modes for thread in threads for repeat in repeats}
    actual_suites = read_keys(root / "suite_metrics.csv", ("mode", "threads", "repeat"))
    actual_cold = read_keys(root / "cold_metrics.csv", ("mode", "threads", "repeat"))
    missing_suites = expected_suites - actual_suites
    missing_cold = expected_suites - actual_cold
    buffer_metric_keys = read_keys(root / "buffer_pool_metrics.csv",
                                   ("mode", "threads", "repeat", "phase", "query"))
    missing_buffer_metrics = set()
    if environment.get("buffer_pool_stats"):
        expected_suite_stats = {(*key, "suite", "all") for key in expected_suites}
        expected_cold_stats = {(*key, "cold", "q00") for key in expected_suites}
        missing_buffer_metrics.update(expected_suite_stats - buffer_metric_keys)
        missing_buffer_metrics.update(expected_cold_stats - buffer_metric_keys)

    missing_queries = set()
    if environment.get("isolated_query_stats"):
        isolated_repeats = range(1, int(environment.get("isolated_repeats", 1)) + 1)
        expected_queries = {(mode, thread, str(repeat), query) for mode in modes for thread in threads
                            for repeat in isolated_repeats for query in queries}
        actual_queries = read_keys(root / "query_process_metrics.csv", ("mode", "threads", "repeat", "query"))
        missing_queries = expected_queries - actual_queries
        expected_query_stats = {(*key[:3], "query", key[3]) for key in expected_queries}
        missing_buffer_metrics.update(expected_query_stats - buffer_metric_keys)

    with (root / "failures.csv").open(newline="") as source:
        failures = list(csv.DictReader(source))

    print(f"Suite cases: {len(actual_suites)}/{len(expected_suites)}")
    print(f"Cold cases: {len(actual_cold)}/{len(expected_suites)}")
    if environment.get("isolated_query_stats"):
        print(f"Isolated query cases: {len(actual_queries)}/{len(expected_queries)}")
    print(f"Recorded failures: {len(failures)}")
    print(f"Missing buffer-stat rows: {len(missing_buffer_metrics)}")

    if missing_suites or missing_cold or missing_queries or missing_buffer_metrics or failures:
        if missing_suites:
            print(f"Missing suite cases: {sorted(missing_suites)[:10]}")
        if missing_cold:
            print(f"Missing cold cases: {sorted(missing_cold)[:10]}")
        if missing_queries:
            print(f"Missing isolated query cases: {sorted(missing_queries)[:10]}")
        if missing_buffer_metrics:
            print(f"Missing buffer-stat rows: {sorted(missing_buffer_metrics)[:10]}")
        raise SystemExit(1)
    print("Completeness check passed")


if __name__ == "__main__":
    main()
