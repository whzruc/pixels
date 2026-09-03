#!/usr/bin/env python3
"""Benchmark the standalone Pixels io_uring scan backends."""

import argparse
import csv
import datetime as dt
import itertools
import os
import shutil
import subprocess
import time
from pathlib import Path


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", action="append", required=True,
                        help="Pixels directory; repeat once per device")
    parser.add_argument("--threads", nargs="+", type=int, default=[12, 24, 48])
    parser.add_argument("--modes", nargs="+", choices=("non-fixed", "dynamic", "static"),
                        default=["non-fixed", "dynamic", "static"])
    parser.add_argument("--schedules", nargs="+", choices=("shared", "device-affine"),
                        default=["shared"])
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--block-size", type=int, default=1048576)
    parser.add_argument("--queue-depth", type=int, default=32)
    parser.add_argument("--files-per-root", type=int, default=0)
    parser.add_argument("--timeout", type=int, default=14400)
    parser.add_argument("--binary", type=Path,
                        default=repo / "build/release/extension/pixels/testcase/io-uring/"
                                "PixelsIoUringScanBenchmark")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    if args.repeats <= 0 or any(value <= 0 for value in args.threads):
        parser.error("repeats and threads must be positive")
    if args.output.exists():
        if not args.overwrite:
            parser.error(f"output directory already exists: {args.output}")
        shutil.rmtree(args.output)
    if not args.binary.exists():
        parser.error(f"benchmark binary does not exist: {args.binary}")
    for root in args.root:
        if not Path(root).exists():
            parser.error(f"input root does not exist: {root}")
    if "device-affine" in args.schedules:
        invalid = [t for t in args.threads if t != len(args.root)]
        if invalid:
            parser.error("device-affine requires threads == number of --root values")

    args.output.mkdir(parents=True)
    (args.output / "command.txt").write_text(
        " ".join(os.environ.get("_", "python3") + " " + str(Path(__file__).resolve())
                 for _ in [0]) + "\n")
    rows = []
    combinations = list(itertools.product(args.threads, args.modes, args.schedules,
                                           range(1, args.repeats + 1)))
    for threads, mode, schedule, repeat in combinations:
        stem = f"t{threads}-{mode}-{schedule}-r{repeat}"
        command = [str(args.binary), "--mode", mode, "--threads", str(threads),
                   "--schedule", schedule, "--block-size", str(args.block_size),
                   "--queue-depth", str(args.queue_depth),
                   "--files-per-root", str(args.files_per_root), *args.root]
        started = time.perf_counter()
        completed = subprocess.run(command, capture_output=True, text=True,
                                   timeout=args.timeout)
        elapsed = time.perf_counter() - started
        (args.output / f"{stem}.out").write_text(completed.stdout)
        (args.output / f"{stem}.err").write_text(completed.stderr)
        rows.append((threads, mode, schedule, repeat, completed.returncode, elapsed))
        print(f"threads={threads} mode={mode} schedule={schedule} repeat={repeat} "
              f"elapsed={elapsed:.3f}s rc={completed.returncode}", flush=True)
        if completed.returncode != 0:
            raise SystemExit(f"failed; see {args.output / (stem + '.err')}")

    with (args.output / "timings.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("threads", "mode", "schedule", "repeat", "returncode",
                         "elapsed_seconds"))
        writer.writerows(rows)
    (args.output / "metadata.txt").write_text(
        f"timestamp={dt.datetime.now(dt.timezone.utc).isoformat()}\n"
        f"binary={args.binary}\nblock_size={args.block_size}\n"
        f"queue_depth={args.queue_depth}\nfiles_per_root={args.files_per_root}\n"
        f"roots={' '.join(args.root)}\n")


if __name__ == "__main__":
    main()
