# 格式无关 I/O 与内存管理器设计

## 1. 目标与非目标

目标是在 Pixels 与 Parquet 格式层之下建立统一的随机读取与内存管理层，使调用方只描述：

- 文件；
- 一批 `(fileOffset, length, destinationOffset)`；
- buffer 的容量、对齐与生命周期；
- 何时提交、等待和消费。

同一套上层协议可选择 `pread`、`io_uring` 或 SPDK。SPDK 使用 DMA-capable 内存作为 NVMe 的直接目标；格式层不接触 fd、ring、qpair、LBA、registered buffer index 或后端配置。

本设计不统一 Pixels 与 Parquet 的解码器，不把 Arrow 解码内存并入原始 I/O buffer 池，也不承诺让所有后端具有相同的并发实现。统一的是语义和生命周期，不是底层机制。

## 2. 当前状态

### 2.1 Pixels

当前热路径为：

```text
PixelsRecordReaderImpl
  -> RequestBatch
  -> Scheduler / NoopScheduler
  -> PhysicalLocalReader
  -> LocalFS::openRaf
  -> DirectRandomAccessFile | DirectUringRandomAccessFile* | DirectSpdkRandomAccessFile
```

后端选择和 buffer 选择分散在四处：

1. `LocalFS::openRaf()` 选择具体 RandomAccessFile；
2. `PixelsRecordReaderImpl::read()` 选择 `BufferPool`、`DynamicBufferPool`、`GlobalStaticBufferPool` 或 `SpdkBufferPool`；
3. `NoopScheduler::executeBatch()` 根据配置选择同步或异步路径；
4. `PhysicalLocalReader` 再次读取配置并 `static_pointer_cast` 到具体实现。

`PhysicalReader` 没有正式的异步接口；异步能力只存在于 `PhysicalLocalReader`。`Request.bufferId` 同时承担 io_uring fixed-buffer index 等后端私有含义。

### 2.2 Parquet

`read_parquet_uring()` 不经过 Pixels 物理层。每个 DuckDB worker 自行：

- 用 `posix_memalign` 分配 `raw_data[2][column]`；
- 建立和销毁 io_uring；
- 注册 fixed buffers；
- 用 `SubmitColumnReads()` 或 `SyncReadColumns()` 读列块；
- 在 `ParquetParallelStateNext()` 中维护 current/next 双缓冲状态。

完成后，`ParquetPixelsPreloadedFile` 将预读区域适配为 Arrow `RandomAccessFile`。Arrow 解压后数组、bitmap、dictionary 等仍由 Arrow `MemoryPool` 分配，这部分不属于原始 I/O 内存。

### 2.3 后端差异

| 后端 | 寻址 | 目标内存 | 提交/完成 | 特有资源 |
|---|---|---|---|---|
| pread | 文件 offset | 普通或对齐内存 | `submit()` 内同步完成 | fd |
| io_uring | 文件 offset | 普通、对齐或 fixed-registered 内存 | SQ submit + CQ wait | fd、ring、注册表 |
| SPDK | file offset 转 LBA extent | DMA-capable hugepage 内存 | 命令立即入队 + qpair poll | namespace、qpair、LBA map |

这些差异必须留在 backend/session 内部，不能继续以配置分支泄漏到格式层。

## 3. 目标分层

```text
PixelsFormatAdapter --------\
                              -> IoManager -> PrefetchPipeline
ParquetFormatAdapter -------/       |              |
                                    |              -> BufferLease / BufferView
                                    |
                                    +-> BufferProvider
                                    |     +-> AlignedAllocator
                                    |     +-> SpdkDmaAllocator
                                    |
                                    +-> IoBackend -> IoSession
                                          +-> PreadSession
                                          +-> IoUringSession
                                          +-> SpdkSession
```

职责边界：

- Format adapter：从格式元数据生成 read ranges，并把已完成的 views 交给解码器；
- `IoManager`：解析配置、协商 backend 能力、创建 session 与 pipeline；
- `BufferProvider`：按 requirements 分配、复用和回收内存；
- `IoSession`：拥有线程/worker 范围的后端资源并执行 batch；
- `PrefetchPipeline`：管理 current/next slot 和文件级 I/O/解码重叠；
- Decoder：只消费不可变 bytes。

## 4. 核心类型

以下接口是设计契约，不是要求逐字采用的最终 C++ API。

```cpp
enum class IoCapability : uint32_t {
    DirectIo          = 1U << 0,
    AsyncSubmit       = 1U << 1,
    FixedRegistration = 1U << 2,
    DmaCapable        = 1U << 3,
    RequiresPolling   = 1U << 4,
    Cancel            = 1U << 5,
};

struct BufferRequirements {
    uint64_t capacity;
    uint32_t alignment = 1;
    bool dmaCapable = false;
    bool fixedRegistrationPreferred = false;
    std::string debugName;
};

class BufferAllocation {
public:
    uint8_t* data() const noexcept;
    uint64_t capacity() const noexcept;
    uint32_t alignment() const noexcept;
    bool dmaCapable() const noexcept;

    // 销毁时调用 allocator 对应的 deleter。
    // 普通内存、posix_memalign 和 spdk_dma_malloc 不再由 ByteBuffer 猜测。
};

class BufferLease {
public:
    std::shared_ptr<BufferAllocation> allocation() const;
    uint64_t generation() const noexcept;
};

struct BufferView {
    std::shared_ptr<const BufferAllocation> owner;
    uint64_t offset;
    uint64_t size;

    const uint8_t* data() const noexcept {
        return owner->data() + offset;
    }
};
```

关键约束：

1. `BufferAllocation` 是底层内存的唯一所有权对象；
2. `BufferView` 必须携带 owner，不能像当前裸 `const uint8_t*` region 一样依赖外部隐式生命周期；
3. backend registration token 不放进公共 `BufferView`，由 session 使用 allocation identity 建立私有映射；
4. buffer 被未完成的 `IoBatch` 或 decoder view 持有时不能归还池或覆盖。

## 5. Read batch 与状态机

```cpp
using RequestId = uint64_t;

struct ReadRange {
    RequestId id;
    uint64_t fileOffset;
    uint32_t length;
    std::shared_ptr<BufferAllocation> target;
    uint64_t targetOffset;
};

struct ReadResult {
    RequestId id;
    uint32_t bytesRead;
    BufferView view;
};

enum class IoBatchState {
    Created,
    Submitted,
    Completing,
    Completed,
    Cancelling,
    Cancelled,
    Failed,
};

class IoBatch {
public:
    IoBatchState state() const noexcept;
    std::span<const ReadResult> results() const;
    std::exception_ptr error() const noexcept;
};

class IoSession {
public:
    virtual ~IoSession() = default;

    virtual IoBatch submit(FileHandle file,
                           std::span<const ReadRange> reads) = 0;
    virtual void wait(IoBatch& batch) = 0;
    virtual bool cancel(IoBatch& batch) = 0;
    virtual IoCapabilities capabilities() const noexcept = 0;
};
```

合法状态转换：

```text
Created -> Submitted -> Completing -> Completed
                    \-> Failed
Submitted -> Cancelling -> Cancelled
                         \-> Completed
```

语义要求：

- `submit()` 成功返回后，session 必须持有所有 target allocation；
- `wait()` 幂等：对 `Completed` 再调用不产生副作用；
- batch 失败时记录第一个错误，但必须排空或取消其余已提交请求后才释放 targets；
- short read 是显式错误，除非 request 标注允许 EOF；
- result 顺序由 `RequestId` 关联，不依赖 CQE 或 SPDK callback 的完成顺序；
- pread session 可以在 `submit()` 内完成全部读取并返回 `Completed`，调用方协议不变。

## 6. BufferProvider

```cpp
class BufferProvider {
public:
    virtual ~BufferProvider() = default;

    virtual BufferLease acquire(const BufferRequirements& requirements) = 0;
    virtual void trim() = 0;
    virtual BufferProviderStats stats() const = 0;
};
```

`IoManager` 根据 backend capabilities 修正 requirements：

| 场景 | allocator | alignment | 注册 |
|---|---|---:|---|
| buffered pread | ordinary/aligned | 1 或 SIMD 要求 | 无 |
| O_DIRECT pread | aligned | fs block size | 无 |
| io_uring non-fixed | aligned | direct I/O 时为 block size | 无 |
| io_uring fixed | aligned | block size | session 私有注册 |
| SPDK | `spdk_dma_malloc` | namespace block size | DMA 映射由 SPDK 环境管理 |

不采用“所有后端都分配 DMA 内存”的原因：

- hugepage 是稀缺的进程级资源；
- Arrow 解码内存不需要 DMA；
- pread/io_uring 使用 DMA 内存没有稳定收益；
- 会扩大 SPDK 初始化与运行环境对普通路径的影响。

因此采用能力驱动策略：只有 `SpdkSession` 要求 `dmaCapable=true`。

## 7. Backend 与 Session

### 7.1 Factory

```cpp
enum class IoBackendKind { Pread, IoUring, Spdk };

struct IoManagerOptions {
    IoBackendKind backend;
    bool directIo;
    bool fixedBuffers;
    bool doubleBuffer;
    uint32_t queueDepth;
    uint32_t blockSize;
};

class IoBackend {
public:
    virtual std::unique_ptr<IoSession>
    createSession(const IoSessionOptions& options) = 0;
};
```

配置只在 `IoManagerFactory` 解析一次。`PixelsRecordReaderImpl`、Parquet scan、scheduler 和 physical reader 不再读取 `localfs.async.lib`。

### 7.2 PreadSession

- 使用显式 `pread` offset，不依赖共享 seek position；
- 处理 EINTR 和 partial read，直至读满或确定 EOF；
- O_DIRECT 时计算 aligned start/end；
- 对未对齐 logical range，可直接读入具有 prefix 空间的 target，并返回偏移后的 view；
- 第一阶段为同步实现；未来需要线程池时不改变接口。

### 7.3 IoUringSession

- 每 DuckDB worker/session 独占一个 ring；
- session 构造和析构负责 queue init/exit；
- fixed buffer registration 以 allocation identity 为 key；
- `ReadRange` 的 `targetOffset` 映射为 SQE address，registered index 保留在私有表；
- user data 保存 request id，不依赖提交顺序；
- `wait()` 验证每个 CQE 的 `res == expectedAlignedLength`；
- queue depth 不足时分波提交，不能假设一次 batch 永远小于 SQ；
- fixed/non-fixed 是一个 backend 的策略，不再对应四个公开 RandomAccessFile 类。

### 7.4 SpdkSession

- 每 worker、每 controller 至多一个 qpair，由 session 所有；
- `FileHandle` 保存 immutable file-to-extents 映射与 namespace；
- logical read 先拆分为一个或多个 `(extent, lba, blocks)`；
- 跨 extent 的 read 必须拆成多个设备命令，不能假设全部数据位于 `extents[0]`；
- target 必须为 DMA-capable；不满足时在 submit 前失败，禁止静默 bounce copy；
- callback 只更新 request completion record；上层通过统一 `wait()` poll；
- aligned read 的 prefix 由结果 view 表达，NVMe 直接写入 DMA allocation；
- metadata 小读允许复用 DMA scratch buffer 后复制到小对象，但应单独计入 `metadata_copy_bytes`，不影响列块零拷贝承诺。

## 8. PrefetchPipeline

```cpp
enum class PipelineSlotState {
    Empty,
    Loading,
    Ready,
    Decoding,
};

struct PipelineSlot {
    PipelineSlotState state;
    FileToken file;
    std::vector<BufferLease> leases;
    std::optional<IoBatch> batch;
};

class PrefetchPipeline {
public:
    ReadyFile acquireFirst(FileReadPlan plan);
    std::optional<ReadyFile> advance(std::optional<FileReadPlan> next);
};
```

双缓冲不再体现为全局 `BufferPool::Switch()`。slot 的不变量是：

- `Empty -> Loading -> Ready -> Decoding -> Empty`；
- 只有 `Empty` slot 可以接收下一文件；
- `Loading` slot 的 batch 完成前不能进入 `Ready`；
- `Decoding` slot 的最后一个 `BufferView` 释放前不能回到 `Empty`；
- single-buffer 模式只创建一个 slot，物理上只分配一套 buffer；
- double-buffer 模式创建两个 slot，当前文件 decode 与下一文件 loading 可重叠。

这可消除“single 模式仍分配两份内存”和通过 curr/next 全局索引发生跨文件 alias 的风险。

## 9. Pixels 适配器

Pixels 保留格式逻辑：

1. 从 row-group footer 得到 column chunk offset/length；
2. 将每个 chunk 转成 `ReadRange`；
3. 根据列投影与 chunk 大小申请 leases；
4. 通过 pipeline 提交并等待；
5. 将 completed `BufferView` 适配为 `ByteBuffer`；
6. 原有 `ColumnReader::read()` 继续解码。

建议增加 owner-aware 的 ByteBuffer bridge：

```cpp
std::shared_ptr<ByteBuffer>
MakeByteBufferView(const BufferView& view);
```

该 bridge 必须在 ByteBuffer 内保存 owner，不能只构造不拥有底层内存的 `fromOtherBB` slice。迁移期可用包装对象同时保存 `BufferView` 与现有 `ByteBuffer`。

`RequestBatch` 可作为过渡输入，但最终应把 `bufferId` 与 `columnName` 从 I/O 契约中移除：

```cpp
struct LogicalReadRequest {
    RequestId id;
    uint64_t offset;
    uint32_t length;
    uint32_t logicalColumnId; // 仅用于 metrics/debug，不参与后端寻址
};
```

## 10. Parquet 适配器

Parquet 保留 footer 解析、row-group/column range 规划和 Arrow 解码：

1. Bind 阶段收集每个 row group 的 column offsets/sizes；
2. adapter 生成 `FileReadPlan`；
3. manager 将各 range 读入 leases；
4. `ParquetPixelsPreloadedFile` 由 `vector<Region>` 构造；
5. Arrow `FileReader` 使用预缓存 metadata，禁止自身 pre-buffer。

```cpp
struct PreloadedRegion {
    int64_t fileOffset;
    BufferView bytes;
};
```

`ParquetPixelsPreloadedFile` 保存 `BufferView`，而不是裸指针。其两种 `ReadAt` 语义：

- `ReadAt(pos, nbytes, out)`：复制到 Arrow 提供的输出；
- `ReadAt(pos, nbytes)`：若请求完全落在单一 region，返回持有 region owner 的 Arrow Buffer view；跨 region 时才分配并拼接。

Arrow decoder 的 `MemoryPool` 与 I/O `BufferProvider` 分离。后续可以注入统计型 Arrow pool，但不应把 decoder allocations 标记为 I/O buffer。

## 11. 错误模型与可观测性

统一错误至少包含：

```cpp
enum class IoErrorCode {
    InvalidArgument,
    OpenFailed,
    AlignmentViolation,
    BufferTooSmall,
    RegistrationFailed,
    SubmitFailed,
    ShortRead,
    CompletionFailed,
    CancelFailed,
    MappingMissing,
    ExtentOutOfRange,
    DeviceUnavailable,
};
```

错误上下文包含 backend、path/file token、logical offset/length、aligned offset/length、request id、系统错误码或 NVMe status。禁止只返回字符串或 assert。

统一 metrics：

- bytes requested / bytes physically read / read amplification；
- batches、requests、queue depth；
- submit、wait、poll、decode-overlap 时间；
- allocations、reuses、peak leased bytes；
- registered bytes；
- DMA bytes、metadata copy bytes；
- cancelled、failed、short reads。

所有指标按 backend、format、worker 汇总，但 format 只作为标签，不改变 backend 行为。

## 12. 配置映射

迁移期保持现有 properties：

| 旧配置 | 新选项 |
|---|---|
| `localfs.enable.async.io=false` | `backend=pread` |
| `localfs.async.lib=iouring` | `backend=iouring` |
| `localfs.async.lib=spdk` | `backend=spdk` |
| `localfs.enable.direct.io` | `directIo` |
| `localfs.iouring.use.fixed.buffer` | `fixedBuffers` |
| `pixels.doublebuffer` | `pipelineSlots=2`，否则 1 |
| `localfs.block.size` | fallback alignment；最终以设备探测值校验 |

`pixel.enable.globalStaticBytebuffer` 与 `pixels.enable.dynamic.buffer` 在兼容阶段映射为 buffer provider policy，完成迁移后废弃。冲突配置应在 factory 创建时一次性报错。

## 13. 迁移路线

### 阶段 0：契约与基线

- 建立 backend contract test；
- 固化 Pixels/Parquet 正确性与性能基线；
- 增加 RSS、requested/physical bytes、allocation 与 copy metrics；
- 不改变生产读取路径。

### 阶段 1：pread façade

- 实现 `BufferProvider`、`PreadSession`、`IoBatch`；
- 先接入独立 microbenchmark；
- 再让 Parquet `SyncReadColumns()` 通过 façade；
- 最后让 Pixels scheduler 的同步路径通过 façade。

退出条件：结果逐字节一致，pread 中位数不比基线慢 5%。

### 阶段 2：io_uring

- 实现 `IoUringSession` non-fixed；
- 加入 fixed registration policy；
- Parquet 删除自管 ring/raw_data；
- Pixels 删除格式层 ring 初始化和 registered index；
- 将 fixed/non-fixed/dynamic/static 变体收敛为 session 策略。

退出条件：ClickBench 正确，single/double 内存符合 slot 数，性能不比当前 io_uring 基线慢 5%。

### 阶段 3：统一 pipeline

- 以 `PrefetchPipeline` 替换 Pixels 与 Parquet 两套文件切换状态机；
- format adapter 只提供 `FileReadPlan` 和 decoder factory；
- 删除 `BufferPool::Switch()` / `ThreadContext::SwapRings()` 的格式层调用。

退出条件：I/O 与 decode overlap 可从统一 metrics 观测，且无跨文件 buffer alias。

### 阶段 4：SPDK DMA

- 实现 LBA mapping file handle 与 extent 拆分；
- `SpdkSession` 使用 `SpdkDmaAllocator`；
- Pixels 先接入，再接入 Parquet；
- 验证 async 列块路径 `copy_bytes=0`。

退出条件：三后端 contract test 一致，SPDK 无跨 extent 错读，hugepage 资源能完全释放。

### 阶段 5：删除旧实现

仅在上述阶段全部通过后：

- 删除 `PhysicalLocalReader` 的后端 casts；
- 废弃 `Request.bufferId`；
- 合并或删除四种 `DirectUringRandomAccessFile*`；
- 删除 `SpdkBufferPool` 与格式层 `raw_data`；
- 将 `PhysicalReader` 收敛为 metadata compatibility adapter，或完全迁移到 manager。

## 14. 验收标准

### 正确性

- 相同随机 ranges 在三后端逐字节一致；
- 覆盖未对齐读取、partial/EINTR、EOF、空 batch、queue depth 分波；
- SPDK 覆盖跨 extent、缺失 mapping、设备错误；
- Pixels reader 现有测试全部通过；
- Parquet ClickBench 44 查询结果与原生 `parquet_scan` 一致。

### 生命周期与并发

- ASan/LSan：无 UAF、double free、泄漏；
- TSan：session 与 pipeline 不发生 data race；
- decoder 持有 view 时 slot 不被覆盖；
- cancel/failure 后所有在途资源可回收；
- SPDK qpair、DMA allocation 和 io_uring registration 对称释放。

### 内存

- single-buffer 只分配一个 slot；
- double-buffer 只分配两个 slot；
- 峰值 leased bytes 可由 `workers × slots × projectedCapacity` 解释；
- 48 worker 全列扫描不因隐藏的第三份 raw buffer 导致 OOM；
- Arrow decoder allocations 单独统计。

### 性能

- 每种格式、每种后端至少 1 次 warm-up + 7 次有效重复；
- 报告 median、p95、MAD 和 95% CI；
- 统一层相对当前对应 backend 的 query-only median 不退化超过 5%；
- io_uring/SPDK 的 double-buffer 能在 I/O 密集查询中降低残余 wait；
- SPDK 列块热路径 `metadata_copy_bytes` 之外的 copy 为 0。

## 15. 主要风险

| 风险 | 后果 | 控制 |
|---|---|---|
| BufferView 不持有 owner | Arrow/Pixels 解码 UAF | owner-aware view，ASan 与 generation 检查 |
| SPDK 只使用首 extent | 跨 extent 静默错读 | plan 阶段拆 extent，contract test 造跨界 case |
| fixed registration 泄漏 | 内存无法回收或 ring 失效 | registration 由 session RAII 管理 |
| single 模式仍分配双份 | 高并发 RSS 无改善 | slot 数决定物理 allocation 数 |
| 抽象隐藏 read amplification | 性能和内存不可解释 | requested/physical bytes 强制指标 |
| Arrow `ReadAt` 再复制 | Parquet 热路径额外带宽 | 单 region 返回 owner-backed Arrow view |
| 全局 thread_local 隐式资源 | worker 复用时生命周期混乱 | session 显式由 local scan state 持有 |
| 一次迁移全部后端 | 回归无法归因 | pread、io_uring、pipeline、SPDK 分阶段门禁 |

## 16. 决策摘要

1. 统一语义，不强行统一后端实现；
2. 使用 `BufferAllocation + BufferLease + BufferView` 表达所有权；
3. 使用能力协商选择普通对齐内存或 SPDK DMA 内存；
4. 后端私有化 fd/ring/qpair/LBA/registration index；
5. 以显式 `IoBatch` 状态机统一同步与异步完成；
6. 双缓冲是通用 pipeline policy，不属于格式 reader；
7. Pixels 和 Parquet 只保留薄 adapter，解码器保持独立；
8. 先建立契约和 pread，再迁移 io_uring，最后迁移 SPDK；
9. 旧实现只在正确性、内存与性能门禁通过后删除。
