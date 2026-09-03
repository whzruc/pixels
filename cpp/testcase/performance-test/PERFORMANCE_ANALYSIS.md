# Pixels Buffer 性能分析：doublebuffer 回归、火焰图判读与 jemalloc 方案

本文档汇总 q45 / 24ssd 场景下 **singlebuffer vs doublebuffer** 的性能调查结论，涵盖 on-CPU / off-CPU 火焰图判读方法、根因分析、已尝试优化方案及 **jemalloc vs glibc** 对比实验，并给出可复现的完整运行命令。

**关联套件使用说明**见 [README.md](./README.md)。

---

## 1. 问题概述

在 48 线程、24 SSD、`doublebuffer` 模式下，q45 查询出现 **负向扩展**：线程从 24 增到 48 时墙钟时间反而变长，且 **doublebuffer 慢于 singlebuffer**。

典型数据（glibc，套件结果 `results/20260615_084721/summary_wall_time.csv`）：

| 线程 | singlebuffer | doublebuffer |
|-----:|-------------:|-------------:|
| 12   | 22.186 s     | 16.924 s     |
| 24   | 13.506 s     | 10.715 s     |
| 48   | 10.340 s     | **14.331 s** |

**最终结论**：瓶颈不在 buffer 架构本身，而在于 **Pixels 扩展的内存分配落在 glibc malloc 上**，高并发 + doublebuffer 双倍分配触发 glibc 多 arena 的 `grow_heap`/`mprotect` 与 arena 锁争用。使用系统 jemalloc（`LD_PRELOAD`）可消除 doublebuffer 在 t48 的回归，并整体提速约 14%–34%。

---

## 2. on-CPU 与 off-CPU 火焰图：区别与结合判读

### 2.1 本质区别

| | on-CPU 火焰图 | off-CPU 火焰图 |
|---|---|---|
| 文件 | `*_oncpu_cpu-clock.svg` | `offcpu.svg` |
| 采样 | `perf record -e cpu-clock` | bcc `offcputime`（kprobe `finish_task_switch`） |
| 宽度含义 | **CPU 时间**（samples） | **阻塞时长**（microseconds） |
| 回答的问题 | 算力烧在哪（指令、解码、I/O 提交） | 线程被什么堵住（锁、缺页、I/O、sleep） |

线程在任意时刻 **要么 on-CPU 要么 off-CPU**，两者互补，但不能直接把百分比相加（分母不同）。

### 2.2 判读流程（推荐）

```
1. 先看 perf stat 的 CPU 利用率
   task-clock / (time_elapsed × 线程数) 很低 → 以 off-CPU 为主
   很高 → 以 on-CPU 为主

2. off-CPU：剔除空闲帧
   忽略 TaskScheduler → futex、[unknown] 等“没活干”的等待
   只看 readBatch / scan / createColumn / I/O / 缺页 / mprotect 等工作路径

3. 同一函数在两张图里的表现
   仅 on-CPU 宽 → 纯计算热点
   仅 off-CPU 宽 → 阻塞瓶颈（优化等待源）
   都宽       → 既算又堵（如 I/O 提交 + io_schedule）

4. 用墙钟 A/B 验证：off-CPU 上很大的帧不一定在关键路径上（可能被并行重叠）
```

### 2.3 本案例示例（doublebuffer t48，glibc）

**perf stat**（`results/20260615_084721/q45_24ssd_doublebuffer_t48/perf_stat.txt`）：

- `time elapsed` ≈ 31.58 s
- `task-clock` ≈ 20.66 s，**0.654 CPUs utilized**（48 线程下几乎都在等）
- `page-faults` ≈ 7.9M，`dTLB-load-misses` 5.79%

→ **阻塞主导**，优先看 off-CPU。

**off-CPU 活跃路径上的大头**（需忽略 futex 空闲）：

| 栈 | 占比（约） | 含义 |
|----|-----------:|------|
| `__GI___mprotect` → `do_mprotect_pkey` → `rwsem_down_write_slowpath` | 43% | glibc arena 扩堆，拿进程级 `mmap_lock` **写锁** |
| `sysmalloc` → `page_fault` → `down_read` | 24% | malloc 扩堆缺页，等 **读锁** |
| `createColumn` → `IntColumnVector` → `memset` → 缺页 | 20% | 列向量首次触摸 |

**on-CPU 补充**（不算空闲时）：

- `StringColumnReader::read` / `BinaryColumnVector::setRef` ~17%（构造 `string_t` 的计算）
- `__io_uring_submit` / `submit_bio` ~11%（I/O 提交）

**重要教训**：off-CPU 里 66% 的 glibc 内存管理帧 **与墙钟关键路径重叠**，单独消掉 off-CPU 帧（列向量池、arena 限制）**不一定**缩短墙钟；换分配器（jemalloc）才一次性解决。

---

## 3. 根因：谁在用 glibc，谁在用 jemalloc

### 3.1 CMake / 二进制事实

| 组件 | 分配器 | 说明 |
|------|--------|------|
| DuckDB 内部 | 自带 **前缀** jemalloc（`duckdb_je_*`） | 扩展内静态链接，**不**接管全局 `malloc` |
| Pixels 扩展 | **glibc** | `new`/`malloc`/`posix_memalign`（列向量、ByteBuffer、decoder 等） |

验证命令：

```bash
# DuckDB 内部 jemalloc 符号（有）
nm build/release/duckdb | grep -c duckdb_je_

# 全局 malloc 导出（无 → 未做 interposition）
nm -D build/release/duckdb | grep -E ' (malloc|posix_memalign|_Znwm)$'
```

顶层 [CMakeLists.txt](../../CMakeLists.txt) 中 `find_library(JEMALLOC_LIB jemalloc)` 的链接行 **被注释**；[Makefile](../../Makefile) 入口为 DuckDB 的 CMake，不会自动把系统 jemalloc 链进 Pixels。

因此 off-CPU 中同时出现 `duckdb_je_*` 与 `__GI___mprotect`/`sysmalloc` 是正常现象：**Pixels 路径走 glibc**。

### 3.2 doublebuffer 为何放大 glibc 问题

- `GlobalStaticBufferPool` 已在初始化时对 **内容读缓冲** 做持久预分配 + `memset` 预触摸（内容缓冲本身不是瓶颈）。
- doublebuffer 仍使 **每线程、每列** 的其它分配（列向量、`ByteBuffer` 视图、decoder 等）在高并发下翻倍，glibc 为每线程创建 arena，`grow_heap` 触发大量 `mprotect`，与缺页读锁争抢 **进程级 `mmap_lock`**。

---

## 4. 已尝试优化方案与结果

### 4.1 方案 B：列向量缓冲池（`ColumnVectorBufferPool`）

- **实现**：线程局部 free-list 复用 `BinaryColumnVector` 的 `string_t` 数组；配置 `pixels.columnvector.pool`（默认 true）。
- **off-CPU**：`setRef → down_read` 从旧跑 ~34% **降至 0**（火焰图中无 `setRef` 帧）。
- **墙钟**：**无改善**，pool ON 略差于 OFF（doublebuffer t48 约 14s vs 11.5s，有噪声）。
- **结论**：消除了重叠的缺页等待，但不在墙钟关键路径上；默认可关或仅作调试。

### 4.2 方案 A：glibc mallopt

- **实现**：`pixels.malloc.tune=true` 时 `M_MMAP_MAX=0`、`M_TRIM_THRESHOLD=-1`；可选 `pixels.malloc.arena_max`。
- **arena 限制 A/B**（doublebuffer t48）：

| `pixels.malloc.arena_max` | 墙钟（约） |
|---------------------------|-----------|
| 0（不限制，默认）         | ~13.6 s   |
| 8                         | **~26 s**（灾难性回归） |

- **结论**：收紧 arena 把争用从 `mmap_lock` 挪到 arena 互斥锁，**不要默认限制**；`arena_max=0` 为安全默认。

### 4.3 内容缓冲预触摸 + MADV_HUGEPAGE

- `GlobalStaticBufferPool` 已预分配 + `memset`；可选 `pixels.static.buffer.hugepage=true`（默认 false）。
- 针对 dTLB miss，可作为后续试验，非本次主因修复。

### 4.4 jemalloc（LD_PRELOAD）—— **有效方案**

系统库：`/lib/x86_64-linux-gnu/libjemalloc.so.2`

**t48 三次重复（`/tmp/pixels_verify/`）**：

| 模式 | glibc | jemalloc |
|------|------:|---------:|
| singlebuffer | 10.37 s（10.33–10.39） | **8.96 s**（8.96–8.97） |
| doublebuffer | 13.2 s（12.4–14.7，抖动大） | **8.67 s**（8.66–8.68，极稳） |

**完整线程扩展曲线（12 / 24 / 48）**：

| 线程 | single (glibc) | single (jemalloc) | double (glibc) | double (jemalloc) |
|-----:|---------------:|------------------:|---------------:|------------------:|
| 12   | 22.334         | 19.951            | 16.879         | 14.742            |
| 24   | 13.325         | 11.974            | 11.103         | **8.908**         |
| 48   | 10.493         | 8.967             | **12.098**     | **8.668**         |

jemalloc 下 doublebuffer 在 t48 **重新快于** singlebuffer，负向扩展消失。

---

## 5. 配置项参考

配置文件：`~/opt/pixels/etc/pixels-cpp.properties`（运行时）；模板见仓库 [pixels-cpp.properties](../../pixels-cpp.properties)。

| 键 | 默认 | 作用 |
|----|------|------|
| `pixels.malloc.tune` | true | glibc：`M_MMAP_MAX=0`、`M_TRIM_THRESHOLD=-1`；**jemalloc 下无效** |
| `pixels.malloc.arena_max` | 0 | glibc：`M_ARENA_MAX`；0=不限制；**勿轻易设为 8** |
| `pixels.columnvector.pool` | true | 列向量 `string_t` 数组线程局部池；墙钟中性偏负 |
| `pixels.static.buffer.hugepage` | false | 静态内容缓冲 `MADV_HUGEPAGE` |
| `pixels.doublebuffer` | 由 suite 按 mode 写入 | double-buffer 开关 |
| `pixel.enable.globalStaticBytebuffer` | 由 suite 按 mode 写入 | 全局静态缓冲池 |

**生产建议（当前结论）**：

- 使用 **`LD_PRELOAD=libjemalloc.so.2`** 或永久链接系统 jemalloc 做全局分配器。
- jemalloc 下可忽略 glibc 专用调参；列向量池默认关闭亦可。

---

## 6. 运行命令

工作目录均为：

```bash
cd /home/whz/test/pixels/cpp/testcase/buffer-perf-concurrency-suite
```

二进制默认：`../../build/release/duckdb`
属性文件默认：`~/opt/pixels/etc/pixels-cpp.properties`

若 `results/` 目录属 root 无写权限，请设置 `RESULT_ROOT=/tmp/pixels_verify`（或任意可写目录）。

### 6.1 编译

```bash
cd /home/whz/test/pixels/cpp

# Release（与多数 benchmark 一致）
make release

# 若 on-CPU 火焰图 [unknown] 过多，改用带帧指针的构建
make relwithdebinfo GEN=ninja
```

### 6.2 仅墙钟：线程扩展曲线（无 perf）

**glibc 基线**：

```bash
cd testcase/buffer-perf-concurrency-suite

unset LD_PRELOAD
RESULT_ROOT=/tmp/pixels_verify \
RUN_PERF_ONCPU=0 RUN_PERF_STAT=0 RUN_OFFCPU=0 \
QUERIES=q45 SSD_MODES=24ssd \
BUFFER_MODES="singlebuffer doublebuffer" \
THREAD_LIST="12 24 48" \
SUITE_TAG=glibc_scale \
./run_suite.sh
```

**jemalloc**：

```bash
export LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2

RESULT_ROOT=/tmp/pixels_verify \
RUN_PERF_ONCPU=0 RUN_PERF_STAT=0 RUN_OFFCPU=0 \
QUERIES=q45 SSD_MODES=24ssd \
BUFFER_MODES="singlebuffer doublebuffer" \
THREAD_LIST="12 24 48" \
SUITE_TAG=jemalloc_scale \
./run_suite.sh

unset LD_PRELOAD
```

汇总 CSV：`$RESULT_ROOT/<SUITE_TAG>/summary_wall_time.csv`

### 6.3 墙钟：t48 重复三次（稳定性对比）

```bash
# glibc
unset LD_PRELOAD
RESULT_ROOT=/tmp/pixels_verify RUN_PERF_ONCPU=0 RUN_PERF_STAT=0 RUN_OFFCPU=0 \
QUERIES=q45 SSD_MODES=24ssd BUFFER_MODES="singlebuffer doublebuffer" \
THREAD_LIST="48 48 48" SUITE_TAG=glibc_t48x3 ./run_suite.sh

# jemalloc
export LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2
RESULT_ROOT=/tmp/pixels_verify RUN_PERF_ONCPU=0 RUN_PERF_STAT=0 RUN_OFFCPU=0 \
QUERIES=q45 SSD_MODES=24ssd BUFFER_MODES="singlebuffer doublebuffer" \
THREAD_LIST="48 48 48" SUITE_TAG=jemalloc_t48x3 ./run_suite.sh
unset LD_PRELOAD
```

### 6.4 完整套件：on-CPU + perf stat + off-CPU（需 root）

**glibc**：

```bash
sudo -E env \
  RESULT_ROOT=/tmp/pixels_verify \
  QUERIES=q45 SSD_MODES=24ssd \
  BUFFER_MODES="singlebuffer doublebuffer" \
  THREAD_LIST="12 24 48" \
  RUN_PERF_ONCPU=1 RUN_PERF_STAT=1 RUN_OFFCPU=1 \
  SUITE_TAG=glibc_full \
  ./run_suite.sh
```

**jemalloc**：

```bash
sudo -E env \
  LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2 \
  RESULT_ROOT=/tmp/pixels_verify \
  QUERIES=q45 SSD_MODES=24ssd \
  BUFFER_MODES="singlebuffer doublebuffer" \
  THREAD_LIST="12 24 48" \
  RUN_PERF_ONCPU=1 RUN_PERF_STAT=1 RUN_OFFCPU=1 \
  SUITE_TAG=jemalloc_full \
  ./run_suite.sh
```

### 6.5 仅 off-CPU（最快验证 mprotect/sysmalloc 是否消失）

**doublebuffer t48，glibc vs jemalloc 各跑一次**：

```bash
# glibc
sudo -E env RESULT_ROOT=/tmp/pixels_verify \
  QUERIES=q45 SSD_MODES=24ssd BUFFER_MODES=doublebuffer THREAD_LIST=48 \
  RUN_PERF_ONCPU=0 RUN_PERF_STAT=0 RUN_OFFCPU=1 \
  SUITE_TAG=glibc_db_t48_offcpu ./run_suite.sh

# jemalloc
sudo -E env LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2 \
  RESULT_ROOT=/tmp/pixels_verify \
  QUERIES=q45 SSD_MODES=24ssd BUFFER_MODES=doublebuffer THREAD_LIST=48 \
  RUN_PERF_ONCPU=0 RUN_PERF_STAT=0 RUN_OFFCPU=1 \
  SUITE_TAG=jemalloc_db_t48_offcpu ./run_suite.sh
```

**输出**：`$RESULT_ROOT/<SUITE_TAG>/q45_24ssd_doublebuffer_t48/offcpu.svg`

对比时在 SVG 中搜索：`__GI___mprotect`、`sysmalloc`、`down_read`、`setRef`。

### 6.6 单次手工跑查询（带 jemalloc）

```bash
export LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2

/home/whz/test/pixels/cpp/build/release/duckdb -c "
  LOAD 'pixels';
  -- 按你的 benchmark 建 view 后执行 q45
"
```

### 6.7 列向量池开关 A/B（可选）

```bash
# 关闭池（追加到运行时 properties，跑完记得恢复）
echo 'pixels.columnvector.pool=false' >> ~/opt/pixels/etc/pixels-cpp.properties

# 跑 suite 后对比 summary_wall_time.csv

# 删除该行或从备份恢复 properties
```

### 6.8 权限与常见问题

```bash
# perf 权限
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid

# 验证 jemalloc 能否加载
LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2 \
  build/release/duckdb -c "select 1;"

# BCC libbcc 版本问题（见 README）
sudo ln -sf /usr/lib/x86_64-linux-gnu/libbcc.so.0.18.0 /usr/lib/x86_64-linux-gnu/libbcc.so.0
```

---

## 7. 结果目录与归档

| 路径 | 内容 |
|------|------|
| `results/20260615_084721/` | 含 off-CPU/on-CPU 的完整 glibc 套件（buffer-perf 正式跑） |
| `/tmp/pixels_verify/glibc_scale/` | glibc 线程扩展墙钟 |
| `/tmp/pixels_verify/jemalloc_scale/` | jemalloc 线程扩展墙钟 |
| `/tmp/pixels_verify/glibc/`、`jemalloc/` | t48×3 稳定性对比 |

每个 case 子目录典型文件：

- `duckdb.log` — 墙钟（`Run Time (s): real`）
- `perf_stat.txt` — CPU 利用率、page-faults 等
- `*_oncpu_cpu-clock.svg` — on-CPU 火焰图
- `offcpu.svg` — off-CPU 火焰图

---

## 8. 后续建议

1. **短期**：部署/脚本中固定 `LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2`，或 RPATH 链接系统 jemalloc 做全局 interposition（注意与 DuckDB 内置前缀 jemalloc 共存无冲突，因后者不导出 `malloc`）。
2. **中期**：CMake 正式启用对 `libjemalloc` 的链接（取消注释并确保最终 `duckdb` 可 interpose），或 `-DOVERRIDE_NEW_DELETE=TRUE` 仅覆盖 C++ new/delete（仍建议配合系统 jemalloc 处理 `posix_memalign`）。
3. **验证**：用 §6.5 的 off-CPU 对比确认 jemalloc 下 `mprotect`/`sysmalloc` 占比下降；用 §6.2 线程曲线确认 doublebuffer 在全并发段优于 singlebuffer。
4. **不必再投入**：默认收紧 glibc arena、依赖列向量池缩短 q45 墙钟（除非有新证据）。

---

## 9. 变更记录（代码侧）

| 变更 | 文件 | 状态 |
|------|------|------|
| 列向量缓冲池 | `pixels-core/.../ColumnVectorBufferPool.*`、`BinaryColumnVector.*` | 已实现；墙钟收益有限 |
| glibc mallopt + 可选 arena 上限 | `pixels-duckdb/pixels_extension.cpp` | 已实现；arena 默认 0 |
| 静态缓冲 MADV_HUGEPAGE | `GlobalStaticBufferPool.cpp` | 可选配置 |
| 配置项说明 | `pixels-cpp.properties` | 已文档化 |

---

*文档生成依据：2026-06-13 ~ 2026-06-15 在 q45/24ssd 上的 perf 套件、墙钟 A/B 与源码/二进制分析。*
