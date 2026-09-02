# Footer Cache / Metadata Lifetime Performance Conclusions

## Workload

- ClickBench Pixels data on 24 NVMe SSDs (15,360 files)
- Query: ClickBench q24
- Backend: io_uring non-fixed
- Threads: 12, 24, and 48
- One warm-up and five measured runs per build/configuration
- Parent build: `rebuild/cpp-column-vector-reuse`
- Candidate build: Footer Cache/metadata lifetime changes
- Runs were randomized and every result SHA-256 matched

## Results

| Threads | Parent median (s) | Candidate median (s) | Candidate speedup | p-value | Parent footer thread time (s) | Candidate footer thread time (s) |
|---:|---:|---:|---:|---:|---:|---:|
| 12 | 380.865 | 378.485 | 1.006x (+0.63%) | 0.0476 | 3.781 | 3.805 |
| 24 | 203.988 | 201.815 | 1.011x (+1.08%) | 0.1667 | 3.829 | 3.886 |
| 48 | 138.445 | 136.688 | 1.013x (+1.29%) | 0.0476 | 4.735 | 4.549 |

## Conclusion

The candidate completed all cross-device scans without metadata aliasing or result divergence.
End-to-end improvements were small (0.63%–1.29%) and below the 5% practical-significance
threshold. The 12- and 48-thread samples meet the exact permutation test's p < 0.05 criterion,
while the 24-thread sample does not; this should not be interpreted as a broad performance claim.
The main value of this change is safe FlatBuffer metadata ownership, full-path cache identity, and
concurrent first-writer-wins insertion. Footer profiler time was similar between builds, so the
measured wall-time change is not evidence of a large footer-I/O reduction.

Raw outputs and per-run data remain local under `results/` and are not part of the commit.
