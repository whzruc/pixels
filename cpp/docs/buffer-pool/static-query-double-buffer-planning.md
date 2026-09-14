# 静态查询的双缓冲容量规划

## 目标

保留 double buffer 的 I/O 与 decode 重叠，同时避免为两个 slot 都分配
`每列全局最大 chunk`。第一阶段只考虑文件集合、投影列和调度顺序在执行前不变，
且同一时刻只执行一个 query 的场景。

## 当前问题与内存下界

对 worker `w`、pipeline slot `s` 和列 `c`，安全容量为：

```text
capacity(w, s, c) = align(max(chunk_bytes(w, sequence, c)
                              where sequence % 2 == s))
```

因此静态 query 的最小安全 I/O buffer 容量是所有 `capacity` 之和。传统对称分配
对两个 slot 都使用 `max(capacity(w, 0, c), capacity(w, 1, c))`，会在两个 slot
的数据分布不对称时浪费内存。该规划不减少 pipeline slot 数，不改变提交顺序，
也不在热路径 shrink/reallocate，因而不会破坏 double buffer 的 overlap。

这个下界不包含 decoder/ColumnVector、footer cache、io_uring ring 和 SPDK 控制内存。

## Trace 与规划器

输入 CSV：

```csv
worker,sequence,column,bytes
0,0,Title,73400320
0,1,Title,12582912
0,2,Title,68157440
```

`sequence` 必须是该 worker 实际消费文件或 row group 的零基序号，而不是全局文件号。
这点很重要：buffer switch 发生在 worker 自己的 pipeline 中。

运行：

```bash
python3 testcase/buffer-pool/static_query_buffer_plan.py trace.csv \
  --alignment 4096 -o plan.json
python3 -m unittest testcase/buffer-pool/test_static_query_buffer_plan.py
```

输出包含逐 `(worker, slot, column)` 容量以及：

- `planned_bytes`：slot-aware 计划的总容量；
- `symmetric_bytes`：相同 trace 下两套对称 buffer 的容量；
- `saved_bytes` / `saved_ratio`：理论可安全回收的容量。

## 接入方案

本 PR 先固定规划算法与实验输入，生产接入按以下顺序进行：

1. 在现有调度点记录 `worker, sequence, column, aligned_bytes`，确认 trace 与真实
   `readAsync` 请求一致；
2. 静态 query 在执行前从已有 metadata/manifest 生成计划，不能为了规划再次同步读取
   所有 footer，否则会把内存收益换成 query latency；
3. 每个 worker 一次性为两个 slot 按计划 acquire 并注册 buffer；执行阶段只查表，
   禁止 grow、registration update 和 shrink；
4. 若实际请求超过计划，查询必须 fail-fast 并报告键和值。性能实验阶段不允许静默
   fallback 到热路径扩容，否则结果无法解释；
5. query 结束统一释放 lease。后续并发 query 再增加全局 budget 与 admission control，
   不在本阶段混入。

## 性能与正确性门禁

使用同一静态 query、相同 worker/file 调度和冷热缓存条件，single buffer、现有 dynamic
double buffer、planned double buffer 各运行至少 5 次，报告 median/P95：

- query wall time、CPU time、`AsyncSubmit`、`AsyncComplete`；
- peak RSS、planned/registered/requested bytes；
- allocation、grow、registration update 次数；
- 每个 worker 的 slot high-water mark。

planned 模式的执行期 allocation/grow/update 必须为 0，查询结果一致；query-only median
不得比现有 double buffer 退化 2%，同时 peak registered buffer bytes 应接近
`planned_bytes`。若 metadata 中没有完整尺寸信息，则保留现有 dynamic 路径，不用猜测值。

## 为什么暂不采用运行时缩容

完成 decode 后缩小旧 slot 看似节省内存，但会在后续大 chunk 到来时重新分配和更新
registered buffer；这正是 dynamic 路径可能退化的来源，也会增加 page fault 和 allocator
抖动。静态场景应先用完整序列计算 high-water mark，把分配移出执行热路径。
