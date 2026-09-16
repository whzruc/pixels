# PR: Dynamic BufferPool 选择性扩容 V1

## Summary

增加一个默认关闭的实验模式：Dynamic double buffer 在读完文件 metadata、提交 column
chunk I/O 之前，如果本地 buffer 不足，优先把文件任务送到同 storage group 中已经拥有
足够大双 buffer 的 worker；没有接收者或队列已满时继续走原本地扩容路径。

目标是减少大 column chunk 在全部线程上复制高水位 buffer 的概率，同时维持无等待的
本地扩容退路。V1 只支持单 row group 文件，并要求接收者两套逐列 buffer 都满足需求。

## Changes

- 查询级容量注册表、每 worker 有界队列、同组选择和一次转移限制；
- metadata-only 逐列、Direct-I/O 对齐后的需求计算；
- DynamicBufferPool owner-thread 容量快照和查询级增长/转移统计；
- 对线程迁移、重叠 TLS owner、错误 buffer 模式和多 row group fail closed；
- 修复 Direct-I/O metadata slice 未持有 backing allocation 的生命周期问题；
- 修复部分 row-group footer cache hit/miss 混合时的索引错误；
- 无硬件调度单测、真实文件结果一致性/性能/perf/火焰图脚本；
- 完整设计、限制、测量口径和测试记录文档。

## Configuration

默认关闭：

```properties
pixels.dynamic.selective.enabled=false
pixels.dynamic.selective.queue.limit=1
```

queue.limit 可取 0..64。0 启用 metadata/统计但禁止转移，作为原 Dynamic 与选择性调度
之间的消融基线。完整约束见 `docs/buffer-pool/selective-growth-v1-design.md`。

## Verification performed

- Release shell 和 CMake 测试 target 编译通过；
- 调度器：10 × 20,000 文件、8 worker，恰好执行一次；
- ASan/UBSan 调度单测通过（受 ptrace 限制，关闭 LeakSanitizer）；
- 16 个真实 Pixels 文件：最终五模式、四类查询结果一致；
- q21/q37/q24 小样本，各三次 Dynamic/selective-0/selective-1 运行通过；
- q21 Dynamic/selective-1 的独立 whole-process perf stat、perf record 和 SVG 生成通过。

完整设计、测试命令、结果和解释边界统一记录在
`docs/buffer-pool/selective-growth-v1-design.md`。

## Risk and rollout

这是实验 PR，不改变默认行为。未完成完整 16 SSD × 21 查询 × 7 模式性能验收；小样本已表明
任务转移不一定降低观察 buffer 容量，q37 还出现轻微耗时增加，q24 没有发生转移。因此合并后
仍保持开关关闭，只有完整矩阵证明收益、且逐查询退化在可接受范围后再考虑默认启用。

后续主要风险是双槽适配条件过于保守，以及 metadata cache 增量内存。若真实转移率太低，
下一步应实现显式 slot 生命周期/绑定，而不是简单放大队列。
