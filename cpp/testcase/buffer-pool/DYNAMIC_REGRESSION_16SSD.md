# 16 SSD Dynamic BufferPool regression profile

The manual runner compares `legacy`, `dynamic`, and `static` with double
buffering enabled on the 20 largest Dynamic regressions from the earlier
24-SSD, 48-thread PR:

```text
q25 q28 q26 q27 q43 q37 q30 q39 q40 q07
q38 q42 q02 q08 q22 q41 q23 q21 q01 q31
```

They are ordered by the measured Dynamic/Legacy speedup, from `0.180x` for
q25 to `0.585x` for q31 (approximately 455% to 71% slower). The list comes
from `results/full-24ssd/query_speedups.csv`; the new 16-SSD run must be
treated as a fresh measurement rather than assuming the ordering is unchanged.

Run from the C++ repository root:

```bash
chmod +x testcase/buffer-pool/run_dynamic_regression_16ssd.sh
sudo -E testcase/buffer-pool/run_dynamic_regression_16ssd.sh
```

The runner copies the installed properties into a temporary `PIXELS_HOME` and
does not modify the original file. It derives `clickbench-pixels-e0-16ssd` from
the first 16 paths of the existing 24-SSD benchmark definition.

Important overrides:

```bash
# Smaller pilot before the full 20-query run
QUERIES="q25 q28 q26" PERF_STAT_REPEAT=1 KEEP_PERF_DATA=0 \
  sudo -E testcase/buffer-pool/run_dynamic_regression_16ssd.sh

# Also collect off-CPU flame graphs (requires BCC/root)
RUN_OFFCPU=1 sudo -E testcase/buffer-pool/run_dynamic_regression_16ssd.sh

# Explicit local paths
DUCKDB_BINARY=/path/to/duckdb \
SOURCE_PROPERTIES=/path/to/pixels-cpp.properties \
COLUMN_SIZE_FILE=/path/to/clickbench-size-e0.csv \
FLAMEGRAPH_DIR=/path/to/FlameGraph \
  sudo -E testcase/buffer-pool/run_dynamic_regression_16ssd.sh
```

Each query/mode directory contains:

- `duckdb.log`: `EXPLAIN ANALYZE` and Pixels profiler output;
- `resource_usage.txt`: `/usr/bin/time -v`, including peak RSS, faults and CPU;
- `perf_stat.txt`: post-initialization cycles, instructions, cache/TLB details,
  context switches, migrations and task-clock;
- `*_oncpu_cpu-clock.svg`: on-CPU flame graph;
- `*.data`: retained `perf.data` for later `perf report` inspection;
- `iostat.txt` and `iostat_summary.txt`: the 16 devices' throughput, IOPS,
  await, queue depth and utilization.

`summary_wall_time.csv` and `summary_io.csv` provide suite-level summaries.
Static initialization is excluded from attach-mode `perf stat`, while
`resource_usage.txt` intentionally retains whole-process peak RSS and startup
cost. Compare both instead of using only one measurement.
