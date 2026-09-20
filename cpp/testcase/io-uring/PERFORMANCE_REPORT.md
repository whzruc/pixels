# io_uring Multi-Backend Performance Conclusions

## Scope

This report summarizes two experiments on 24 Samsung NVMe SSDs: an end-to-end ClickBench q24 query and a raw I/O scan that bypasses DuckDB, PixelsReader, decoding, and materialization. The compared backends are ordinary buffers (non-fixed), dynamically registered fixed buffers (dynamic), and statically registered fixed buffers (static). Raw output and per-run measurements remain local and are not included in Git.

## Executive conclusions

1. q24 is not sensitive to the buffer registration strategy. Across 12, 24, and 48 threads, the largest end-to-end difference was 2.75%, below the predefined 5% practical-significance threshold.
2. In the raw scan at 12 threads, before platform bandwidth saturation, dynamic and static were about 4% faster than non-fixed.
3. Aggregate throughput reached about 109.6 GB/s at 24 threads. Moving to 48 threads improved throughput by only about 0.3%–0.6%, indicating platform saturation.
4. Once saturated, the three backends differed by less than 0.5%. Lower fixed-buffer CPU and kernel overhead could no longer translate into additional device throughput.
5. Dynamic and static were effectively equivalent during steady-state scanning. This experiment did not measure dynamic grow/update, initialization, memory consumption, or registration failures, so it does not establish equal lifecycle costs.
6. Assigning exactly one thread to each device did not improve performance; it was 0.38%–0.83% slower than the shared interleaved queue. The shared queue can absorb small device and file-level imbalances.
7. One SSD reached about 6.14 GB/s, while the 24-device average was about 4.55 GB/s. The scaling curve and PCIe topology point to a shared platform I/O path—not the scanner or work queue—as the main source of the roughly 109 GB/s aggregate ceiling.

## Raw I/O scan

Each run scanned 15,360 files and 1,916,931,234,624 bytes using 1,836,072 requests of 1,048,576 bytes. Queue depth was 32 per thread. Each combination had one warm-up and three measured runs; the table reports measured medians.

| Threads | non-fixed (GB/s) | dynamic (GB/s) | static (GB/s) |
|---:|---:|---:|---:|
| 12 | 66.18 | 68.80 | 68.74 |
| 24 | 109.57 | 109.57 | 109.70 |
| 48 | 109.87 | 110.14 | 110.34 |

The roughly 4% fixed-buffer advantage at 12 threads indicates better I/O-path efficiency in an unsaturated workload. At 24 threads, aggregate platform bandwidth dominates and the three implementations converge.

## Shared queue versus one thread per device

With 24 threads and 24 devices, device-affine scheduling changed only file ownership; backend, request size, and queue depth remained identical.

| Backend | Shared queue (GB/s) | One thread per device (GB/s) | Relative change |
|---|---:|---:|---:|
| non-fixed | 109.57 | 109.12 | -0.41% |
| dynamic | 109.57 | 109.15 | -0.38% |
| static | 109.70 | 108.79 | -0.83% |

The small differences rule out the shared work queue as a material bottleneck. Three repetitions are not enough to claim statistical significance for sub-percent effects.

## Device scaling and the platform ceiling

A short non-fixed scan produced the following scaling trend:

| SSDs | Aggregate (GB/s) | Per SSD (GB/s) |
|---:|---:|---:|
| 1 | 6.14 | 6.14 |
| 4 | 23.47 | 5.87 |
| 8 | 44.07 | 5.51 |
| 12 | 54.18 | 4.52 |
| 24 | 109.12 | 4.55 |

The 24 NVMe devices are distributed across four PCIe root groups in a 4, 8, 8, 4 layout. The 4-, 12-, and 24-device results—about 23.5, 54.2, and 109.1 GB/s—are consistent with stepwise scaling as additional PCIe root groups participate. Strictly, the evidence localizes the ceiling to the shared PCIe/CPU I/O fabric, DMA, or memory path; it does not identify the exact limiting component inside a Root Complex.

## End-to-end q24

q24 reads all columns, applies `URL LIKE '%google%'`, sorts by `EventTime`, and returns 10 rows. The data was distributed across the same 24 SSDs. DuckDB and Pixels reader used the same thread count. Each combination had one warm-up and seven measured runs, randomized with a fixed seed. All 63 measured runs produced identical query results and completed without errors. The table reports median end-to-end wall-clock time; lower is better.

| Threads | non-fixed (s) | dynamic (s) | static (s) | Fastest backend |
|---:|---:|---:|---:|:---|
| 12 | 391.134 | 387.452 | 386.226 | static |
| 24 | 213.743 | 212.269 | 213.898 | dynamic |
| 48 | 147.892 | 150.814 | 151.959 | non-fixed |

Relative differences were:

- At 12 threads, dynamic was 0.95% faster than non-fixed, and static was 1.27% faster.
- At 24 threads, dynamic was 0.69% faster than non-fixed and 0.77% faster than static.
- At 48 threads, non-fixed was 1.98% faster than dynamic and 2.75% faster than static.

Every difference remained below the predefined 5% practical-significance threshold. Because q24 includes string filtering, sorting, and result materialization, the I/O submission path is only part of total runtime. It is useful for showing that the multi-backend implementation introduces no material end-to-end regression, but it cannot amplify the pure-I/O benefit of fixed buffers. From 12 to 48 threads, non-fixed, dynamic, and static achieved approximately 2.65x, 2.57x, and 2.54x speedups respectively; non-fixed scaled slightly better at high concurrency.

## Engineering assessment

The multi-backend implementation preserves different buffer lifecycle strategies while reaching the same platform peak throughput. A default backend should not be selected from saturated GB/s alone. Future evaluation should include CPU cycles per byte, request latency, registration and update counts, initialization time, lock contention, and memory use. The dynamic-backend performance regression can remain a separate follow-up investigation.
