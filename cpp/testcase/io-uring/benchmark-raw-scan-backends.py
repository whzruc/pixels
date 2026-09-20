#!/usr/bin/env python3

import argparse
import csv
import itertools
import os
import random
import shutil
import statistics
import subprocess
from pathlib import Path


MODES = ("non-fixed", "dynamic", "static")


def parse_output(output):
    result = {}
    for line in output.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in {"mode", "schedule", "threads", "files", "bytes", "requests", "seconds", "gib_per_second"}:
            result[key] = value
    required = {"mode", "schedule", "threads", "files", "bytes", "requests", "seconds", "gib_per_second"}
    if result.keys() != required:
        raise RuntimeError(f"benchmark output is incomplete: {sorted(result)}")
    return result


def run_once(args, threads, mode, repeat, measured):
    command = [
        str(args.binary),
        "--mode", mode,
        "--threads", str(threads),
        "--schedule", args.schedule,
        "--block-size", str(args.block_size),
        "--queue-depth", str(args.queue_depth),
        *(["--files-per-root", str(args.files_per_root)] if args.files_per_root else []),
        *map(str, args.roots),
    ]
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=args.timeout,
        env=dict(os.environ, PIXELS_SRC=str(args.repo_root), PIXELS_HOME=str(args.pixels_home)),
    )
    phase = "run" if measured else "warmup"
    raw = args.output / "raw" / f"t{threads}-{args.schedule}-{mode}-{phase}-{repeat}"
    raw.parent.mkdir(parents=True, exist_ok=True)
    raw.with_suffix(".out").write_text(completed.stdout)
    raw.with_suffix(".err").write_text(completed.stderr)
    if completed.returncode != 0:
        raise RuntimeError(f"threads={threads} {mode} {phase} failed; see {raw.with_suffix('.err')}")
    return parse_output(completed.stdout)


def median_absolute_deviation(values):
    center = statistics.median(values)
    return statistics.median(abs(value - center) for value in values)


def permutation_p_value(first, second):
    observed = abs(statistics.median(first) - statistics.median(second))
    combined = tuple(first) + tuple(second)
    extreme = 0
    total = 0
    for selected_indexes in itertools.combinations(range(len(combined)), len(first)):
        selected_indexes = set(selected_indexes)
        left = [value for index, value in enumerate(combined) if index in selected_indexes]
        right = [value for index, value in enumerate(combined) if index not in selected_indexes]
        if abs(statistics.median(left) - statistics.median(right)) >= observed - 1e-12:
            extreme += 1
        total += 1
    return extreme / total


def main():
    cpp_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        action="append",
        type=Path,
        help=("directory or .pxl file; repeat for multiple devices. If omitted, "
              "auto-discover /data/9a3-*/clickbench/pixels-e0-fb"),
    )
    parser.add_argument("--threads", nargs="+", type=int, default=[12, 24, 48])
    parser.add_argument("--schedule", choices=("shared", "device-affine"),
                        default="shared")
    parser.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES))
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--block-size", type=int, default=1024 * 1024)
    parser.add_argument("--queue-depth", type=int, default=32)
    parser.add_argument("--files-per-root", type=int,
                        help="limit each input root for a small-scale validation")
    parser.add_argument("--seed", type=int, default=1392)
    parser.add_argument("--timeout", type=int, default=14400)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--binary",
        type=Path,
        default=cpp_root / "build" / "release" / "extension" / "pixels" /
                "testcase" / "io-uring" / "PixelsIoUringScanBenchmark",
    )
    parser.add_argument("--pixels-home", type=Path, default=cpp_root.parent)
    args = parser.parse_args()
    if args.root:
        args.roots = [root.resolve() for root in args.root]
    else:
        args.roots = sorted(
            path.resolve()
            for path in Path("/data").glob("9a3-*/clickbench/pixels-e0-fb")
            if path.is_dir()
        )
        if not args.roots:
            parser.error("no --root supplied and no default ClickBench directories found")
        print(f"Auto-discovered {len(args.roots)} input roots:", flush=True)
        for root in args.roots:
            print(f"  {root}", flush=True)
    args.output = args.output.resolve()
    args.repo_root = cpp_root.parent

    if args.repeats <= 0:
        parser.error("--repeats must be positive")
    if any(value <= 0 for value in args.threads):
        parser.error("--threads values must be positive")
    if args.schedule == "device-affine" and any(
            value != len(args.roots) for value in args.threads):
        parser.error("device-affine scheduling requires every --threads value "
                     "to equal the number of input roots")
    if args.files_per_root is not None and args.files_per_root <= 0:
        parser.error("--files-per-root must be positive")
    for path in [args.binary, args.pixels_home, *args.roots]:
        if not path.exists():
            parser.error(f"required path does not exist: {path}")
    results_root = (cpp_root / "testcase" / "io-uring" / "results").resolve()
    protected_outputs = {
        Path("/"), Path("/tmp"), Path.home().resolve(), cpp_root.resolve(),
        cpp_root.parent.resolve(), results_root,
    }
    contains_repository = args.output in cpp_root.resolve().parents
    if args.output in protected_outputs or contains_repository:
        parser.error(f"refusing to overwrite protected directory: {args.output}")
    if args.output.exists():
        if not args.output.is_dir():
            parser.error(f"output path exists and is not a directory: {args.output}")
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)
    with (args.output / "configuration.csv").open("w", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(("schedule", "roots", "block_size", "queue_depth",
                         "files_per_root", "repeats", "seed"))
        writer.writerow((args.schedule, len(args.roots), args.block_size,
                         args.queue_depth, args.files_per_root or "all",
                         args.repeats, args.seed))

    for threads in args.threads:
        for mode in args.modes:
            run_once(args, threads, mode, 0, measured=False)

    schedule = [
        (threads, mode, repeat)
        for threads in args.threads
        for repeat in range(1, args.repeats + 1)
        for mode in args.modes
    ]
    random.Random(args.seed).shuffle(schedule)
    rows = []
    expected = None
    for threads, mode, repeat in schedule:
        result = run_once(args, threads, mode, repeat, measured=True)
        identity = (result["files"], result["bytes"], result["requests"])
        expected = expected or identity
        if identity != expected:
            raise RuntimeError(f"scan coverage mismatch: expected {expected}, got {identity}")
        rows.append((threads, mode, repeat, int(result["files"]), int(result["bytes"]),
                     int(result["requests"]), float(result["seconds"]),
                     float(result["gib_per_second"])))
        print(f"threads={threads} {mode} repeat={repeat} "
              f"seconds={result['seconds']} GiB/s={result['gib_per_second']}", flush=True)

    with (args.output / "timings.csv").open("w", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(("threads", "mode", "repeat", "files", "bytes", "requests",
                         "seconds", "gib_per_second"))
        writer.writerows(rows)

    with (args.output / "summary.csv").open("w", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(("threads", "first", "second", "first_median_gib_per_second",
                         "second_median_gib_per_second", "second_speedup", "first_mad",
                         "second_mad", "permutation_p_value", "clear_difference"))
        for threads in args.threads:
            samples = {
                mode: [row[7] for row in rows if row[0] == threads and row[1] == mode]
                for mode in args.modes
            }
            for first, second in itertools.combinations(args.modes, 2):
                first_median = statistics.median(samples[first])
                second_median = statistics.median(samples[second])
                p_value = permutation_p_value(samples[first], samples[second])
                relative_difference = abs(second_median - first_median) / first_median
                clear = p_value < 0.05 and relative_difference >= 0.05
                writer.writerow((threads, first, second, first_median, second_median,
                                 second_median / first_median,
                                 median_absolute_deviation(samples[first]),
                                 median_absolute_deviation(samples[second]), p_value,
                                 str(clear).lower()))
                print(f"threads={threads} {first} vs {second}: "
                      f"second_speedup={second_median / first_median:.3f} "
                      f"p={p_value:.4f} clear={clear}")


if __name__ == "__main__":
    main()
