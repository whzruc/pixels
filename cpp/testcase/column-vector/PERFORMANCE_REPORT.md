# ColumnVector Memory-Reuse Performance Conclusions

## Optimization

The change adds a bounded thread-local reuse pool for the large aligned descriptor array in `BinaryColumnVector` and allocates the writer-only `std::string` container lazily. Buckets are keyed by allocation size and alignment and require no cross-thread lock. Column vectors also track backing-storage ownership so zero-copy views into Reader buffers are never freed as owned memory.

`pixels.columnvector.pool` defaults to `true`. Setting it to `false` restores pre-optimization direct allocation/free and eager string-container construction, enabling an A/B comparison with the same binary.

## Experiment

- Data: ClickBench Pixels data distributed across 24 NVMe SSDs
- Query: `SELECT sum(length(URL)) FROM hits;`
- Purpose: force string-column scanning and BinaryColumnVector construction without q24 sorting and complex computation masking memory-management cost
- I/O backend: non-fixed for both sides
- Threads: 12, 24, and 48
- One warm-up and five measured runs per combination
- 30 measured runs randomized with a fixed seed
- Both sides used the same Release binary and differed only in configuration
- All result SHA-256 hashes matched and every error log was empty

Raw per-run results remain local and are not included in Git.

## Results

| Threads | Before median (s) | After median (s) | Speedup | Before MAD (s) | After MAD (s) | Permutation p-value |
|---:|---:|---:|---:|---:|---:|---:|
| 12 | 15.661 | 15.275 | 1.025x (2.53%) | 0.050 | 0.105 | 0.0476 |
| 24 | 11.727 | 11.674 | 1.005x (0.45%) | 0.185 | 0.163 | 0.7143 |
| 48 | 12.757 | 12.484 | 1.022x (2.19%) | 0.137 | 0.029 | 0.0476 |

## Conclusions

ColumnVector memory reuse improved end-to-end string-scan time by approximately 0.5%–2.5%. The 12- and 48-thread samples reached `p < 0.05` in the exact permutation test for this five-run experiment; the 24-thread difference did not. Every effect remained below the 5% practical-significance threshold, so this is a small workload-dependent optimization rather than a broad large speedup.

The larger effects at 12 and 48 threads are consistent with reducing repeated large-array allocation, release, and first-touch costs. At 24 threads, other system costs dominate near the machine's most effective storage concurrency. The experiment did not separately collect mmap, page-fault, or allocator CPU metrics, so wall-clock time alone cannot quantify each mechanism's contribution.

The primary engineering benefit is bounded, lock-free-per-thread reuse with explicit zero-copy ownership. Performance was modestly positive and no regression was observed in the measured medians.
