# Footer Cache / Metadata Lifetime Benchmark

This A/B benchmark compares the parent build (`rebuild/cpp-column-vector-reuse`) with the
Footer Cache candidate build. It runs the same query and configuration with 12, 24, and 48
threads, randomizes measured runs, validates result hashes, and records both wall time and
the existing FileTail/RowGroupFooter profiler time.

Build the parent commit and candidate commit into separate directories, then run:

```bash
result_dir="$PWD/testcase/footer-cache/results/run-$(date +%Y%m%d-%H%M%S)"

python3 testcase/footer-cache/benchmark-footer-cache.py \
  --pixels-glob '/data/disk*/clickbench/*.pxl' \
  --column-sizes /path/to/clickbench-size.csv \
  --baseline-duckdb /path/to/parent-build/duckdb \
  --candidate-duckdb "$PWD/build/release/duckdb" \
  --threads 12 24 48 \
  --repeats 5 \
  --warmups 1 \
  --mode non-fixed \
  --output "$result_dir"
```

Use the exact 24-device globs and column-size file used by the io_uring tests. Raw outputs,
generated homes, timings, and summaries stay below `testcase/footer-cache/results/` and must
not be committed. Only a reviewed bilingual conclusion report should be added later.

The candidate is primarily a lifetime/correctness fix. A small or statistically insignificant
wall-time change is acceptable; inspect the footer profiler columns separately from total query
time and do not claim a cache speedup solely from noisy end-to-end timing.

## 中文说明

该脚本对比父提交 `rebuild/cpp-column-vector-reuse` 与 Footer Cache 候选提交，使用完全相同的
查询和配置覆盖 12、24、48 线程。正式运行会随机交错，校验查询结果哈希，并记录端到端时间及
现有 FileTail/RowGroupFooter profiler 时间。

请分别构建父提交和当前提交，再将两个 DuckDB 二进制路径传给脚本。数据必须使用此前 io_uring
实验相同的 24 盘 glob 和 column-size 文件。原始输出、临时 PIXELS_HOME、逐次时间和汇总结果
保存在 `testcase/footer-cache/results/`，不要提交；实验完成后只提交经过审核的中英文结论报告。

本修改首先是生命周期与并发正确性修复。端到端性能变化很小或不显著也属于合理结果；分析时应
将 footer profiler 指标和总查询时间分开，不应仅依据有噪声的墙钟时间宣称缓存加速。
