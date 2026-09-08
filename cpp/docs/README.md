# Pixels C++ 文档索引

本目录保存与实现长期对应的设计、配置和使用文档。测试脚本的运行方法与实验结论保留在各自的 `testcase/<suite>/` 目录中。

本目录的设计源文档已从 2026-09-05 归档恢复。归档中的 PDF、LaTeX
中间文件和模板 ZIP 属于可再生成产物或第三方附件，不作为维护源文件提交。

## 功能

- [CPU 线程绑核](features/cpu-affinity.md)
- [列大小收集](features/column-size-collection.md)
- [ColumnVector 重用优化](features/column-vector-reuse.md)

## Buffer Pool

- [Global ByteBuffer Pool](buffer-pool/global-buffer-pool.md)
- [Global Static Buffer Pool 与 Ring Pool](buffer-pool/global-static-buffer-pool.md)

这两篇分别描述动态池和静态池。列大小收集是静态池初始化的输入；ColumnVector 重用则是另一层对象复用，暂不与 Buffer Pool 文档合并。

## I/O

- [Parquet io_uring 双缓冲](io/parquet-uring.md)
- [SPDK I/O 方案](io/spdk.md)

## 性能测试与分析

- [测试目录总览](../testcase/README-zh.md)
- [统一性能测试套件](../testcase/performance-test/README.md)
- [性能瓶颈分析](../testcase/performance-test/PERFORMANCE_ANALYSIS.md)
- [Buffer Pool 测试](../testcase/buffer-pool/README.md)
- [io_uring 测试](../testcase/io-uring/README.md)
- [并行扫描测试](../testcase/parallel-scan/README.md)
- [SPDK 测试工具](../testcase/spdk/README.md)

## Git 分支与 PR

- [当前线性分支关系](io/git-pr-path.zh-CN.md)
- [Parquet PR 说明](io/parquet-pr-description.md)
- [SPDK PR 说明](io/spdk-pr-description.md)

## 待验证设计

- [Pixels I/O 性能研究 TODO](todo/README.md)：I/O/计算 scale、io_uring polling、SPDK共享poller与metadata异步化

## 文档维护约定

- 项目构建、入门和常见问题放在根目录 `README.md`。
- 跨测试复用的实现设计与配置放在 `docs/`。
- 测试命令、结果格式和实验结论放在对应 `testcase/<suite>/`。
- 自动生成的 `RUN_META.txt`、perf、iostat 和图表属于测试结果，不作为项目文档提交。
- PDF、LaTeX `.aux`、Overleaf ZIP 和会议模板压缩包从对应 Markdown、TeX
  与 BibTeX 源文件重新生成，不在仓库中维护重复副本。
