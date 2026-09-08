# Pixels Metadata 异步化与跨文件 Pipeline

## 为什么做

Pixels 在读取列数据前有严格依赖：

```text
FileTailOffset → FileTail → RowGroupFooter → Column chunks
```

当前 FileTailOffset、FileTail和RowGroupFooter在worker文件切换路径上逐文件同步读取。io_uring构建实际走同步 `pread64`；SPDK构建通过 `syncRead` 提交后busy-poll。主数据已经批量异步化，但metadata仍会在高并发文件边界形成大量小I/O和同步等待。

单请求“提交后立即等待”没有队列深度。真正目标应是跨多个文件分阶段批量推进metadata。

## 现有证据

### io_uring 最新结果

| 模式 | 12线程 | 24线程 | 48线程 |
|---|---:|---:|---:|
| single metadata累计时间 | 7.57 s | 10.41 s | 43.91 s |
| double metadata累计时间 | 5.99 s | 6.17 s | 12.16 s |

48线程 singlebuffer中 TailOffset为14.52秒、FileTail为5.72秒、RowGroupFooter为23.67秒。metadata/wall约90%，表示平均约0.9个worker等价时间处在同步metadata调用中，并不表示wall可直接减少43.91秒。

### SPDK 旧结果

| 模式 | 12线程 | 24线程 | 48线程 |
|---|---:|---:|---:|
| single metadata累计时间 | 13.90 s | 48.15 s | 141.36 s |
| double metadata累计时间 | 9.36 s | 26.27 s | 146.84 s |

48线程 `Spdk.SyncRead.Poll` 约96秒，且metadata合计与 `SyncRead.Total` 基本相等，证明几乎全部同步SPDK I/O来自这三类metadata。

当前调度器通常把一个文件交给一个worker，证据不支持“大量worker重复读取同一个文件metadata”。确定存在的小规模重复是bind读取的首文件footer cache没有直接传递给scan生命周期。

## 预期效果

### io_uring

异步化主要减少文件边界stall和pread wakeup。48线程保守wall目标为1%～5%，高文件数、小footer或更高metadata请求密度下可能更高。doublebuffer当前metadata只有12.16 thread-s，平均wall收益上限更低。

### SPDK

首要目标是消除约96秒同步metadata busy-poll，并将小请求交给批量completion基础设施。48线程wall改善的实验目标约3%～10%，同时应显著降低task-clock。该区间不是把96秒按线程数机械折算的保证。

### 其他收益

- 平滑文件边界小I/O突发；
- 合并submit和completion处理；
- 为query级footer cache和跨query cache建立统一入口；
- 减少DMA buffer分配、复制和释放；
- 可以控制metadata小请求与列数据大请求的公平性。

## Pipeline 草案

```text
Metadata coordinator
  │
  ├─ Stage A：批量读取窗口内文件的最后8字节
  │             ↓
  │           解析 FileTailOffset
  │             ↓
  ├─ Stage B：批量读取 FileTail
  │             ↓
  │           解析 schema / row-group位置
  │             ↓
  ├─ Stage C：批量读取目标 RowGroupFooter
  │             ↓
  │           构造 PreparedPixelsReader
  │
  └─ ready queue → scan workers → column data pipeline
```

预取窗口同时限制文件数、请求数和metadata bytes，避免一次性占用过多registered/DMA memory。

## 数据结构草案

```cpp
enum class MetadataState {
    NeedTailOffset,
    TailOffsetInFlight,
    NeedFileTail,
    FileTailInFlight,
    NeedRowGroupFooter,
    RowGroupFooterInFlight,
    Ready,
    Failed
};

struct MetadataTask {
    FileId file_id;
    MetadataState state;
    MetadataBuffers buffers;
    ParsedFileTail file_tail;
    std::vector<RowGroupFooter> row_groups;
    CompletionToken completion;
};
```

缓存key至少包含规范化path、文件版本标识（size/mtime或对象版本）、row-group id和读取schema/投影相关信息。并发miss应使用single-flight/future，避免同一key重复发请求。

## 后端实现

### io_uring

- positional read和明确offset；
- direct I/O对齐；
- metadata registered buffer size class；
- 一次准备多个SQE并批量submit；
- 批量CQE处理和请求级错误传播；
- completion前保持ring、reader和buffer生命周期。

### SPDK

- metadata buffer必须DMA-capable；
- buffer和operation按size class复用；
- 请求路由到拥有controller/qpair的固定poller；
- 批量收割completion；
- metadata小请求和主数据请求采用配额或加权公平调度。

## 实施顺序

1. 增加metadata request count、bytes、cache hit/miss、latency直方图；
2. 传递bind footer cache，消除已知首文件重复；
3. 先异步批量读取地址已知的RowGroupFooter；
4. 将 `PixelsReaderBuilder` 拆成可分阶段推进的状态机；
5. 实现跨文件TailOffset/FileTail窗口；
6. 接入query级single-flight cache；
7. io_uring接registered pool，SPDK接固定poller和DMA pool；
8. 加入同步fallback和故障注入测试。

## 验收

- 结果正确，corrupt file和短读错误可定位到具体文件/阶段；
- metadata请求数和字节数符合预期，无新增重复；
- SPDK `SyncRead.Poll` 接近零或只存在于fallback；
- io_uring同步pread off-CPU明显下降；
- metadata per-file latency及p95/p99下降；
- wall time、task-clock和cache misses均不恶化；
- 在不同窗口大小下报告吞吐、内存占用与尾延迟。
