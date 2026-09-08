# PR7: DuckDB Pixels Parallel Scan

This report describes the changes and validation for the PR7 delta after the
footer-cache lifetime work (`2a225ca6`). Raw benchmark output and dataset files
are intentionally excluded from the repository.

## Changes

- Hardened parallel scan state initialization and progress handling in the
  DuckDB bind/scan path, including empty-storage and end-of-file guards.
- Preserved device-aware storage scheduling while making the shared scan state
  safe for parallel workers.
- Added optional worker CPU-affinity configuration and focused parsing tests;
  the production default remains disabled.
- Added a standalone native io_uring scan benchmark covering non-fixed,
  dynamic, and static buffer backends, shared scheduling, device-affine
  scheduling, repeated runs, return-code checks, and automatic root discovery.
- Documented the reproducible validation procedure and kept generated results
  local-only.

## Functional validation

The following focused tests passed with `PIXELS_SRC` and `PIXELS_HOME` pointed
at the same host checkout:

- `CPUAffinityTest`: 3/3 passed
- `PixelsFooterCacheTest`: 4/4 passed
- `ColumnVectorBufferPoolTest`: 2/2 passed
- `DynamicBufferPoolTest`: 3/3 passed
- `GlobalStaticBufferPoolTest`: 1/1 passed
- `DirectUringNonFixedTest`: 1/1 passed

The complete ClickBench query set (`q01`–`q43`) was executed once with six SSD
roots and 24 DuckDB threads: 43/43 queries passed with exit status 0. Q24 was
also executed with 24 SSDs and 48 threads: exit status 0. No SQE, queue-full,
or I/O errors were observed in these validations.

## Performance validation

The native scan benchmark read 3072 files (368.77 GB decimal) from 24 SSDs,
with three repetitions per configuration. Median elapsed times were:

| Threads | Non-fixed | Dynamic | Static |
| ---: | ---: | ---: | ---: |
| 12 | 5.91 s | 5.63 s | 5.72 s |
| 24 | 3.72 s | 3.86 s | 4.06 s |
| 48 | 3.90 s | 4.10 s | 4.50 s |

At 24 threads with device-affine scheduling, medians were 3.64 s (non-fixed),
3.79 s (dynamic), and 4.08 s (static). All benchmark invocations returned
zero. These measurements characterize the standalone I/O path; they are not
claimed as a general DuckDB query speedup.

An end-to-end Q24 run on 24 SSDs and 48 threads read approximately 1.924 TB in
140.07 s, or about 13.73 GB/s, with a peak resident set of approximately
74.1 GB. A six-SSD Q24 run also completed successfully at 1, 12, 24, and 48
threads.

## Scope and follow-up

This PR does not resolve the previously observed Dynamic BufferPool performance
regression, does not change the unsupported 96-thread configuration, and does
not include raw test data.
