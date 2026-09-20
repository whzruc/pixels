# io_uring 三种工作模式：Interrupt、SQ Polling、CQ/I/O Polling

## 为什么做

当前 io_uring 使用普通提交和阻塞等待 CQE。它的优点是等待时让出 CPU，缺点是需要系统调用、睡眠和唤醒。在小延迟、高 IOPS 的 NVMe负载下，polling可能降低提交或完成延迟；但在 Pixels 这种“列解码占主导、I/O 可与计算重叠”的负载里，polling也可能只增加 CPU竞争。

本研究要回答的不是“polling是否更快”，而是三种模式在 single/doublebuffer、不同worker规模下的 latency、CPU和wall time权衡。

## 三种模式

### 1. Interrupt/阻塞等待基线

```text
io_uring_submit()
io_uring_wait_cqe_nr()
未完成时线程睡眠，由设备中断/内核完成路径唤醒
```

当前实现属于这一类。它节省 CPU，但会产生 syscall、scheduler和context-switch成本。

### 2. SQ Polling（`IORING_SETUP_SQPOLL`）

内核创建 SQ poll线程，持续观察 submission queue。用户线程准备好 SQE后，通常可以减少进入内核提交的次数；SQ poll线程睡眠后仍需通过 wakeup恢复。

SQPOLL优化的是“提交路径”，不自动消除 CQE等待，也不等于 completion polling。需要处理 ring权限、固定文件/注册资源、`IORING_SQ_NEED_WAKEUP` 和 poll线程CPU绑定。

### 3. CQ/I/O Polling（`IORING_SETUP_IOPOLL`）

用户或内核路径主动轮询存储完成，目标是降低完成延迟。它通常要求支持poll的块设备、direct I/O以及一致的I/O类型，且会持续消耗 CPU。实现时应使用 liburing支持的 peek/enter/poll方式，不能用无节制的空循环替代。

SQPOLL与IOPOLL作用在不同阶段，技术上可以形成组合实验；第一轮应分别隔离变量，避免无法归因。

## 现有证据

io_uring 48线程 singlebuffer：

```text
AsyncSubmit.Total       84.10 thread-s
AsyncComplete.Total    276.07 thread-s
AsyncComplete.WaitCQE  266.06 thread-s
ProcessCQE               3.28 thread-s
off-CPU io_cqring_wait 119.54 s
context switches       约133.4万
```

doublebuffer：

```text
AsyncSubmit.Total       89.73 thread-s
AsyncComplete.Total     14.71 thread-s
AsyncComplete.WaitCQE    4.64 thread-s
context switches       约9.8万
```

这说明 completion等待是 singlebuffer的重要成本，但 doublebuffer已经通过重叠消除了绝大部分等待。SQPOLL可能降低提交开销；IOPOLL最可能帮助singlebuffer的尾延迟，但也会与字符串解码争抢 CPU。

## 预期效果

- Interrupt是CPU效率基线，预期仍最适合doublebuffer和高解码压力；
- SQPOLL若能显著减少 submit syscall，候选目标为降低 I/O control CPU 10%～40%，对应查询 wall收益预计较小，保守目标0%～3%；
- IOPOLL可能降低singlebuffer completion latency和context switches，但48线程wall收益的合理实验目标约0%～8%，并可能因CPU/cache竞争出现负收益；
- doublebuffer的 `WaitCQE` 只剩4.64 thread-s，IOPOLL的wall收益上限很低，重点应是延迟稳定性而非平均wall time。

这些区间是立项门槛，不是现有数据能够保证的收益。

## 实现草案

1. 增加配置 `localfs.iouring.mode=interrupt|sqpoll|iopoll`；
2. 建立ring时按模式设置 `io_uring_params.flags`；
3. SQPOLL增加 idle timeout、poll CPU和NUMA绑定配置，并正确处理 `IORING_SQ_NEED_WAKEUP`；
4. IOPOLL启动前验证direct I/O、文件系统和块设备支持，不支持时明确失败而不是静默fallback；
5. 把 ring setup、submit syscall、SQ wakeup、CQ empty poll、CQ completion数量分别计数；
6. 每个worker独立ring与少量共享ring作为两个后续变量，第一轮保持现有ring所有权不变；
7. 确保异常和query结束时可靠退出poll线程并释放registered buffer。

新增指标建议：

```text
Uring.Submit.Calls
Uring.Submit.Syscalls
Uring.SQPoll.Wakeups
Uring.CQPoll.Calls
Uring.CQPoll.EmptyCalls
Uring.CQPoll.Completions
Uring.RequestLatency histogram/p50/p95/p99
```

## 验证设计

先固定 q24、24 SSD、48线程，对single/doublebuffer分别测试三种模式，每种重复三次；确认方向后再扩展12/24线程。除wall time外必须报告task-clock和poll core CPU，否则IOPOLL通过多烧CPU得到的小幅收益会被误判为优化。
