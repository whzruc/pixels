# C++ Buffer Pool Validation

The C++ reader supports three values for `pixel.bufferpool.mode`:

- `legacy`: the existing buffer pool and io_uring registration path.
- `dynamic`: thread-local buffers allocated and registered on demand.
- `static`: per-column double buffers preallocated for a bounded number of scan threads.

The static pool uses `pixel.column.size.path` to size each column buffer and
`pixel.static.buffer.threads` to bound the number of preallocated thread slots.
Set `pixel.static.buffer.hugepage=true` to request transparent HugePages with
`MADV_HUGEPAGE`. This is an advisory optimization and requires Linux support.

Set `pixel.bufferpool.stats.enabled=true` to print process-lifetime internal
buffer pool statistics on exit. The option is disabled by default. Output is
written to stderr in a machine-readable form:

```text
[BufferPoolStats] mode=dynamic current_allocated_bytes=0 peak_allocated_bytes=1048576 current_registered_bytes=0 peak_registered_bytes=1048576 allocations=4 frees=4 registrations=0 registration_updates=6 reuses=20 growths=2
```

Allocated and registered byte counters cover Pixels-managed I/O buffers only;
they do not include DuckDB operators, column vectors, thread stacks, or other
process memory. `registrations` counts buffers registered during initialization
or pool growth, while `registration_updates` counts dynamic slot updates.

Build the project, then compare the three modes with the same ClickBench data:

```bash
testcase/buffer-pool/validate-clickbench.sh \
  /home/whz/pixels/clickbench-size-e0.csv \
  '/data/9a3-01/clickbench/pixels-e0-fb/*' \
  1
```

The script runs a row count, a filtered count, and a numeric aggregate. It
fails if any mode returns a different result. Increase the thread count only
after accounting for static-pool memory: each thread owns two buffers for every
column.

## 24-SSD performance experiment

The full experiment compares `legacy`, `dynamic`, `static`, and
`static-hugepage` with 12, 24, and 48 threads. Each configuration is run three
times in randomized order. One DuckDB process executes q00 as a warmup followed
by q01 through q43, so static-pool initialization is paid once rather than once
per query.

The runner records:

- a separate cold-start process containing initialization and q00, with its own
  CPU, RSS, page-fault, cache, and dTLB measurements;
- initialization, warmup, and per-query wall time;
- process CPU time and maximum resident memory;
- cycles, instructions, cache misses, dTLB misses, page faults, context
  switches, and CPU migrations;
- raw query output hashes as a correctness warning;
- raw DuckDB, `/usr/bin/time`, and `perf stat` output.

Run from the C++ repository root:

```bash
python3 testcase/buffer-pool/run-24ssd-benchmark.py \
  --root testcase/buffer-pool/results/preflight \
  --dry-run

testcase/buffer-pool/run-24ssd-benchmark.sh
```

An explicit result directory and runner overrides can be supplied:

```bash
testcase/buffer-pool/run-24ssd-benchmark.sh \
  testcase/buffer-pool/results/manual-run \
  --repeats 3 \
  --threads 12 24 48
```

The command is resumable. Run the same command again with the same result
directory to skip completed `(mode, threads, repeat)` cases. Use a small pilot
before the complete experiment:

```bash
testcase/buffer-pool/run-24ssd-benchmark.sh \
  testcase/buffer-pool/results/pilot \
  --repeats 1 \
  --threads 12 \
  --queries q01 q02
```

The complete experiment scans approximately 1.8 TiB across 24 SSDs and can run
for many hours. Static double buffers require approximately 639 MiB per thread
with the current ClickBench column-size file, excluding DuckDB operator memory.
During the pilot, a 48-thread high-cardinality aggregation exceeded 300 GiB RSS,
so check that the machine is otherwise idle and has sufficient available memory.
Run from a normal host shell: restricted sandboxes may reject io_uring setup with
`EPERM`. The runner also refuses to start while another DuckDB process is active.

Monitor a running experiment:

```bash
tail -f testcase/buffer-pool/results/manual-run/runner.log
column -s, -t testcase/buffer-pool/results/manual-run/suite_metrics.csv
```

If the terminal or process is interrupted, rerun the same command and result
directory. Completed cases and completed cold-start measurements are skipped.

To regenerate summaries without rerunning queries:

```bash
python3 testcase/buffer-pool/analyze-24ssd-results.py \
  testcase/buffer-pool/results/manual-run
```

Use `summary.csv` to separate the effects:

- allocation bottleneck: compare dynamic/legacy steady-query speedup together
  with initialization time, page faults, system CPU time, and RSS;
- preallocation tradeoff: compare static with dynamic and inspect whether lower
  steady latency compensates for static initialization and memory usage;
- HugePage impact: compare static-hugepage with static at the same thread count,
  especially dTLB misses, minor faults, and query latency.

## Full internal-statistics experiment

The full statistics experiment enables process-lifetime counters for the normal
three-repeat suite and also runs every q01-q43 query in an isolated process once.
The isolated phase attributes buffer allocation, registration, reuse, growth,
process RSS, and perf counters to one query without contamination from earlier
queries. Its query time includes cold initialization and must not be compared to
the warmed suite latency.

Run a preflight first:

```bash
python3 testcase/buffer-pool/run-24ssd-benchmark.py \
  --root testcase/buffer-pool/results/full-buffer-stats \
  --threads 12 24 48 \
  --modes legacy dynamic static static-hugepage \
  --repeats 3 \
  --buffer-pool-stats \
  --isolated-query-stats \
  --isolated-repeats 1 \
  --dry-run
```

Then launch the resumable full run:

```bash
testcase/buffer-pool/run-full-buffer-stats-experiment.sh \
  testcase/buffer-pool/results/full-buffer-stats
```

The expected output contains 36 cold cases, 36 warmed-suite cases, and 516
isolated query cases. The wrapper analyzes the CSV files and fails its final
completeness check if a case is missing or `failures.csv` is nonempty. Re-run the
same command and result directory to retry only missing cases.
