# SPDK/io_uring：I/O 与计算的 Scale 分析

## 为什么做

当前 wall time只告诉我们查询是否变快，不能回答 worker 从12增加到48时，新增资源究竟消耗在设备等待、completion polling还是列解码。尤其是 doublebuffer 会让 I/O 和计算重叠：wall time下降的同时，累计计算时间可能上升。

建立统一的 I/O/计算分类后，才能判断后续应该优化 poller、metadata pipeline、解码并发度还是 NUMA 布局，也能用相同口径比较 io_uring 与 SPDK。

## 现有证据

最新 io_uring 48线程结果：

| 指标 | Single | Double | 变化 |
|---|---:|---:|---:|
| Wall time | 48.81 s | 45.89 s | -6.0% |
| `Uring.AsyncComplete.WaitCQE` | 266.06 thread-s | 4.64 thread-s | -98.3% |
| `ReadDataColumns.Decode` | 1471.58 thread-s | 1588.98 thread-s | +8.0% |
| task-clock | 2009.74 s | 2146.37 s | +6.8% |
| instructions | 10.870 万亿 | 10.887 万亿 | +0.15% |
| cache misses | 14.006 billion | 15.002 billion | +7.1% |

这说明 doublebuffer 消除了大部分 worker可见 I/O 等待，却让更多 worker同时解码，造成 cache/内存竞争。累计 Decode 增长并不代表多处理了8%的数据。

SPDK 旧数据中，singlebuffer `AsyncComplete.Poll` 累计约291～336秒，doublebuffer降至约0.94～7.55秒；48线程同步 metadata poll仍约96秒。SPDK polling既属于 I/O等待，又消耗真实 CPU，不能简单归入“计算”。

## 预期效果

本项目主要产出可解释性，本身不保证降低 wall time。它应能：

- 定位每个线程规模下 I/O stall、I/O control CPU、polling CPU和计算的增长斜率；
- 识别超过有效扩展点的 worker数量；
- 给后续优化提供可验证的收益上限；
- 避免把数百 thread-seconds误写成数百秒 wall time收益。

根据当前 io_uring 数据，如果仅通过限制解码竞争回收 doublebuffer 多出的约6.8% task-clock，同时保持 I/O 重叠，48线程 wall time可研究的目标区间约为1%～5%。这是实验目标，不是由累计时间直接推出的保证。

## 分类草案

### io_uring

```text
I/O stall
  Uring.AsyncComplete.WaitCQE
  Pixels.Metadata.FileTailOffsetRead
  Pixels.Metadata.FileTailRead
  Pixels.Metadata.RowGroupFooterRead

I/O control CPU
  Uring.AsyncSubmit.Total
  Uring.AsyncComplete.ProcessCQE
  read.AllocateBuffers

Compute
  ReadDataColumns.Decode
  ReadFilterColumns
  ApplyFilterExpr
  TransformOutput
  ApplyFilter

Control/Other
  StateNext.LockWait/CloseReader/SwitchBuffer/
  BuildPixelsReader/CreateRecordReader
```

### SPDK

```text
I/O polling CPU
  Spdk.AsyncComplete.Poll
  Spdk.SyncRead.Poll

I/O control CPU
  Spdk.AsyncRead.Submit
  Spdk.SyncRead.Submit
  Spdk.AsyncComplete.ReleaseOps
  Spdk.SyncRead.DmaAllocate/Copy/DmaFree

Compute、Control/Other
  与 io_uring 使用相同的 Pixels 上层标签
```

嵌套标签不能重复求和。例如 `RowGroupFooterRead` 嵌套在 `read.Total` 内，`WaitCQE` 嵌套在 `AsyncComplete.Total` 内。

## 实现草案

1. 建立一个解析脚本，将每次 `duckdb.log`、`perf_stat.txt` 和 wall summary转换为统一 CSV；
2. CSV 至少记录 backend、mode、threads、repeat、wall、bytes、rows及上述互斥分类；
3. 增加 request count、read bytes、batch count，使结果可以按GB和每batch归一化；
4. SPDK用新增阶段标签重新运行12/24/48线程，避免与旧数据混用；
5. 生成三组图：累计thread-s、等价并发度、每GB成本；
6. 如需严格 wall critical path，为每个文件记录低开销 begin/end事件，不能从汇总 thread-time反推。

核心派生指标：

```text
io_equivalent_concurrency      = io_thread_s / wall_time
compute_equivalent_concurrency = compute_thread_s / wall_time
poll_cpu_per_completion        = poll_thread_s / completion_count
compute_seconds_per_GB         = compute_thread_s / bytes_read
```

## 验证矩阵

```text
backend: io_uring, SPDK
mode: singlebuffer, doublebuffer
threads: 12, 24, 48
repeat: 3
query: q24
ssd: 24
profiler: Pixels + perf stat；off-CPU每种模式至少一次
```
