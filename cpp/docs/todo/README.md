# Pixels I/O 性能研究 TODO

本目录保存尚未实施或尚未完成验证的性能研究设计。它们不是已经确认的优化结论，而是根据当前 q24、24 SSD、12/24/48 worker 实验形成的工作假设。

## 研究项目

| 项目 | 核心问题 | 当前优先级 |
|---|---|---|
| [SPDK/io_uring：I/O 与计算的 Scale 分析](01-io-vs-compute-scaling.md) | worker 增加时，I/O 等待、I/O CPU 和解码计算是否以相同速度膨胀？ | P0：其他优化的统一基线 |
| [io_uring 三种工作模式](02-iouring-polling-modes.md) | interrupt、SQ polling、CQ/I/O polling分别适合什么负载？ | P1 |
| [SPDK 共享 Completion Poller](03-spdk-shared-completion-poller.md) | 是否应由固定少量 core集中提交和收割 completion？ | P1，先补计数器 |
| [Metadata 异步化](04-metadata-async-pipeline.md) | 如何把逐文件同步依赖链改成跨文件批量异步 pipeline？ | P0 |
| [I/O、Compute 与 Pipeline 性能边界](05-io-compute-pipeline-ceilings.md) | 设备、软件 I/O、纯计算和完整流水线各自的极限在哪里，阶段之间损失了多少？ | P0：优化前的性能上界 |

## 统一数据来源

- io_uring 最新阶段数据：`comparison-total-20260821_154045-iouring-metadata-attribution-v2-add-stage`
- SPDK attribution 数据：`comparison-total-20260816_195615-spdk-sync-attribution-v2`
- 当前汇总分析：[PERFORMANCE_ANALYSIS](../../testcase/performance-test/PERFORMANCE_ANALYSIS.md)

不同日期的 io_uring 与 SPDK 结果只用于发现趋势，不用于解释很小的绝对差异。所有候选实现都应在同一次构建、同一数据集和相同 profiler 口径下重新建立 baseline。

## 统一验收原则

1. wall time至少重复三次，报告中位数、最小值和离散程度；
2. 累计 thread-time不能直接与 wall time相加；
3. 同时报告 task-clock、cycles、instructions、cache misses和context switches；
4. I/O 优化必须同时检查 CPU 成本，不能只检查设备吞吐；
5. 所有结果按 query、读取字节数或输出行数归一化，避免把工作量变化误认为效率变化。
