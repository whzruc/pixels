# 前置 PR 测试与报告索引

本文档整理当前并行扫描 PR 依赖的前置提交，以及对应的测试入口、结论报告和原始结果位置。
原始日志、性能数据和大体积实验目录只保存在本地，不作为 Git 内容提交。

| 功能/提交 | 主要测试入口 | 结论报告位置 | 备注 |
|---|---|---|---|
| FlatBuffers 序列化（`9e0c2b4f`） | `cpp/tests/` 下 writer、metadata、reader 测试 | 未发现独立性能报告 | 主要是序列化实现迁移，当前仓库没有单独的基准结论文件 |
| C++ profiler 开关（`528721c4`） | `testcase/io-uring/` 中的性能脚本可采集 profiler 数据 | `cpp/testcase/io-uring/PERFORMANCE_REPORT.zh-CN.md` | profiler 是测试观测能力，不是该报告的独立性能对象 |
| Dynamic BufferPool（`2c61ceb2`） | `cpp/tests/BufferPool/DynamicBufferPoolTest.cpp` | `cpp/testcase/io-uring/PERFORMANCE_REPORT.zh-CN.md` | dynamic 与其他 io_uring 后端统一比较；已知性能退化留作后续工作 |
| Global Static BufferPool（`3f84dd61`） | `cpp/tests/BufferPool/GlobalStaticBufferPoolTest.cpp` | `cpp/testcase/io-uring/PERFORMANCE_REPORT.zh-CN.md` | static 后端性能结论与 dynamic/non-fixed 一起记录 |
| Static BufferPool 不可用处理（`94d5b5e8`） | `GlobalStaticBufferPoolTest` | 同上 | 覆盖 io_uring 不可用时的回退/处理路径 |
| HugePage 与 ClickBench 验证（`b6256452`） | `cpp/testcase/buffer-pool/validate-clickbench.sh` | `cpp/testcase/buffer-pool/README.md` | 原始 buffer-pool 结果在本地 `testcase/buffer-pool/results/` |
| io_uring 多后端（`cccc4fce`） | `cpp/testcase/io-uring/PixelsIoUringScanBenchmark.cpp`、相关 Python runner | `cpp/testcase/io-uring/PERFORMANCE_REPORT.zh-CN.md` | 纯 I/O、Q24、shared/device-affine 报告均从该实验体系产生 |
| ColumnVector backing storage 复用（`cc5e62c0`） | `ColumnVectorBufferPoolTest`、`testcase/column-vector/q-string-scan.sql` | `cpp/testcase/column-vector/PERFORMANCE_REPORT.zh-CN.md` | 报告记录 12/24/48 线程 A/B 性能结果 |
| Footer Cache / metadata lifetime（`d9fd46f4`、`2a225ca6`） | `cpp/tests/metadata/PixelsFooterCacheTest.cpp`、footer-cache benchmark | `cpp/testcase/footer-cache/PERFORMANCE_REPORT.zh-CN.md` | 报告记录生命周期、缓存身份和端到端影响 |
| DuckDB 并行扫描（PR7） | `cpp/testcase/parallel-scan/README.md`、本 PR7 报告 | `cpp/testcase/parallel-scan/PR7_REPORT.md`、`PR7_REPORT.zh-CN.md` | q01–q43 全量正确性和 Q24 高并发验证见报告 |

## 原始结果目录

已经提交的仓库内容只包含脚本、README 和结论报告。逐次运行日志通常位于：

- `cpp/testcase/io-uring/results/`
- `cpp/testcase/buffer-pool/results/`

这些目录中的原始输出、环境快照和图表可能是本地未跟踪文件；它们不应通过 `git add .`
加入 PR。
