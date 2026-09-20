# Pixels C++ 分阶段提交方案（历史归档）

> 状态（2026-09-08）：本文保存前期拆分依据，不再作为实际执行清单。
> 当前正式分支为 `feature/cpp-flatbuffers` →
> `feature/cpp-runtime-profiler` → `feature/cpp-buffer-pool` →
> `feature/cpp-io-uring-backends` → `feature/cpp-footer-cache` →
> `feature/cpp-parallel-scan` → `feature/cpp-scan-performance` →
> `feature/cpp-spdk` → `feature/cpp-parquet`。以
> [`../io/git-pr-path.zh-CN.md`](../io/git-pr-path.zh-CN.md) 为准。

本文档根据当前 `feature/UpdateBufferPool` 分支、工作区改动和本地保存的目标仓库基线整理，作为后续拆分提交和发起 PR 的参考。

## 一、当前工作区概况

- 当前分支：`feature/UpdateBufferPool`
- 当前 HEAD：`ef6ae3f1 dynamic bufferpool implementation`
- 已存在的自有功能提交：
  - `159e29c0`：double/single buffer
  - `c681909b`：FlatBuffers
  - `ef6ae3f1`：dynamic buffer pool
- 当前未提交变化：
  - 72 个已跟踪文件发生变化
  - 589 个未跟踪文件
  - `testcase/performance-test` 约 1.3 GB
  - 包含大量 perf、火焰图、实验 JSON、缓存和生成结果
- 本地 `whzruc/master` 指向 `25151923`，明显落后于当前分支所包含的上游提交。由于分析时环境无法连接 GitHub，尚未核实远端最新指针。

这些变化不是单一的 BufferPool 更新，而是同时包含：

- 全局、静态和动态 BufferPool
- io_uring 多种读取后端
- SPDK 后端
- ColumnVector 内存复用
- DuckDB Pixels 扫描集成
- Arrow/Parquet 扫描
- CPU affinity
- profiling 和性能实验工具

因此不适合一次性提交。

## 二、提交前必须处理的问题

当前暂存区存在不一致状态：

- `PhysicalLocalReader.cpp` 已暂存为删除，但同路径又有未跟踪的新文件
- `NoopScheduler.cpp` 同样是“暂存删除 + 未跟踪重建”
- `.gitmodules` 同时有已暂存和未暂存修改
- `FlameGraph` 已暂存为移动到 `third-party/FlameGraph`
- `pixels-duckdb/duckdb` 子模块内部还有未提交修改

正式整理前，建议先取消这些文件的暂存，但保留工作区内容：

```bash
git restore --staged -- \
  .gitmodules \
  cpp/pixels-common/lib/physical/io/PhysicalLocalReader.cpp \
  cpp/pixels-common/lib/physical/scheduler/NoopScheduler.cpp \
  cpp/FlameGraph \
  cpp/third-party/FlameGraph
```

不要使用：

```bash
git add .
git add -A
```

以下内容默认不应提交：

- `Untitled`
- `__pycache__/`
- `*.pyc`
- `*.perf.data`
- `*.perf.data.raw`
- `*.collapsed`
- 火焰图生成产物
- `.complete`
- 大批 `comparison-results/`
- 本机测试日志、CSV/JSON 实验结果
- `../libs/`
- `../pixels-common/include/pixels_generated.h`
- 未明确来源的整个 `third-party/arrow/`

建议先补充 `.gitignore`，排除这些生成物。

## 三、推荐的分阶段提交方案

### 阶段 1：完善配置和性能分析基础设施

建议提交信息：

```text
feat(cpp): add configurable profiling and column size utilities
```

包含：

- `pixels-common/include/profiler/ProfilerSwitch.h`
- `pixels-common/lib/profiler/ProfilerSwitch.cpp`
- `pixels-common/include/profiler/AbstractProfiler.h`
- `pixels-common/include/profiler/TimeProfiler.h`
- `pixels-common/lib/profiler/TimeProfiler.cpp`
- `pixels-common/lib/profiler/CountProfiler.cpp`
- `pixels-common/include/utils/ColumnSizeCSVReader.h`
- `pixels-common/include/utils/ColumnSizeCSVWriter.h`
- `pixels-common/lib/utils/ColumnSizeCSVWriter.cpp`
- `pixels-common/include/utils/ConfigFactory.h`
- `pixels-common/lib/utils/ConfigFactory.cpp`
- 对应的 `pixels-common/CMakeLists.txt` 修改
- `pixels-cpp.properties` 中仅与 profiler、column-size 有关的配置
- `tests/column-size-analyzer/` 中的源代码和必要测试输入

不要在此提交性能结果文件。

### 阶段 2：实现全局 ByteBuffer 池和动态缓冲管理

建议提交信息：

```text
feat(cpp): introduce reusable global byte buffer pools
```

包含：

- `GlobalByteBufferPool.h/.cpp`
- `DynamicBufferPool.h/.cpp`
- `BufferPool.h/.cpp`
- `ByteBuffer.h/.cpp`
- `global-bufferpool.properties`
- `pixels-cpp.properties` 中的以下配置：
  - `pixels.enable.dynamic.buffer`
  - 全局池相关参数
  - 内存统计开关
- `docs/buffer-pool/global-buffer-pool.md`

这一提交只建立内存池抽象、分配、释放、扩容和统计能力，不同时接入 DuckDB 扫描流程。

### 阶段 3：实现全局静态缓冲池和线程上下文

建议提交信息：

```text
feat(cpp): add thread-aware global static buffer pool
```

包含：

- `GlobalStaticBufferPool.h/.cpp`
- `ThreadContext.h/.cpp`
- `BufferPool` 中静态双缓冲索引和固定 buffer ID 支持
- `PixelsReadGlobalState.hpp`
- `PixelsReadLocalState.hpp` 中的线程上下文字段
- `pixels-cpp.properties` 中的以下配置：
  - `pixel.enable.globalStaticBytebuffer`
  - `pixels.static.buffer.hugepage`
- `docs/buffer-pool/global-static-buffer-pool.md`

这一阶段依赖阶段 2。

### 阶段 4：重构本地异步读取和 io_uring 后端

建议提交信息：

```text
feat(cpp): support fixed, non-fixed and dynamic io_uring readers
```

包含：

- `PhysicalReader.h`
- `PhysicalLocalReader.h/.cpp`
- `Request.h/.cpp`
- `RequestBatch.h/.cpp`
- `NoopScheduler.cpp`
- `DirectRandomAccessFile.cpp`
- `DirectUringRandomAccessFile.h/.cpp`
- `DirectUringRandomAccessFileDynamic.cpp`
- `DirectUringRandomAccessFileNonFixed.h/.cpp`
- `DirectUringRandomAccessFileStatic.h/.cpp`
- `LocalFS.cpp`
- 对应的 CMake 修改
- `docs/io/unified-io-memory-manager-design.md`

重点验证：

- synchronous `pread`
- 普通 io_uring
- fixed-buffer io_uring
- non-fixed io_uring
- dynamic buffer pool
- single/double buffer

应确保 `PhysicalLocalReader.cpp` 和 `NoopScheduler.cpp` 最终表现为“修改”，而不是删除。

### 阶段 5：增加 ColumnVector 内存复用

建议提交信息：

```text
perf(cpp): reuse column vector backing buffers
```

包含：

- `ColumnVectorBufferPool.h/.cpp`
- `ColumnVector.h/.cpp`
- `BinaryColumnVector.h/.cpp`
- `DateColumnVector.cpp`
- `DecimalColumnVector.cpp`
- `IntColumnVector.cpp`
- `LongColumnVector.cpp`
- `TimestampColumnVector.cpp`
- 各 `ColumnReader.cpp` 中与复用直接相关的变化
- `PixelsRecordReaderImpl.h/.cpp` 中仅与 RowBatch/ColumnVector 复用相关的部分
- `pixels-core/CMakeLists.txt`
- `pixels-cpp.properties` 中的以下配置：
  - `pixels.columnvector.pool`
  - malloc 调优参数
- `docs/features/column-vector-reuse.md`

`PixelsRecordReaderImpl.cpp` 同时含有 BufferPool、SPDK、profiling 等多类修改，建议使用交互式分块暂存：

```bash
git add -p cpp/pixels-core/lib/reader/PixelsRecordReaderImpl.cpp
```

### 阶段 6：集成 Pixels DuckDB 扫描及 CPU affinity

建议提交信息：

```text
feat(duckdb): integrate buffer pools into parallel Pixels scans
```

包含：

- `PixelsScanFunction.cpp` 中 Pixels 格式扫描部分
- `pixels_extension.cpp` 中 Pixels 扫描注册和初始化部分
- `CPUAffinity.h`
- `PixelsReadGlobalState.hpp`
- `PixelsReadLocalState.hpp` 中扫描状态部分
- `PixelsFooterCache.h/.cpp`
- `PixelsReaderBuilder.cpp`
- `PixelsBitMask.cpp`
- `PixelsFilter.cpp`
- `TypeDescription.h/.cpp`
- `docs/features/cpu-affinity.md`
- 以下配置项：
  - `pixels.enable.cpu.affinity`
  - `pixels.cpu.affinity.strategy`
  - `pixels.cpu.affinity.core.mapping`

`PixelsScanFunction.cpp` 单文件约有 1400 行变化，是整个系列风险最高的文件。建议进一步按以下内容分块暂存：

- BufferPool 生命周期
- async prefetch/double buffer
- CPU affinity
- profiler 输出

如果无法通过 `git add -p` 清晰拆开，应先整理源码再提交。

### 阶段 7：独立增加 Parquet + Arrow 扫描路径

建议提交信息：

```text
feat(duckdb): add Arrow-based Parquet scan with io_uring
```

包含：

- `ArrowRandomAccessFile.hpp/.cpp`
- `ParquetPixelsScan.hpp/.cpp`
- `pixels_extension.cpp` 中 `read_parquet_uring` 注册部分
- `CMakeLists.txt` 中 Arrow/Parquet 依赖
- `Makefile` 中对应构建目标
- `docs/io/parquet-uring.md`
- `tests/parquet-reader-performance-test/` 中测试源码和脚本

当前实现已经选择正式 Git submodule：`cpp/third-party/arrow` 固定为
Apache Arrow 18.0.0（`9105a410`），并由 CMake 在项目内构建 Arrow/Parquet。
不得回退为系统 Arrow/Parquet，也不要提交 submodule 工作区中的本地修改。

`pixels-duckdb/duckdb` 子模块的修改也必须单独确认。如果需要更新，应先在 DuckDB 仓库形成明确提交，再在父仓库提交子模块指针。

### 阶段 8：独立增加可选 SPDK 后端

建议提交信息：

```text
feat(cpp): add optional SPDK asynchronous storage backend
```

包含：

- `SpdkBufferPool.h/.cpp`
- `DirectSpdkRandomAccessFile.h/.cpp`
- `ByteBuffer` 中的 `BY_SPDK_DMA` 支持
- `LocalFS.cpp` 中的 SPDK 分支
- `PixelsRecordReaderImpl.cpp` 中的 SPDK 路径
- `PixelsScanFunction.cpp` 中 SPDK 初始化、切换和释放逻辑
- CMake 中的 `PIXELS_ENABLE_SPDK`
- `testcase/spdk/` 中可复现的脚本
- `docs/io/spdk.md`

SPDK 应默认关闭，确保未安装 SPDK 的普通环境仍然可以编译：

```text
PIXELS_ENABLE_SPDK=OFF
```

该阶段最好形成独立 PR，因为它有额外的系统依赖、DMA 内存语义和硬件验证要求。

## 四、测试和文档安排

测试代码应放进对应功能提交，不建议最后集中形成一个笼统的 tests 提交：

- BufferPool 单元测试：阶段 2 或阶段 3
- io_uring 测试：阶段 4
- ColumnVector 测试：阶段 5
- DuckDB Pixels 扫描测试：阶段 6
- Parquet 测试：阶段 7
- SPDK 脚本：阶段 8

可以在最后增加一个纯文档提交：

```text
docs(cpp): document buffer pool and asynchronous I/O architecture
```

其中只放总览、索引和不隶属于单一功能的设计说明。

## 五、不建议直接纳入本次提交的内容

以下变化看起来属于实验目录重组或本机产物，需要逐项确认：

- 删除 `testcase/buffersize-tests/`
- 删除旧的 `run_perf.py`、`process_sqls.py`
- 删除 `single_doublebuffer_async_sync_test.py`
- 删除 `generate_flamegraphs.sh`
- `FlameGraph` 子模块移动
- `.gitmodules` 修改
- `testcase/benchmark.json` 中的本机路径
- `testcase/clickbench-gen/clickbench-gen.sh`
- `tests/writer/PixelsWriterTest.cpp` 仅增加一行的变化
- `README.md` 中环境特定说明
- 超过 1 GB 的性能采样结果

如果确实需要重组测试框架，应独立提交：

```text
refactor(cpp): reorganize performance testing tools
```

该提交只包含脚本移动和文档更新，不包含采样产物。

## 六、推荐的 PR 划分

相比一个超大 PR，建议形成三个 PR：

1. **BufferPool 与 io_uring 基础设施**
   - 对应阶段 1～5
2. **DuckDB Pixels/Parquet 扫描优化**
   - 对应阶段 6～7
3. **SPDK 后端**
   - 对应阶段 8

每个 PR 都应从目标仓库最新的 `master` 创建分支。

当前已有的 FlatBuffers 提交与这批 C++ I/O 优化关联较弱。如果目标 `master` 尚未包含它，建议将其保留在独立 PR 中，不要混入 BufferPool PR。

## 七、整体依赖顺序

```text
配置/Profiler
    ↓
全局 ByteBuffer 池
    ↓
静态池与线程上下文
    ↓
io_uring 后端
    ↓
ColumnVector 复用
    ↓
DuckDB Pixels 集成
    ├── Parquet/Arrow
    └── SPDK
```

该顺序有利于代码审查、问题定位、单独回滚和性能收益归因。
