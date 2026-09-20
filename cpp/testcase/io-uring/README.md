# io_uring backend comparison

`benchmark-q24-backends.py` compares the three new buffer strategies used by
the Pixels io_uring reader:

- `non-fixed`: ordinary aligned buffers submitted with `io_uring_prep_read`;
- `dynamic`: buffers registered and updated on demand;
- `static`: buffers preallocated and registered before scanning.

The benchmark runs ClickBench q24 because it scans URL and EventTime, filters
strings, sorts the matches, and therefore provides a compact I/O-heavy
end-to-end comparison. Each mode is warmed once, then measured in randomized
order in a fresh DuckDB process. The script verifies output hashes and reports
median time, median absolute deviation, pairwise speedup, and an exact
permutation p-value. A difference is marked clear only when `p < 0.05` and the
median difference is at least 5%.

Run from the C++ repository root:

```bash
python3 testcase/io-uring/benchmark-q24-backends.py \
  --pixels-glob '/data/ssd1/clickbench/*.pxl' \
  --pixels-glob '/data/ssd2/clickbench/*.pxl' \
  --column-sizes /path/to/clickbench-column-sizes.csv \
  --threads 12 24 48 \
  --repeats 7 \
  --output /tmp/pixels-q24-io-uring
```

Repeat `--pixels-glob` once per storage device. Keeping the glob quoted lets
the Pixels reader expand it instead of asking the shell to pass every file as
an argument.

By default the benchmark covers 12, 24, and 48 threads. Each thread count and
mode is warmed independently; measured runs are randomized across the complete
matrix. Both DuckDB's worker count and `pixel.threads` are set to the requested
value. Results in `summary.csv` are grouped by thread count.

Use `--modes legacy non-fixed dynamic static` when the existing legacy fixed
buffer implementation should also be included. The output directory contains
raw query output, per-run timings, copied configurations, and `summary.csv`.

## Raw physical I/O scan

`PixelsIoUringScanBenchmark` isolates the physical I/O path. It recursively
finds `.pxl` files, reads every byte in fixed-size blocks through io_uring, and
discards each completed buffer without constructing a `PixelsReader`, parsing
footers, decoding columns, materializing vectors, filtering, or sorting.

The timed region excludes file discovery and per-thread ring/buffer setup. Each
worker owns one ring and `queue-depth` reusable aligned buffers. The three modes
use the same block size, queue depth, file list, and thread count:

- `non-fixed` submits ordinary reads into unregistered buffers;
- `dynamic` registers the same number of buffers through `DynamicBufferPool`;
- `static` allocates and registers the same number before the timed scan.

Build it with:

```bash
cmake --build build/release --target PixelsIoUringScanBenchmark -j12
```

Run the randomized 12/24/48-thread comparison across multiple devices with:

```bash
roots=()
for disk in $(seq -w 1 24); do
  roots+=(--root "/data/9a3-${disk}/clickbench/pixels-e0-fb")
done

result_dir="testcase/io-uring/results/raw-scan-24ssd-$(date +%Y%m%d-%H%M%S)"

python3 testcase/io-uring/benchmark-raw-scan-backends.py \
  "${roots[@]}" \
  --threads 12 24 48 \
  --modes non-fixed dynamic static \
  --block-size 1048576 \
  --queue-depth 32 \
  --repeats 7 \
  --output "$result_dir" \
  2>&1 | tee "${result_dir}.log"
```

On the standard test host, `--root` may be omitted. The runner then discovers
all `/data/9a3-*/clickbench/pixels-e0-fb` directories and prints the selected
list before starting. Explicit `--root` options remain available for other
layouts.

`timings.csv` records bytes, request count, elapsed time, and GiB/s for every
run. `summary.csv` reports median throughput, MAD, pairwise speedup, an exact
permutation p-value, and whether the difference is both statistically
significant and at least 5%. Any positive repeat count is accepted, although
fewer than five samples provide little statistical power. If the output
directory already exists, the runner replaces that exact directory.

Before a full run, use `--files-per-root 2` to scan only two files from every
device and validate that the interleaved multi-device schedule works.
