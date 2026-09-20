# SPDK 固定少量共享 Completion Poller

## 为什么做

当前普通 SPDK worker在 `readAsyncComplete()` 中直接调用 `spdk_nvme_qpair_process_completions()`。等待未完成请求时，查询worker持续busy-poll，既不能执行Pixels解码，也不会像阻塞I/O一样让出CPU。

“共享poller”不是让多个线程同时操作同一个qpair。SPDK qpair应保持单线程所有权。正确设计是由固定少量poller core拥有qpair，worker通过队列提交请求并接收完成通知。

## 现有证据

旧 SPDK 数据：

| 模式 | 12线程 | 24线程 | 48线程 |
|---|---:|---:|---:|
| single `AsyncComplete.Poll` | 291.12 s | 287.56 s | 335.80 s |
| double `AsyncComplete.Poll` | 0.94 s | 1.57 s | 7.55 s |

48线程 singlebuffer 的 `curr_reader.complete` 为338.69秒，底层 `AsyncComplete.Total` 为338.60秒，说明归因可靠。doublebuffer把它降到约11秒，证明主数据预取本身已经解决大部分completion等待。

48线程仍有约96秒 `Spdk.SyncRead.Poll`，但它来自同步metadata路径；只重构主数据poller不会自动解决metadata polling。

上述数据证明worker polling很贵，却还不能证明固定共享poller一定更快。当前缺少每次poll收割数量和空轮询比例。

## 预期效果

### Singlebuffer

如果大量 `process_completions()` 返回0，专用poller可批量收割并释放查询worker CPU。目标可以是：

- `AsyncComplete.Poll` worker累计时间下降70%～95%；
- task-clock下降5%～15%；
- wall time改善3%～10%，或者在相同wall time下减少所需CPU core。

### Doublebuffer

当前异步poll只剩0.94～7.55秒，因此平均wall time的合理目标只有0%～3%。更重要的收益可能是减少qpair/poller数量、改善尾延迟和为异步metadata提供统一完成基础设施。

收益区间必须由empty-poll和batch指标校准，不能用335秒累计时间直接除以wall time当作收益。

## 架构草案

```text
Pixels worker
  ├─ 构造 SpdkIoRequest
  ├─ push 到 poller submission ring
  ├─ 继续解码或等待 completion token
  └─ 从 completion ring取得结果

Poller core（每 controller/NUMA 少量）
  ├─ 独占一个或多个 qpair
  ├─ 批量 drain submission ring
  ├─ 批量提交 NVMe command
  ├─ process_completions(max_batch)
  └─ callback写 completion token/ring
```

### 所有权

- 一个qpair只由所属poller线程提交和处理completion；
- DMA buffer和operation在completion之前由poller/request对象持有；
- worker不能直接调用该qpair的 `process_completions()`；
- submission/completion ring按NUMA分配，尽量避免跨socket cache line传递。

### 等待策略

worker等待结果可配置为：

1. 继续处理已有batch，优先选择；
2. 短暂自旋后sleep/futex；
3. 纯自旋，仅用于低延迟对照。

如果worker继续自旋completion flag，只是把NVMe polling变成内存polling，不能算完成优化。

## 实施步骤

1. 暂不改架构，先增加 `Poll.Calls/EmptyCalls/Completions/MaxBatch/Cycles`；
2. 记录 controller、qpair、worker和NUMA node映射；
3. 实现一个controller一个poller的最小原型；
4. 将异步主数据接入，保留现有per-worker路径作为配置baseline；
5. 比较1、2、4个poller/controller，避免poller自身饱和；
6. 稳定后让异步metadata复用同一基础设施，并加入小请求公平调度；
7. 最后再做DMA buffer和operation pool复用。

## 验收指标

```text
empty_poll_ratio          = empty_calls / poll_calls
completions_per_poll      = completions / poll_calls
poll_cpu_per_completion   = poll_cycles / completions
submission_queue_delay
device_latency p50/p95/p99
worker_wait_time
poller_utilization
cross-NUMA request ratio
```

必须同时检查wall time、总task-clock和专用poller core数量。若wall只改善1%却永久占用多个额外core，应明确记录吞吐/成本权衡。
