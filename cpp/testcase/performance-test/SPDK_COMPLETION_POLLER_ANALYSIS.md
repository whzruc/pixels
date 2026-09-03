# SPDK completion polling静态分析与实验计划

## 1. 静态结论

`Spdk.SyncRead.Poll`不是异步列chunk读取自动退化出的路径。当前代码有两条明确且并存的I/O路径：

- 列chunk：`PixelsRecordReaderImpl::read()`提供`SpdkBufferPool` DMA buffer，经`NoopScheduler`调用`readAsync()`，最后由`readAsyncComplete()`轮询CQ。
- 元数据：没有reuse buffer，经`NoopScheduler`同步分支或`PixelsReaderBuilder`直接调用`readFully/readLong`，最终进入`DirectSpdkRandomAccessFile::syncRead()`并原地busy poll。

同步元数据读取的具体来源：

1. `PixelsReaderBuilder::build()`在file-tail cache miss时读取8字节tail offset，再读取完整file tail；两次都调用`syncRead()`。
2. `PixelsRecordReaderImpl::prepareRead()`在row-group-footer cache miss时构造不带reuse buffer的`RequestBatch`。`NoopScheduler::executeBatch()`因此选择同步`readFully()`，每个footer进入一次`syncRead()`。
3. double buffer只提前执行下一文件的列chunk `read()`；下一文件的builder构造、file tail和row-group footer仍由worker同步完成。因此`Spdk.SyncRead.Poll`在double-buffer模式下出现是设计现状，不是异常回退。

每次`syncRead()`还会执行`spdk_dma_malloc → NVMe submit → busy poll → memcpy → spdk_dma_free`。对8字节tail offset也会按4 KiB对齐读取并分配bounce buffer，高文件数和高并发会放大固定开销。

## 2. 当前qpair与poll模型

`DirectSpdkRandomAccessFile::tls_qpairs`为每个worker线程、每个NVMe controller分配一个qpair。文件对象只保存指向该thread-local qpair的非拥有指针。同步和异步完成都由发起请求的worker调用`spdk_nvme_qpair_process_completions()`。

不能只创建一个共享线程去轮询现有worker qpair：SPDK qpair要求单一执行上下文拥有，提交和completion处理不应由多个线程并发操作。安全的共享poller需要同时迁移提交与完成处理。

## 3. 共享completion方案

建议先实现可配置的实验路径，保留现有模式作为基线：

```text
DuckDB workers
    │ MPSC submission queue（每个请求含controller、LBA、buffer、completion token）
    ▼
固定poller线程（建议每NUMA节点或每组controller 1个）
    ├─ 独占其controller qpairs
    ├─ 批量drain submission queue
    ├─ 批量提交NVMe命令
    └─ spdk_nvme_qpair_process_completions(qpair, batch_limit)
          │
          ▼
completion token（atomic状态/condition variable）→ worker
```

第一版建议配置：

- `localfs.spdk.poller.mode=worker|shared`，默认`worker`；
- `localfs.spdk.poller.count=1|2|4`；
- controller按PCI地址稳定散列到poller；
- 每个poller为归属controller独占一个qpair；
- submission queue有界，满时记录backpressure并由worker短暂协助或等待；
- completion callback只写token，不在poller执行解码；
- 先保留现有`SpdkBufferPool`，避免同时改变DMA内存与polling两个变量。

共享poller验证后，再做DMA优化：为file-tail offset/file tail/row-group footer建立可复用的thread-local或poller-owned小型DMA slab，消除每次`spdk_dma_malloc/free`。

## 4. 已增加的观测点

为区分同步poll来源，代码新增三个内部profiler标签：

- `Pixels.Metadata.FileTailOffsetRead`
- `Pixels.Metadata.FileTailRead`
- `Pixels.Metadata.RowGroupFooterRead`

它们与`Spdk.SyncRead.*`是包含关系，不能相加；用途是判断同步poll主要由哪类元数据触发。实验还应增加调用次数、读取字节数和cache hit/miss计数，避免只凭累计时间推断请求数量。

## 5. 实验分阶段

### A. 同步poll归因实验

只比较现有`spdk`与`spdk-doublebuffer`，运行q24、12/24/48线程，开启内部profiler、perf stat和火焰图。目标是量化三类元数据耗时与`Spdk.SyncRead.Poll`的对应关系，并确认cache miss数量。

### B. shared-poller A/B实验

实现shared模式后，在完全相同环境下比较：

- worker poller（现状）；
- shared poller count=1；
- shared poller count=2；
- shared poller count=4。

主要指标：`Total Time`、SPDK IOPS/MiB/s、worker CPU、poller CPU、IPC、cache miss、上下文切换、submission queue深度、backpressure次数、完成批大小、p50/p95/p99请求延迟。

正确性门槛：每个配置必须查询成功、结果一致、`submitted=completed`、`errors=0`，且ASan或生命周期检查不得发现completion token/DMA buffer提前释放。

## 6. 可复用测试入口

归因实验可以直接复用`run_all_comparison.sh`，仅运行SPDK阶段：

```bash
sudo -E env \
  COMPARISON_QUERIES=q24 \
  SSD_MODES=24ssd \
  THREAD_LIST="12 24 48" \
  ENABLE_PIXELS_PROFILER=1 \
  ENABLE_PERF_PROFILING=1 \
  KEEP_PERF_DATA=1 \
  SKIP_PIXELS=1 \
  SKIP_PARQUET=1 \
  SKIP_ANALYSIS=1 \
  DISABLE_EMAIL=1 \
  RUN_STAMP="$(date +%Y%m%d_%H%M%S)-spdk-sync-attribution" \
  bash testcase/performance-test/run_all_comparison.sh
```

shared-poller实现后使用同一入口，只需分别设置poller mode/count；具体环境变量会在实现时接入测试脚本。
