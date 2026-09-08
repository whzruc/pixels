# Parquet io_uring Backend — Pull Request Description

## Summary

This PR completes the Parquet io_uring scan integration and restores the
corresponding benchmark/test entry points. It supports asynchronous Parquet
scanning with single-buffer and double-buffer modes, plus a synchronous pread
baseline.

## What changed

- Completed the Parquet scan pipeline and profiling summaries for scan,
  decode, Arrow-to-DuckDB conversion, I/O submission/wait, and initialization.
- Restored the Parquet benchmark wrapper with explicit defaults for Q24,
  24 SSDs, and 48 worker threads.
- Restored Parquet reader performance test sources and configuration files.
- Added/updated CMake test registration for the Parquet reader benchmark.
- Preserved the existing `pq-async-singlebuffer`,
  `pq-async-doublebuffer`, and `pq-pread` modes.

## Testing

The following runs completed successfully with `EXPLAIN ANALYZE`:

| Workload | Mode | Threads | Runtime |
|---|---|---:|---:|
| Q24, 1 SSD | async single-buffer | 48 | 33.60 s |
| Q24, 1 SSD | async double-buffer | 48 | 35.68 s |
| Q24, 16 SSD | async single-buffer | 48 | 214.90 s |
| Q24, 16 SSD | async double-buffer | 48 | 205.46 s |
| Q24, 24 SSD | pread | 48 | 243.62 s |

The Parquet pipeline profiling output was also generated successfully. For
the 16-SSD Q24 run, the double-buffer profile reported 0.021 s in the I/O wait
stage, while the single-buffer run spent 63.119 s waiting for I/O. This shows
that the pipeline is functional, but it also exposes substantial remaining
performance overhead elsewhere in the Parquet path.

## Performance status and follow-up work

The current Parquet implementation is functionally usable but performance is
currently poor compared with the Pixels-native path. The main follow-up areas
are:

- reduce Arrow-to-DuckDB conversion and decode overhead;
- improve scheduling and overlap between I/O, decode, and export stages;
- reduce per-file/per-row-group reader construction overhead;
- review buffer ownership, allocation, and reuse in the Parquet path;
- repeat the comparison after the above optimizations on identical hardware.

This PR should therefore be treated as an integration and correctness
baseline, not as a final Parquet performance optimization.

Raw benchmark logs, perf data, flame graphs, and generated result directories
are intentionally excluded from the commit.
