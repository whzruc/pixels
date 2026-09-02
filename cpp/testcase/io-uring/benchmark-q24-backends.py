#!/usr/bin/env python3

import argparse
import csv
import glob
import hashlib
import itertools
import os
import random
import shutil
import statistics
import subprocess
import time
from pathlib import Path


DEFAULT_MODES = ("non-fixed", "dynamic", "static")
VALID_MODES = ("legacy",) + DEFAULT_MODES


def replace_property(text, key, value):
    prefix = f"{key}="
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.startswith(prefix):
            lines[index] = prefix + value
            break
    else:
        lines.append(prefix + value)
    return "\n".join(lines) + "\n"


def prepare_home(output, base_config, mode, threads, column_sizes):
    home = output / "homes" / f"t{threads}" / mode
    config = home / "cpp" / "etc" / "pixels-cpp.properties"
    config.parent.mkdir(parents=True, exist_ok=True)
    text = base_config.read_text()
    text = replace_property(text, "pixel.bufferpool.mode", mode)
    text = replace_property(text, "pixel.threads", str(threads))
    text = replace_property(text, "pixel.static.buffer.threads", str(threads))
    text = replace_property(text, "pixel.column.size.path", str(column_sizes))
    text = replace_property(text, "pixel.enable.profiler", "false")
    config.write_text(text)
    return home


def run_query(args, threads, mode, repeat, measured):
    home = prepare_home(args.output, args.base_config, mode, threads, args.column_sizes)
    pixels_sources = ", ".join(
        "'" + pixels_glob.replace("'", "''") + "'" for pixels_glob in args.pixels_globs
    )
    sql = "\n".join((
        f"PRAGMA threads={threads};",
        f"CREATE VIEW hits AS SELECT * FROM pixels_scan([{pixels_sources}]);",
        args.query.read_text(),
    ))
    command = [str(args.duckdb), "-csv", "-noheader", "-c", sql]
    environment = dict(os.environ, PIXELS_SRC=str(args.repo_root), PIXELS_HOME=str(home))
    started = time.perf_counter()
    completed = subprocess.run(command, capture_output=True, text=True, env=environment,
                               timeout=args.timeout)
    elapsed = time.perf_counter() - started
    phase = "run" if measured else "warmup"
    raw = args.output / "raw" / f"t{threads}-{mode}-{phase}-{repeat}"
    raw.parent.mkdir(parents=True, exist_ok=True)
    raw.with_suffix(".out").write_text(completed.stdout)
    raw.with_suffix(".err").write_text(completed.stderr)
    if completed.returncode != 0:
        raise RuntimeError(
            f"threads={threads} {mode} {phase} failed; see {raw.with_suffix('.err')}"
        )
    ignored_prefixes = ("PIXELS_SRC is ", "PIXELS_HOME is ", "pixels properties file is ")
    result_lines = [line.strip() for line in completed.stdout.splitlines()
                    if line.strip() and not line.startswith(ignored_prefixes)]
    digest = hashlib.sha256("\n".join(sorted(result_lines)).encode()).hexdigest()
    return elapsed, digest


def permutation_p_value(first, second):
    observed = abs(statistics.median(first) - statistics.median(second))
    combined = tuple(first) + tuple(second)
    first_size = len(first)
    extreme = 0
    total = 0
    indexes = range(len(combined))
    for selected in itertools.combinations(indexes, first_size):
        selected = set(selected)
        left = [value for index, value in enumerate(combined) if index in selected]
        right = [value for index, value in enumerate(combined) if index not in selected]
        if abs(statistics.median(left) - statistics.median(right)) >= observed - 1e-12:
            extreme += 1
        total += 1
    return extreme / total


def median_absolute_deviation(values):
    center = statistics.median(values)
    return statistics.median(abs(value - center) for value in values)


def main():
    cpp_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--pixels-glob",
        dest="pixels_globs",
        action="append",
        required=True,
        help="Pixels input glob; repeat this option to scan multiple devices",
    )
    parser.add_argument("--column-sizes", required=True, type=Path)
    parser.add_argument("--threads", nargs="+", type=int, default=[12, 24, 48])
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--modes", nargs="+", choices=VALID_MODES, default=list(DEFAULT_MODES))
    parser.add_argument("--seed", type=int, default=1392)
    parser.add_argument("--timeout", type=int, default=14400)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--duckdb", type=Path, default=cpp_root / "build" / "release" / "duckdb")
    parser.add_argument("--base-config", type=Path, default=cpp_root / "etc" / "pixels-cpp.properties")
    parser.add_argument("--query", type=Path,
                        default=cpp_root / "pixels-duckdb" / "duckdb" / "benchmark" / "clickbench" /
                        "queries" / "q24.sql")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.repo_root = cpp_root.parent

    if args.repeats < 5:
        parser.error("--repeats must be at least 5 for the significance check")
    if any(threads <= 0 for threads in args.threads):
        parser.error("--threads values must be positive")
    if len(set(args.threads)) != len(args.threads):
        parser.error("--threads values must be unique")
    for path in (args.duckdb, args.base_config, args.query, args.column_sizes):
        if not path.exists():
            parser.error(f"required path does not exist: {path}")
    for pixels_glob in args.pixels_globs:
        if next(glob.iglob(pixels_glob), None) is None:
            parser.error(f"Pixels input glob has no matches: {pixels_glob}")
    if args.output.exists():
        parser.error(f"output directory already exists: {args.output}")
    args.output.mkdir(parents=True)
    shutil.copy2(args.query, args.output / "q24.sql")

    for threads in args.threads:
        for mode in args.modes:
            run_query(args, threads, mode, 0, measured=False)

    schedule = [
        (threads, mode, repeat)
        for threads in args.threads
        for repeat in range(1, args.repeats + 1)
        for mode in args.modes
    ]
    random.Random(args.seed).shuffle(schedule)
    rows = []
    expected_hashes = {}
    for threads, mode, repeat in schedule:
        elapsed, digest = run_query(args, threads, mode, repeat, measured=True)
        expected_hash = expected_hashes.setdefault(threads, digest)
        if digest != expected_hash:
            raise RuntimeError(
                f"result hash mismatch for threads={threads} {mode} repeat {repeat}"
            )
        rows.append((threads, mode, repeat, elapsed, digest))
        print(f"threads={threads} {mode} repeat={repeat} elapsed={elapsed:.6f}s")

    with (args.output / "timings.csv").open("w", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(("threads", "mode", "repeat", "elapsed_seconds", "sha256"))
        writer.writerows(rows)

    with (args.output / "summary.csv").open("w", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(("threads", "first", "second", "first_median_seconds",
                         "second_median_seconds",
                         "second_speedup", "first_mad_seconds", "second_mad_seconds",
                         "permutation_p_value", "clear_difference"))
        for threads in args.threads:
            samples = {
                mode: [row[3] for row in rows if row[0] == threads and row[1] == mode]
                for mode in args.modes
            }
            for first, second in itertools.combinations(args.modes, 2):
                first_median = statistics.median(samples[first])
                second_median = statistics.median(samples[second])
                p_value = permutation_p_value(samples[first], samples[second])
                relative_difference = abs(second_median - first_median) / first_median
                clear = p_value < 0.05 and relative_difference >= 0.05
                writer.writerow((threads, first, second, first_median, second_median,
                                 first_median / second_median,
                                 median_absolute_deviation(samples[first]),
                                 median_absolute_deviation(samples[second]),
                                 p_value, str(clear).lower()))
                print(f"threads={threads} {first} vs {second}: "
                      f"speedup={first_median / second_median:.3f} "
                      f"p={p_value:.4f} clear={clear}")


if __name__ == "__main__":
    main()
