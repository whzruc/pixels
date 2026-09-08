# Benchmark Results

`run-24ssd-benchmark.sh` creates one timestamped directory per experiment here.
Each directory contains raw DuckDB output, `perf stat` counters, process resource
usage, per-query timings, and generated CSV/Markdown summaries.

Timestamped results are ignored by Git because a complete experiment can be
large. Selectively add a final `summary.csv`, `query_summary.csv`, and
`report.md` when results should accompany a pull request.
