# Pixels C++ Git 历史整理 TODO（历史归档）

> 状态（2026-09-08）：本文记录整理前的检查过程，其中旧分支名和待办命令
> 仅供追溯，不应再次执行。当前正式线性分支及 commit 对照见
> [`../io/git-pr-path.zh-CN.md`](../io/git-pr-path.zh-CN.md)。BufferPool WIP
> 已并入 `feature/cpp-buffer-pool`，SPDK 与 Parquet 已改为严格的前后关系，
> Parquet 使用固定在 `cpp/third-party/arrow` 的 Arrow 18.0.0。

本文档用于彻底重建当前 Pixels C++ 本地提交历史：保留现有代码成果，以最新 `origin/master` 为基线，重新制作可独立审查的功能提交，随后分别用于不同 Issue/PR。

网络相关操作暂不展开。Issue 创建、远端推送、PR 创建和 CI 检查只保留占位章节。

## 0. 当前历史和正确基线

当前旧提交链：

```text
2a109658 [Issue #1211] rename PixelsWriterBuffer to PixelsWriteBuffer (#1228)
└── 159e29c0 add testcase and add double/single buffer switch support
    └── c681909b replace protobuf with flatbuffers for serialization
        └── ef6ae3f1 dynamic bufferpool implementation
            └── 6a0995d5 chore(cpp): ignore generated build and profiling artifacts
```

已知最新远端基线：

```text
b99eef2b [Issue #1387] rename flatc osx-aarch64 directory to osx-aarch_64 (#1388)
```

`2a109658` 已经包含在 `b99eef2b` 的历史中。因此新的提交必须基于最新 `origin/master`，不能把 master 回退到 `2a109658`。

- [ ] 保留 `2a109658` 之后的全部上游提交。
- [ ] 丢弃 4 个本地提交的提交历史，但保留其代码成果。
- [ ] 旧提交只用于重新应用代码，不能直接进入最终 PR。
- [ ] 不使用 `git add .`、`git add -A`、`git add --all`。
- [ ] 混合文件使用 `git add -p`。
- [ ] 新文件需要分块时先执行 `git add -N <file>`。
- [ ] 每个功能提交必须同时包含对应代码、配置、构建修改、测试和文档。

## 1. 保存当前完整现场

当前工作区有大量未提交修改，且 DuckDB 子模块内部不干净。硬重置前必须完成全部备份。

### 1.1 保存工作区补丁

```bash
cd /home/whz/test/pixels/cpp

git branch --show-current
git status --short --untracked-files=all
git log --oneline --decorate -8

git diff --binary > /tmp/pixels-working-tree-20260825.patch
git diff --cached --binary > /tmp/pixels-index-20260825.patch
git status --short --untracked-files=all > /tmp/pixels-status-20260825.txt
git ls-files --others --exclude-standard > /tmp/pixels-untracked-20260825.txt
git submodule status > /tmp/pixels-submodules-20260825.txt
```

- [ ] 确认上述 `/tmp` 文件存在且不是空的意外结果。

### 1.2 保存 DuckDB 子模块状态

```bash
git -C pixels-duckdb/duckdb status --short
git -C pixels-duckdb/duckdb diff --binary > /tmp/pixels-duckdb-20260825.patch
git -C pixels-duckdb/duckdb branch backup/pixels-cpp-rebuild-20260825
```

- [ ] 如果子模块有未跟踪源码，单独复制或在子模块备份分支提交。
- [ ] 父仓库提交不能保存子模块内部未提交文件。

### 1.3 记录 Arrow 和 FlameGraph

```bash
git -C third-party/arrow status --short
git -C third-party/arrow remote -v
git -C third-party/arrow rev-parse HEAD

git -C third-party/FlameGraph status --short
git -C third-party/FlameGraph remote -v
git -C third-party/FlameGraph rev-parse HEAD
```

- [ ] 保存 URL、提交号和状态。
- [ ] 不把嵌套 Git 仓库当普通目录提交。

### 1.4 创建完整本地快照分支

```bash
git switch -c backup/full-cpp-workspace-20260825
git diff --name-status
```

逐项确认后保存已跟踪变化：

```bash
git add -- \
  ../.gitmodules \
  .gitignore \
  CMakeLists.txt \
  Makefile \
  README.md \
  include \
  pixels-common \
  pixels-core \
  pixels-cpp.properties \
  pixels-duckdb/PixelsScanFunction.cpp \
  pixels-duckdb/pixels_extension.cpp \
  testcase \
  tests/writer/PixelsWriterTest.cpp
```

保存新增源码：

```bash
git add -- \
  global-bufferpool.properties \
  pixels-common/include/physical/GlobalByteBufferPool.h \
  pixels-common/include/physical/GlobalStaticBufferPool.h \
  pixels-common/include/physical/SpdkBufferPool.h \
  pixels-common/include/physical/ThreadContext.h \
  pixels-common/include/physical/natives/DirectSpdkRandomAccessFile.h \
  pixels-common/include/physical/natives/DirectUringRandomAccessFileNonFixed.h \
  pixels-common/include/physical/natives/DirectUringRandomAccessFileStatic.h \
  pixels-common/lib/physical/GlobalByteBufferPool.cpp \
  pixels-common/lib/physical/GlobalStaticBufferPool.cpp \
  pixels-common/lib/physical/SpdkBufferPool.cpp \
  pixels-common/lib/physical/ThreadContext.cpp \
  pixels-common/lib/physical/natives/DirectSpdkRandomAccessFile.cpp \
  pixels-common/lib/physical/natives/DirectUringRandomAccessFileNonFixed.cpp \
  pixels-common/lib/physical/natives/DirectUringRandomAccessFileStatic.cpp \
  pixels-core/include/vector/ColumnVectorBufferPool.h \
  pixels-core/lib/vector/ColumnVectorBufferPool.cpp \
  pixels-duckdb/ArrowRandomAccessFile.cpp \
  pixels-duckdb/ArrowRandomAccessFile.hpp \
  pixels-duckdb/CPUAffinity.h \
  pixels-duckdb/ParquetPixelsScan.cpp \
  pixels-duckdb/ParquetPixelsScan.hpp
```

精确保存必要文档和测试源码：

```bash
git add -f -- \
  docs/buffer-pool \
  docs/features \
  docs/io \
  docs/todo/GIT_TODO.md \
  tests/BufferPool \
  tests/column-size-analyzer \
  tests/reader-performance-test/CMakeLists.txt \
  tests/reader-performance-test/README.md \
  tests/reader-performance-test/reader_performance_test.cpp \
  tests/reader-performance-test/create_rowbatch_microbench.cpp \
  tests/reader-performance-test/run_perf_simple.py \
  tests/reader-performance-test/config.properties \
  tests/parquet-reader-performance-test/CMakeLists.txt \
  tests/parquet-reader-performance-test/README.md \
  tests/parquet-reader-performance-test/IMPLEMENTATION.md \
  tests/parquet-reader-performance-test/parquet_reader_performance_test.cpp \
  tests/parquet-reader-performance-test/run_perf.py \
  tests/parquet-reader-performance-test/config.properties
```

检查误加内容：

```bash
git diff --cached --name-only | \
  rg 'build/|perf-results|perf-breakdown|perf\.data|collapsed|\.svg$|\.pdf$|__pycache__' || true
```

有误加时：

```bash
git restore --staged -- <误加路径>
```

创建本地快照：

```bash
git diff --cached --stat
git commit -m "WIP: preserve Pixels C++ workspace before rebuilding history"
git tag backup/pixels-cpp-workspace-20260825

SNAPSHOT_COMMIT=$(git rev-parse HEAD)
echo "$SNAPSHOT_COMMIT"
```

此分支和 tag 暂时只保留在本地。

## 2. 与最新 origin/master 保持一致

网络更新命令先留空。本地已经执行过 fetch，并已知 `origin/master=b99eef2b`。

### 2.1 网络更新占位

```text
TODO(network): 再次 fetch origin 并确认最新 origin/master。
```

### 2.2 确认祖先关系

```bash
git log -1 --oneline origin/master
git merge-base --is-ancestor 2a109658 origin/master
echo $?
```

预期输出 `0`。

### 2.3 创建干净重建分支

完整快照创建后执行：

```bash
git switch -c rebuild/pixels-cpp-prs origin/master
```

验证：

```bash
git status
git rev-parse HEAD
git rev-parse origin/master
git diff --exit-code origin/master...HEAD
```

工作区应为空，两个提交号应相同。

## 3. 将旧代码应用到新 master，但不生成旧提交

按旧历史顺序应用：

```bash
git cherry-pick -n 159e29c0
git cherry-pick -n c681909b
git cherry-pick -n ef6ae3f1
git cherry-pick -n 6a0995d5
git cherry-pick -n "$SNAPSHOT_COMMIT"
```

每一步出现冲突时：

```bash
git status
git diff --name-only --diff-filter=U
```

- [ ] 同时阅读上游版本和本地版本。
- [ ] 不对复杂文件直接整体选择 `ours` 或 `theirs`。
- [ ] 优先保留上游新增类型、API 和 bug fix，再迁入本地功能。
- [ ] 解决后精确暂存：

```bash
git add -- <已解决文件1> <已解决文件2>
git cherry-pick --continue
```

如果要放弃当前正在应用的旧提交：

```bash
git cherry-pick --abort
```

全部应用后，将内容恢复成未暂存状态：

```bash
git reset
git status --short --untracked-files=all
```

此时 `HEAD` 仍应等于 `origin/master`：

```bash
git rev-parse HEAD
git rev-parse origin/master
```

## 4. 清理与排除规则

- [ ] `.gitignore` 不忽略整个 `docs/`、`tests/` 或 `testcase/`。
- [ ] 只忽略 build、缓存、perf 输出、火焰图和结果目录。
- [ ] 不提交 `Untitled`、`__pycache__`、perf 数据、生成 SVG/PDF、实验结果。
- [ ] 不提交 `/home/whz`、`/data/9a3-*`、`/tmp/pixels*`。
- [ ] 不提交整个未跟踪 `third-party/arrow/`。
- [ ] 不提交不可访问的 DuckDB 子模块提交指针。
- [ ] FlameGraph 移动和 `.gitmodules` 修改单独确认。
- [ ] testcase 旧脚本删除和目录重组不混入功能 PR。

`.gitignore` 简单提交：

```bash
git add -- .gitignore
git diff --cached --check
git commit -m "chore(cpp): ignore generated build and profiling artifacts"
```

## 5. 原始 PR 划分和依赖顺序（已由当前线性分支取代）

| PR | 本地分支 | 功能 | 依赖 |
|---|---|---|---|
| 1 | `feature/cpp-flatbuffers-metadata` | C++ FlatBuffers 元数据迁移 | 无 |
| 2 | `feature/cpp-profiling-tools` | Profiling 与列大小工具 | 无 |
| 3 | `feature/cpp-buffer-pools` | Global/Dynamic/Static BufferPool | PR 2 |
| 4 | `feature/cpp-io-uring-readers` | fixed/non-fixed/dynamic io_uring | PR 3 |
| 5 | `feature/cpp-column-vector-reuse` | ColumnVector 所有权与内存复用 | 可独立 |
| 6 | `fix/cpp-footer-lifetime` | Footer Cache 与元数据生命周期 | PR 1 |
| 7 | `feature/duckdb-pixels-parallel-scan` | DuckDB Pixels 并行扫描 | PR 3～6 |
| 8 | `feature/duckdb-arrow-parquet-scan` | Arrow Parquet 扫描 | PR 7 或独立 |
| 9 | `feature/cpp-spdk-reader` | 可选 SPDK NVMe 后端 | PR 3、4 |

建议顺序：

```text
第一批并行：PR 1、PR 2、PR 5
第二批：PR 6（等待 PR 1）、PR 3（等待 PR 2）
第三批：PR 4（等待 PR 3）
第四批：PR 7（等待 PR 3、4、5、6）
第五批：PR 8、PR 9
```

## 6. PR 1：C++ FlatBuffers 元数据迁移

### 提交文件

```text
../.gitmodules                                      # 只选 FlatBuffers submodule
CMakeLists.txt
Makefile
pixels.fbs
../proto/pixels.fbs
../proto/pixels.proto                               # 只选迁移相关修改
third-party/flatbuffers                             # 正式 submodule 指针

pixels-cli/CMakeLists.txt
pixels-cli/include/executor/LoadExecutor.h
pixels-cli/include/load/PixelsConsumer.h
pixels-cli/lib/executor/LoadExecutor.cpp
pixels-cli/lib/load/PixelsConsumer.cpp
pixels-cli/main.cpp

pixels-core/include/PixelsFooterCache.h
pixels-core/include/PixelsReader.h
pixels-core/include/PixelsReaderBuilder.h
pixels-core/include/PixelsReaderImpl.h
pixels-core/include/PixelsWriterImpl.h
pixels-core/include/TypeDescription.h
pixels-core/include/reader/ColumnReader.h
pixels-core/include/reader/DateColumnReader.h
pixels-core/include/reader/DecimalColumnReader.h
pixels-core/include/reader/IntColumnReader.h
pixels-core/include/reader/LongColumnReader.h
pixels-core/include/reader/PixelsRecordReaderImpl.h
pixels-core/include/reader/StringColumnReader.h
pixels-core/include/reader/TimestampColumnReader.h
pixels-core/include/stats/StatsRecorder.h
pixels-core/include/writer/ColumnWriter.h
pixels-core/include/writer/IntColumnWriter.h
pixels-core/include/writer/LongColumnWriter.h

pixels-core/lib/PixelsFooterCache.cpp
pixels-core/lib/PixelsReaderBuilder.cpp
pixels-core/lib/PixelsReaderImpl.cpp
pixels-core/lib/PixelsWriterImpl.cpp
pixels-core/lib/TypeDescription.cpp
pixels-core/lib/encoding/RunLenIntEncoder.cpp
pixels-core/lib/reader/ColumnReader.cpp
pixels-core/lib/reader/DateColumnReader.cpp
pixels-core/lib/reader/DecimalColumnReader.cpp
pixels-core/lib/reader/IntColumnReader.cpp
pixels-core/lib/reader/LongColumnReader.cpp
pixels-core/lib/reader/PixelsRecordReaderImpl.cpp
pixels-core/lib/reader/StringColumnReader.cpp
pixels-core/lib/reader/TimestampColumnReader.cpp
pixels-core/lib/stats/StatsRecorder.cpp
pixels-core/lib/vector/IntColumnVector.cpp
pixels-core/lib/writer/ColumnWriter.cpp
pixels-core/lib/writer/DateColumnWriter.cpp
pixels-core/lib/writer/DecimalColumnWriter.cpp
pixels-core/lib/writer/IntColumWriter.cpp
pixels-core/lib/writer/LongColumnWriter.cpp
pixels-core/lib/writer/TimestampColumnWriter.cpp

tests/CMakeLists.txt
tests/data/example.pxl
tests/writer/CMakeLists.txt
tests/writer/IntegerWriterTest.cpp
tests/writer/PixelsWriterTest.cpp
```

### 暂存

```bash
git add -N pixels.fbs
git add -p CMakeLists.txt Makefile pixels.fbs
git add -p pixels-cli
git add -p pixels-core/include pixels-core/lib
git add -p tests/CMakeLists.txt tests/writer
git add -p ../.gitmodules
```

### Commit

```bash
git commit \
  -m "refactor(cpp): replace protobuf metadata with FlatBuffers" \
  -m "Replace the Pixels C++ footer, row-group metadata, statistics, reader, and writer serialization paths with FlatBuffers. Update schema generation, CLI integration, build dependencies, and round-trip tests. Preserve backing ByteBuffers while FlatBuffer metadata views remain alive, and keep the format migration independent from BufferPool and scan optimizations."
```

## 7. PR 2：Profiling 与列大小工具

### 提交文件

```text
pixels-common/include/profiler/AbstractProfiler.h
pixels-common/include/profiler/ProfilerSwitch.h
pixels-common/include/profiler/TimeProfiler.h
pixels-common/lib/profiler/CountProfiler.cpp
pixels-common/lib/profiler/ProfilerSwitch.cpp
pixels-common/lib/profiler/TimeProfiler.cpp
pixels-common/include/utils/ColumnSizeCSVReader.h
pixels-common/include/utils/ColumnSizeCSVWriter.h
pixels-common/lib/utils/ColumnSizeCSVWriter.cpp
pixels-common/include/utils/ConfigFactory.h
pixels-common/lib/utils/ConfigFactory.cpp
pixels-common/CMakeLists.txt                         # 只选本 PR source
tests/column-size-analyzer/CMakeLists.txt
tests/column-size-analyzer/ColumnSizeAnalyzer.cpp
tests/column-size-analyzer/README.md
tests/CMakeLists.txt                                 # 只选 analyzer
pixels-cpp.properties                                # 只选本 PR key
docs/features/column-size-collection.md
```

配置：

```properties
pixel.column.size.path=
pixel.enable.profiler=false
```

### Commit

```bash
git commit \
  -m "feat(cpp): add configurable profiling and column size utilities" \
  -m "Add runtime-controlled TimeProfiler and CountProfiler instrumentation so profiling overhead is avoided when disabled. Add metadata-only column-size reader, writer, and analyzer utilities that inspect Row Group Footers and report the maximum chunk size of every column. Include configuration, CMake, tests, and documentation without machine-specific paths or generated results."
```

## 8. PR 3：BufferPool 基础

### 提交文件

```text
global-bufferpool.properties
pixels-common/include/physical/BufferPool.h
pixels-common/lib/physical/BufferPool.cpp
pixels-common/include/physical/DynamicBufferPool.h
pixels-common/lib/physical/DynamicBufferPool.cpp
pixels-common/include/physical/GlobalByteBufferPool.h
pixels-common/lib/physical/GlobalByteBufferPool.cpp
pixels-common/include/physical/GlobalStaticBufferPool.h
pixels-common/lib/physical/GlobalStaticBufferPool.cpp
pixels-common/include/physical/ThreadContext.h
pixels-common/lib/physical/ThreadContext.cpp
pixels-common/include/physical/natives/ByteBuffer.h    # 排除 SPDK DMA
pixels-common/lib/physical/natives/ByteBuffer.cpp      # 排除 SPDK DMA
pixels-common/CMakeLists.txt                           # 只选 pool sources
tests/BufferPool/CMakeLists.txt
tests/BufferPool/dynamic_buffer_pool_test.cpp
tests/CMakeLists.txt                                   # 只选 BufferPool
pixels-cpp.properties                                  # 只选本 PR key
docs/buffer-pool/global-buffer-pool.md
docs/buffer-pool/global-static-buffer-pool.md
```

配置：

```properties
pixel.bufferpool.fixedSize=false
pixel.bufferpool.bufferpoolSize=0
pixels.enable.dynamic.buffer=false
pixels.doublebuffer=false
pixel.enable.globalBytebuffer=false
pixel.enable.globalStaticBytebuffer=false
pixel.globalStaticBytebuffer.columnSize=
pixels.static.buffer.hugepage=false
```

该 PR 建议保留三个内部提交：

```text
feat(cpp): introduce a reusable global byte buffer pool
feat(cpp): support dynamic and double-buffered read pools
feat(cpp): add a thread-aware global static buffer pool
```

GitHub 最终可 squash 为一个 PR 提交。

## 9. PR 4：io_uring 多后端

### 提交文件

```text
pixels-common/include/physical/PhysicalReader.h
pixels-common/include/physical/Request.h
pixels-common/include/physical/RequestBatch.h
pixels-common/include/physical/io/PhysicalLocalReader.h
pixels-common/include/physical/natives/DirectUringRandomAccessFile.h
pixels-common/include/physical/natives/DirectUringRandomAccessFileDynamic.h
pixels-common/include/physical/natives/DirectUringRandomAccessFileNonFixed.h
pixels-common/include/physical/natives/DirectUringRandomAccessFileStatic.h
pixels-common/lib/physical/Request.cpp
pixels-common/lib/physical/RequestBatch.cpp
pixels-common/lib/physical/io/PhysicalLocalReader.cpp       # 排除 SPDK
pixels-common/lib/physical/natives/DirectRandomAccessFile.cpp
pixels-common/lib/physical/natives/DirectUringRandomAccessFile.cpp
pixels-common/lib/physical/natives/DirectUringRandomAccessFileDynamic.cpp
pixels-common/lib/physical/natives/DirectUringRandomAccessFileNonFixed.cpp
pixels-common/lib/physical/natives/DirectUringRandomAccessFileStatic.cpp
pixels-common/lib/physical/scheduler/NoopScheduler.cpp
pixels-common/lib/physical/storage/LocalFS.cpp               # 排除 SPDK
pixels-common/CMakeLists.txt                                 # 只选 reader sources
pixels-core/include/reader/PixelsRecordReaderImpl.h          # 只选 backend
pixels-core/lib/reader/PixelsRecordReaderImpl.cpp            # 只选 backend
pixels-cpp.properties
docs/io/unified-io-memory-manager-design.md
```

配置：

```properties
localfs.iouring.use.fixed.buffer=false
```

### Commit

```bash
git commit \
  -m "feat(cpp): add fixed non-fixed and dynamic io_uring readers" \
  -m "Refactor local physical reads to select synchronous pread, AIO, fixed-buffer io_uring, reusable non-fixed io_uring, or dynamic sparse-registration backends. Extend requests with column and registered-buffer metadata, obtain worker rings from ThreadContext, and preserve Direct I/O alignment, completion, cleanup, and fallback behavior. Keep SPDK outside this change."
```

## 10. PR 5：ColumnVector 内存复用

### 提交文件

```text
pixels-core/include/vector/ColumnVectorBufferPool.h
pixels-core/lib/vector/ColumnVectorBufferPool.cpp
pixels-core/include/vector/ColumnVector.h
pixels-core/lib/vector/ColumnVector.cpp
pixels-core/include/vector/BinaryColumnVector.h
pixels-core/lib/vector/BinaryColumnVector.cpp
pixels-core/lib/vector/DateColumnVector.cpp
pixels-core/lib/vector/DecimalColumnVector.cpp
pixels-core/lib/vector/IntColumnVector.cpp
pixels-core/lib/vector/LongColumnVector.cpp
pixels-core/lib/vector/TimestampColumnVector.cpp
pixels-core/lib/reader/ColumnReader.cpp
pixels-core/lib/reader/DateColumnReader.cpp
pixels-core/lib/reader/DecimalColumnReader.cpp
pixels-core/lib/reader/IntColumnReader.cpp
pixels-core/lib/reader/LongColumnReader.cpp
pixels-core/lib/reader/TimestampColumnReader.cpp
pixels-core/CMakeLists.txt
pixels-cpp.properties
docs/features/column-vector-reuse.md
```

配置：

```properties
pixels.columnvector.pool=true
pixels.malloc.tune=true
pixels.malloc.arena_max=0
```

### Commit

```bash
git commit \
  -m "perf(cpp): reuse column vector backing storage" \
  -m "Add a thread-local aligned buffer pool keyed by allocation size and alignment to reuse large ColumnVector backing arrays across files. Track data and null-buffer ownership so zero-copy views into Reader ByteBuffers are never freed by vectors. Reuse BinaryColumnVector storage and lazily allocate write-only string containers while preserving resize and close behavior."
```

## 11. PR 6：Footer Cache 与元数据生命周期

### 提交文件

```text
pixels-core/include/PixelsFooterCache.h
pixels-core/lib/PixelsFooterCache.cpp
pixels-core/lib/PixelsReaderBuilder.cpp
pixels-core/include/reader/PixelsRecordReaderImpl.h  # 只选 footer lifetime
pixels-core/lib/reader/PixelsRecordReaderImpl.cpp    # 只选 footer/nextRowGroup
include/PixelsReadGlobalState.hpp                    # 只选共享 Footer Cache
```

可独立验证时加入：

```text
pixels-core/lib/PixelsBitMask.cpp
pixels-core/lib/PixelsFilter.cpp
pixels-core/include/TypeDescription.h
pixels-core/lib/TypeDescription.cpp
```

### Commit

```bash
git commit \
  -m "fix(cpp): preserve footer and row group metadata lifetimes" \
  -m "Share parsed Pixels file footers across parallel reader states and retain each Row Group Footer ByteBuffer while FlatBuffer views reference it. Centralize Row Group progression and batch-state updates to prevent dangling metadata pointers, repeated footer reads, and inconsistent prefetch state. Keep this correctness fix separate from allocation and scan scheduling."
```

## 12. PR 7：DuckDB Pixels 并行扫描

### 提交文件

```text
include/PixelsReadGlobalState.hpp
include/PixelsReadLocalState.hpp
pixels-duckdb/CPUAffinity.h
pixels-duckdb/PixelsScanFunction.cpp             # 排除 Parquet/SPDK
pixels-duckdb/pixels_extension.cpp               # 排除 Parquet/SPDK
pixels-core/include/reader/PixelsRecordReaderImpl.h
pixels-core/lib/reader/PixelsRecordReaderImpl.cpp
pixels-cpp.properties
docs/features/cpu-affinity.md
tests/reader-performance-test/CMakeLists.txt
tests/reader-performance-test/README.md
tests/reader-performance-test/reader_performance_test.cpp
tests/reader-performance-test/create_rowbatch_microbench.cpp
tests/reader-performance-test/run_perf_simple.py
tests/reader-performance-test/config.properties
```

配置：

```properties
pixels.enable.cpu.affinity=false
pixels.cpu.affinity.strategy=round-robin
pixels.cpu.affinity.core.mapping=
```

### Commit

```bash
git commit \
  -m "feat(duckdb): integrate reusable buffers into parallel Pixels scans" \
  -m "Associate each DuckDB Pixels scan worker with a stable thread id, current ring, prefetch ring, and reusable column buffers. Coordinate Row Group submission, completion, decoding, and buffer switching so double buffering overlaps the next read with current processing. Share footer metadata and add optional CPU affinity while preserving synchronous and non-fixed fallbacks."
```

## 13. PR 8：Arrow Parquet 扫描

### 提交文件

```text
pixels-duckdb/ArrowRandomAccessFile.cpp
pixels-duckdb/ArrowRandomAccessFile.hpp
pixels-duckdb/ParquetPixelsScan.cpp
pixels-duckdb/ParquetPixelsScan.hpp
pixels-duckdb/pixels_extension.cpp                  # 只选 Parquet 注册
CMakeLists.txt                                     # 只选 Arrow/Parquet
Makefile                                           # 只选 Arrow/Parquet
tests/CMakeLists.txt                                # 只选 Parquet benchmark
tests/parquet-reader-performance-test/CMakeLists.txt
tests/parquet-reader-performance-test/README.md
tests/parquet-reader-performance-test/IMPLEMENTATION.md
tests/parquet-reader-performance-test/parquet_reader_performance_test.cpp
tests/parquet-reader-performance-test/run_perf.py
tests/parquet-reader-performance-test/config.properties
docs/io/parquet-uring.md
```

- [ ] Arrow 依赖方案留待确认。
- [ ] 不提交当前整个 `third-party/arrow/`。
- [ ] 不提交 benchmark 结果。

### Commit

```bash
git commit \
  -m "feat(duckdb): add an Arrow-based Parquet scan path" \
  -m "Adapt project random-access I/O to Arrow, parse Parquet metadata, distribute Row Groups across workers, and convert Arrow RecordBatches into DuckDB output. Support asynchronous and optionally double-buffered reads. Add a pure-I/O Parquet benchmark for comparison with Pixels without committing generated measurements or machine-specific paths."
```

## 14. PR 9：SPDK NVMe 后端

### 提交文件

```text
pixels-common/include/physical/SpdkBufferPool.h
pixels-common/lib/physical/SpdkBufferPool.cpp
pixels-common/include/physical/natives/DirectSpdkRandomAccessFile.h
pixels-common/lib/physical/natives/DirectSpdkRandomAccessFile.cpp
pixels-common/include/physical/natives/ByteBuffer.h       # 只选 BY_SPDK_DMA
pixels-common/lib/physical/natives/ByteBuffer.cpp         # 只选 spdk_dma_free
pixels-common/lib/physical/storage/LocalFS.cpp            # 只选 SPDK
pixels-common/lib/physical/io/PhysicalLocalReader.cpp     # 只选 SPDK
pixels-core/include/reader/PixelsRecordReaderImpl.h       # 只选 SPDK
pixels-core/lib/reader/PixelsRecordReaderImpl.cpp         # 只选 SPDK
pixels-duckdb/PixelsScanFunction.cpp                      # 只选 SPDK
CMakeLists.txt                                            # PIXELS_ENABLE_SPDK
pixels-common/CMakeLists.txt                              # SPDK source/library
pixels-cpp.properties
testcase/spdk/
docs/io/spdk.md
```

配置：

```properties
localfs.enable.spdk=false
localfs.spdk.lba_map=
```

### Commit

```bash
git commit \
  -m "feat(cpp): add an optional SPDK NVMe read backend" \
  -m "Add optional SPDK environment and NVMe initialization, external file-to-LBA mapping, and per-thread queue pairs. Allocate two sets of DMA-capable per-column buffers so NVMe reads complete directly into Reader-consumed memory without bounce buffers. Keep SPDK behind a build option and disabled by default so ordinary builds need no SPDK installation or hardware."
```

## 15. 原子提交完成后的本地整理

记录提交：

```bash
git log --reverse --format='%H %s' origin/master..HEAD > /tmp/pixels-rebuilt-commits.txt
```

遗漏内容使用 fixup：

```bash
git add -- <明确文件>
git commit --fixup=<目标提交号>
git rebase -i --autosquash origin/master
```

最终定义提交变量：

```bash
FLATBUFFERS=<hash>
PROFILER=<hash>
GLOBAL_POOL=<hash>
DYNAMIC_POOL=<hash>
STATIC_POOL=<hash>
IO_URING=<hash>
VECTOR_POOL=<hash>
FOOTER=<hash>
FILTER=<hash-if-any>
DUCKDB_SCAN=<hash>
PARQUET=<hash>
SPDK=<hash>
```

## 16. 从重建分支组装本地 PR 分支

以下命令只创建本地分支，不 push。

第一批：

```bash
git switch -c feature/cpp-flatbuffers-metadata origin/master
git cherry-pick "$FLATBUFFERS"

git switch -c feature/cpp-profiling-tools origin/master
git cherry-pick "$PROFILER"

git switch -c feature/cpp-column-vector-reuse origin/master
git cherry-pick "$VECTOR_POOL"
```

依赖 PR 合并并更新本地 `origin/master` 后：

```bash
git switch -c feature/cpp-buffer-pools origin/master
git cherry-pick "$GLOBAL_POOL" "$DYNAMIC_POOL" "$STATIC_POOL"

git switch -c feature/cpp-io-uring-readers origin/master
git cherry-pick "$IO_URING"

git switch -c fix/cpp-footer-lifetime origin/master
git cherry-pick "$FOOTER"

git switch -c feature/duckdb-pixels-parallel-scan origin/master
git cherry-pick "$DUCKDB_SCAN"

git switch -c feature/duckdb-arrow-parquet-scan origin/master
git cherry-pick "$PARQUET"

git switch -c feature/cpp-spdk-reader origin/master
git cherry-pick "$SPDK"
```

每个分支检查：

```bash
git log --oneline origin/master..HEAD
git diff --stat origin/master...HEAD
git diff --name-status origin/master...HEAD
git diff --check origin/master...HEAD
```

## 17. 每个提交和 PR 的本地检查

### 无关信息

```bash
git diff origin/master...HEAD | \
  rg '/home/whz|/data/9a3|/tmp/pixels|password|passwd|secret|token|api[_-]?key' || true

git diff --name-only origin/master...HEAD | \
  rg 'build/|perf-results|perf\.data|collapsed|__pycache__|\.pyc$' || true
```

### 配置重复 key

```bash
awk -F= '
  /^[[:space:]]*[^#[:space:]][^=]*=/ {
    key=$1
    gsub(/[[:space:]]/, "", key)
    count[key]++
  }
  END {
    for (key in count)
      if (count[key] > 1)
        print key, count[key]
  }
' pixels-cpp.properties
```

### 子模块

```bash
git submodule status
git diff --submodule=log origin/master...HEAD
git -C pixels-duckdb/duckdb status --short
```

### 基础测试门槛

- [ ] 相关 CMake target 可以独立构建。
- [ ] BufferPool：allocate、grow、reuse、switch、reset。
- [ ] io_uring：pread、fixed、non-fixed、dynamic、single/double buffer。
- [ ] ColumnVector：pool on/off、resize、close、ASan/UBSan。
- [ ] Footer：多文件、多 Row Group、并发扫描、ASan。
- [ ] DuckDB：单线程/多线程和 filter 查询结果一致。
- [ ] Parquet：与 DuckDB `parquet_scan` 结果一致。
- [ ] SPDK：关闭 SPDK 时普通构建成功；硬件测试结果与文件系统读取一致。

## 18. 网络操作占位

以下内容暂不生成详细命令：

- [ ] TODO(network)：更新远端并确认最新 master。
- [ ] TODO(network)：为 9 个功能创建对应 GitHub Issue。
- [ ] TODO(network)：将本地 PR 分支 push 到 `whzruc/pixels`。
- [ ] TODO(network)：创建以 `pixelsdb/pixels:master` 为 base 的 Draft PR。
- [ ] TODO(network)：在 PR 描述中加入 Summary、Dependencies、Testing、`Closes #N`。
- [ ] TODO(network)：查看 CI，测试通过后将 Draft 标记为 Ready。
- [ ] TODO(network)：PR 合并后删除 fork 分支。

推荐最终 PR 标题格式：

```text
[Issue #N] replace Protobuf metadata with FlatBuffers
[Issue #N] add C++ profiling and column size utilities
[Issue #N] add reusable buffer pools to Pixels C++
[Issue #N] support multiple io_uring buffer strategies
[Issue #N] reuse ColumnVector backing buffers
[Issue #N] preserve Pixels footer metadata lifetimes
[Issue #N] integrate reusable buffers into DuckDB Pixels scans
[Issue #N] add an Arrow-based Parquet scan path
[Issue #N] add an optional SPDK NVMe reader
```

## 19. 最终完成条件

- [ ] 新历史以执行时最新 `origin/master` 为基线。
- [ ] 新历史不包含 4 个旧本地提交。
- [ ] `2a109658` 之后的全部上游改动被保留。
- [ ] 每项功能有独立提交和明确的 PR 文件范围。
- [ ] 混合文件通过 `git add -p` 拆分。
- [ ] 配置项随实现它的功能提交。
- [ ] 主配置没有本机路径、重复 key 或失效旧 key。
- [ ] 测试源码与生成结果分离。
- [ ] Arrow、DuckDB、FlatBuffers、FlameGraph 依赖方式明确且可复现。
- [ ] SPDK 和硬件相关功能默认关闭。
- [ ] 所有本地测试通过后再执行网络阶段。
