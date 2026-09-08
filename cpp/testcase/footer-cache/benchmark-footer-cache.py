#!/usr/bin/env python3
"""Compare the parent and footer-cache builds with identical Pixels settings."""

import argparse
import csv
import glob
import hashlib
import itertools
import os
import random
import re
import shutil
import statistics
import subprocess
import time
from pathlib import Path


PROFILE_RE = re.compile(r"^(.+?) ([0-9.eE+-]+)s\(thread time\)$")
IGNORED_PREFIXES = ("PIXELS_SRC is ", "PIXELS_HOME is ", "pixels properties file is ")


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


def prepare_home(args, build, threads):
    home = args.output / "homes" / build / f"t{threads}"
    config = home / "cpp" / "etc" / "pixels-cpp.properties"
    config.parent.mkdir(parents=True, exist_ok=True)
    text = args.base_config.read_text()
    text = replace_property(text, "pixel.threads", str(threads))
    text = replace_property(text, "pixel.static.buffer.threads", str(threads))
    text = replace_property(text, "pixel.column.size.path", str(args.column_sizes))
    text = replace_property(text, "pixel.bufferpool.mode", args.mode)
    text = replace_property(text, "pixel.enable.profiler", "true")
    config.write_text(text)
    return home


def parse_output(stdout):
    profiles = {}
    result_lines = []
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        if not line or line.startswith(IGNORED_PREFIXES):
            continue
        match = PROFILE_RE.match(line)
        if match:
            profiles[match.group(1)] = float(match.group(2))
        else:
            result_lines.append(line)
    digest = hashlib.sha256("\n".join(sorted(result_lines)).encode()).hexdigest()
    return digest, profiles


def run_query(args, build, threads, repeat, measured):
    binary = args.baseline_duckdb if build == "baseline" else args.candidate_duckdb
    home = prepare_home(args, build, threads)
    sources = ", ".join("'" + item.replace("'", "''") + "'" for item in args.pixels_globs)
    sql = "\n".join((
        f"PRAGMA threads={threads};",
        f"CREATE VIEW hits AS SELECT * FROM pixels_scan([{sources}]);",
        args.query.read_text(),
    ))
    environment = dict(os.environ, PIXELS_SRC=str(args.repo_root), PIXELS_HOME=str(home))
    started = time.perf_counter()
    completed = subprocess.run([str(binary), "-csv", "-noheader", "-c", sql],
                               capture_output=True, text=True, env=environment,
                               timeout=args.timeout)
    elapsed = time.perf_counter() - started
    phase = "run" if measured else "warmup"
    stem = args.output / "raw" / f"t{threads}-{build}-{phase}-{repeat}"
    stem.parent.mkdir(parents=True, exist_ok=True)
    stem.with_suffix(".out").write_text(completed.stdout)
    stem.with_suffix(".err").write_text(completed.stderr)
    if completed.returncode != 0:
        raise RuntimeError(f"{build} t{threads} {phase} failed; see {stem.with_suffix('.err')}")
    digest, profiles = parse_output(completed.stdout)
    return elapsed, digest, profiles


def permutation_p_value(first, second):
    observed = abs(statistics.median(first) - statistics.median(second))
    combined = tuple(first) + tuple(second)
    extreme = total = 0
    for selected in itertools.combinations(range(len(combined)), len(first)):
        selected = set(selected)
        left = [v for i, v in enumerate(combined) if i in selected]
        right = [v for i, v in enumerate(combined) if i not in selected]
        extreme += abs(statistics.median(left) - statistics.median(right)) >= observed - 1e-12
        total += 1
    return extreme / total


def mad(values):
    center = statistics.median(values)
    return statistics.median(abs(value - center) for value in values)


def main():
    cpp_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--pixels-glob", dest="pixels_globs", action="append", required=True)
    parser.add_argument("--column-sizes", required=True, type=Path)
    parser.add_argument("--baseline-duckdb", required=True, type=Path)
    parser.add_argument("--candidate-duckdb", required=True, type=Path)
    parser.add_argument("--threads", nargs="+", type=int, default=[12, 24, 48])
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--mode", choices=("legacy", "non-fixed", "dynamic", "static"),
                        default="non-fixed")
    parser.add_argument("--seed", type=int, default=20260902)
    parser.add_argument("--timeout", type=int, default=14400)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--base-config", type=Path,
                        default=cpp_root / "etc" / "pixels-cpp.properties")
    parser.add_argument("--query", type=Path,
                        default=cpp_root / "pixels-duckdb" / "duckdb" / "benchmark" /
                                "clickbench" / "queries" / "q24.sql")
    args = parser.parse_args()
    args.repo_root = cpp_root.parent
    args.output = args.output.resolve()

    if args.repeats <= 0 or args.warmups < 0:
        parser.error("--repeats must be positive and --warmups must be non-negative")
    if any(value <= 0 for value in args.threads):
        parser.error("--threads values must be positive")
    for path in (args.baseline_duckdb, args.candidate_duckdb, args.column_sizes,
                 args.base_config, args.query):
        if not path.exists():
            parser.error(f"required path does not exist: {path}")
    for pattern in args.pixels_globs:
        if next(glob.iglob(pattern), None) is None:
            parser.error(f"Pixels input glob has no matches: {pattern}")
    if args.output.exists():
        if not args.overwrite:
            parser.error(f"output directory exists: {args.output}; pass --overwrite to replace it")
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)
    shutil.copy2(args.query, args.output / "query.sql")

    builds = ("baseline", "candidate")
    for threads in args.threads:
        for warmup in range(1, args.warmups + 1):
            for build in builds:
                run_query(args, build, threads, warmup, measured=False)

    schedule = [(threads, build, repeat) for threads in args.threads
                for repeat in range(1, args.repeats + 1) for build in builds]
    random.Random(args.seed).shuffle(schedule)
    rows = []
    expected_hashes = {}
    for threads, build, repeat in schedule:
        elapsed, digest, profiles = run_query(args, build, threads, repeat, measured=True)
        expected = expected_hashes.setdefault(threads, digest)
        if digest != expected:
            raise RuntimeError(f"result hash mismatch: {build} t{threads} repeat {repeat}")
        rows.append({"threads": threads, "build": build, "repeat": repeat,
                     "elapsed_seconds": elapsed, "sha256": digest,
                     "file_tail_seconds": profiles.get("Pixels.Metadata.FileTailRead", 0.0),
                     "row_group_footer_seconds":
                         profiles.get("Pixels.Metadata.RowGroupFooterRead", 0.0)})
        print(f"threads={threads} build={build} repeat={repeat} elapsed={elapsed:.6f}s")

    with (args.output / "timings.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=rows[0].keys(), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    with (args.output / "summary.csv").open("w", newline="") as output:
        fields = ("threads", "baseline_median_seconds", "candidate_median_seconds",
                  "candidate_speedup", "baseline_mad_seconds", "candidate_mad_seconds",
                  "permutation_p_value", "baseline_footer_thread_seconds_median",
                  "candidate_footer_thread_seconds_median")
        writer = csv.DictWriter(output, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for threads in args.threads:
            samples = {build: [row for row in rows if row["threads"] == threads and
                               row["build"] == build] for build in builds}
            baseline = [row["elapsed_seconds"] for row in samples["baseline"]]
            candidate = [row["elapsed_seconds"] for row in samples["candidate"]]
            footer = lambda row: row["file_tail_seconds"] + row["row_group_footer_seconds"]
            summary = {
                "threads": threads,
                "baseline_median_seconds": statistics.median(baseline),
                "candidate_median_seconds": statistics.median(candidate),
                "candidate_speedup": statistics.median(baseline) / statistics.median(candidate),
                "baseline_mad_seconds": mad(baseline),
                "candidate_mad_seconds": mad(candidate),
                "permutation_p_value": permutation_p_value(baseline, candidate),
                "baseline_footer_thread_seconds_median":
                    statistics.median(map(footer, samples["baseline"])),
                "candidate_footer_thread_seconds_median":
                    statistics.median(map(footer, samples["candidate"])),
            }
            writer.writerow(summary)
            print(f"threads={threads} speedup={summary['candidate_speedup']:.3f} "
                  f"p={summary['permutation_p_value']:.4f}")


if __name__ == "__main__":
    main()
