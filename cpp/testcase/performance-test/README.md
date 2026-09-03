# buffer-perf-concurrency-suite 使用指南

对 Pixels 存储引擎的多种 buffer 模式进行系统性性能分析，输出：

- **on-CPU 火焰图**（`perf record -e cpu-clock`）
- **perf stat** 硬件计数器统计
- **off-CPU 火焰图**（BCC `offcputime`，需 root）
- **汇总 CSV**（各组合的查询耗时）

> **性能调查结论文档**（doublebuffer 回归、jemalloc vs glibc、火焰图判读、完整运行命令）：见 [PERFORMANCE_ANALYSIS.md](./PERFORMANCE_ANALYSIS.md)

分配器可通过 `ALLOCATOR=glibc`（默认）或 `ALLOCATOR=jemalloc` 选择。jemalloc
模式会对整个 DuckDB 进程设置 `LD_PRELOAD`；这与 DuckDB 内置的 `duckdb_je_*`
前缀分配器不同，后者不会自动接管 Pixels 扩展中的 libc 分配。

---

## 目录结构

```
performance-test/
├── run_suite.sh          # 主入口
├── lib_suite_common.sh   # 共享函数（buffer 配置、临时 DB 管理）
├── README.md             # 本文档
└── results/
    └── <SUITE_TAG>/
        ├── RUN_META.txt
        ├── summary_wall_time.csv
        └── <query>_<ssd>_<mode>_t<N>/
            ├── duckdb.log
            ├── perf_stat.txt
            ├── *_oncpu_cpu-clock.svg
            ├── *_oncpu_cpu-clock.data
            └── offcpu.svg
```

---

## 前置条件

| 依赖 | 安装方式 |
|------|---------|
| `perf` | `apt install linux-tools-$(uname -r)` |
| FlameGraph | 项目子模块：`git submodule update --init cpp/third-party/FlameGraph` |
| BCC offcputime | `apt install bpfcc-tools python3-bpfcc`（off-CPU 可选） |
| DuckDB + Pixels | `make relwithdebinfo GEN=ninja`（需帧指针，否则火焰图有 unknown） |
| `pixels-cpp.properties` | `~/opt/pixels/etc/pixels-cpp.properties` |

> **帧指针说明**：on-CPU 火焰图默认使用 `--call-graph=fp`，需要二进制以 `RelWithDebInfo` 或带 `-fno-omit-frame-pointer` 编译。若用 `Release` 构建，改为 `PERF_CALLGRAPH=dwarf`（更慢但无需帧指针）。

---

## 快速开始

```bash
cd testcase/performance-test

# glibc 基线
ALLOCATOR=glibc ./run_suite.sh

# jemalloc 对照（可通过 JEMALLOC_LIB 指定库路径）
ALLOCATOR=jemalloc ./run_suite.sh

# 最小测试：q45，24ssd，singlebuffer vs doublebuffer，48线程，仅 perf stat
BUFFER_MODES="singlebuffer doublebuffer" \
THREAD_LIST="48" \
RUN_PERF_ONCPU=0 RUN_OFFCPU=0 \
./run_suite.sh

# 完整测试（需 root 跑 off-CPU）
sudo -E ./run_suite.sh
```

---

## 环境变量参考

### 测试矩阵

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `QUERY` | `q45` | 单个查询，被 `QUERIES` 覆盖时忽略 |
| `QUERIES` | `q45` | 空格分隔的查询列表，如 `"q01 q24 q45"`；`all` 自动发现所有 q* |
| `SQL_DIR` | `clickbench/queries-test` | 优先搜索目录（q45 等扩展查询） |
| `SQL_DIR_EXTRA` | `clickbench/queries` | 次级搜索目录（q01-q43） |
| `SSD_MODES` | `24ssd` | 空格分隔的 SSD 数量，如 `"1ssd 6ssd 24ssd"` |
| `BUFFER_MODES` | 全部 5 种 | 见下方 buffer mode 说明 |
| `THREAD_LIST` | `1 8 16 24 48` | 空格分隔的线程数列表 |

### Buffer Mode 说明

与 `run_perf_four_modes.py` 的 `TEST_SCENARIOS` 完全对应：

| Mode | async I/O | fixed buffer | global static | double-buffer |
|------|-----------|--------------|---------------|---------------|
| `singlebuffer`（= `fixed`） | ✓ io_uring | ✓ | ✓ | ✗ |
| `nonfixed` | ✓ io_uring | ✗ | ✓ | ✗ |
| `pread-singlebuffer` | ✗ pread | ✗ | ✗ | ✗ |
| `pread-doublebuffer` | ✗ pread | ✗ | ✗ | ✓ |
| `doublebuffer` | ✓ io_uring | ✓ | ✓ | ✓ |

### 路径配置

| 变量 | 默认值 |
|------|--------|
| `DUCKDB_BINARY` | `../../build/release/duckdb` |
| `SQL_DIR` | `../../pixels-duckdb/duckdb/benchmark/clickbench/queries-test` |
| `BENCHMARK_JSON` | `../benchmark.json` |
| `BENCHMARK_PREFIX` | `clickbench-pixels-e0` |
| `PROPERTIES_PATH` | `~/opt/pixels/etc/pixels-cpp.properties` |
| `FLAMEGRAPH_DIR` | 自动搜索项目内 `third-party/FlameGraph`，其次搜索 `~/FlameGraph` |
| `RESULT_ROOT` | `./results` |
| `SUITE_TAG` | 时间戳，如 `20260506_143000` |

### 采集开关

| 变量 | 默认 | 说明 |
|------|------|------|
| `RUN_PERF_ONCPU` | `1` | on-CPU 火焰图（`perf record`） |
| `RUN_PERF_STAT` | `1` | 硬件计数器统计（`perf stat`） |
| `RUN_OFFCPU` | `1` | off-CPU 火焰图（BCC，需 root） |

### perf 参数

| 变量 | 默认 | 说明 |
|------|------|------|
| `PERF_RECORD_FREQ` | `99` | 采样频率（Hz） |
| `PERF_CALLGRAPH` | `fp` | 栈展开方式：`fp`（帧指针）或 `dwarf` |
| `PERF_STAT_REPEAT` | `2` | `perf stat -r N` 重复次数 |
| `PERF_NO_SUDO` | `0` | 设为 `1` 时 perf 不加 sudo |

### off-CPU 参数

| 变量 | 默认 | 说明 |
|------|------|------|
| `OFFCPU_STACK_SIZE` | `32768` | BCC stack storage 大小 |
| `MIN_BLOCK_US` | `1` | 最短阻塞时间过滤（微秒） |
| `OFFCPU_RELAX_KPTR` | `1` | 临时放松 `kptr_restrict` 以解析内核符号 |

---

## 典型用法

### 1. 只对比 singlebuffer vs doublebuffer，48 线程

```bash
BUFFER_MODES="singlebuffer doublebuffer" \
THREAD_LIST="48" \
./run_suite.sh
```

### 2. 多 SSD 规模对比

```bash
SSD_MODES="1ssd 6ssd 24ssd" \
BUFFER_MODES="singlebuffer doublebuffer" \
THREAD_LIST="48" \
RUN_OFFCPU=0 \
./run_suite.sh
```

### 3. 多查询扫描

```bash
# 指定几个查询
QUERIES="q01 q24 q33 q45" \
BUFFER_MODES="singlebuffer doublebuffer" \
THREAD_LIST="24 48" \
./run_suite.sh

# 跑全部 q01-q43 + q45
QUERIES=all \
BUFFER_MODES="singlebuffer doublebuffer" \
THREAD_LIST="48" \
RUN_PERF_ONCPU=0 RUN_OFFCPU=0 \
./run_suite.sh
```

### 4. 全量五种 buffer mode，含 off-CPU（需 root）

```bash
sudo -E env \
  QUERIES="q45" \
  SSD_MODES="24ssd" \
  THREAD_LIST="12 24 48" \
  ./run_suite.sh
```

### 5. 只跑 perf stat，不跑火焰图（快速基准）

```bash
RUN_PERF_ONCPU=0 RUN_OFFCPU=0 \
BUFFER_MODES="pread-singlebuffer singlebuffer doublebuffer" \
THREAD_LIST="48" \
./run_suite.sh
```

### 6. 使用 DWARF 展开（Release 构建无帧指针时）

```bash
PERF_CALLGRAPH=dwarf \
BUFFER_MODES="singlebuffer doublebuffer" \
./run_suite.sh
```

### 7. 指定结果目录和标签

```bash
RESULT_ROOT=/data/perf_results \
SUITE_TAG=exp_doublebuffer_v2 \
./run_suite.sh
```

---

## 结果说明

### 目录命名

每个测试组合对应一个子目录：

```
<query>_<ssd_mode>_<buffer_mode>_t<threads>/
```

例如：`q45_24ssd_doublebuffer_t48/`

### 文件说明

| 文件 | 内容 |
|------|------|
| `duckdb.log` | 基准跑的 DuckDB 输出，含 `Run Time (s): real X.XXX` |
| `perf_stat.txt` | `perf stat` 硬件计数器，含 cycles、instructions、cache-misses 等 |
| `*_oncpu_cpu-clock.svg` | on-CPU 火焰图，热色系，单位 samples |
| `*_oncpu_cpu-clock.data` | 原始 perf 数据，可用 `perf report -i` 交互分析 |
| `offcpu.svg` | off-CPU 火焰图，蓝色系，单位 microseconds |
| `offcputime.err` | offcputime 错误日志（off-CPU 失败时查看） |

### summary_wall_time.csv

```
query,ssd_mode,mode,threads,wall_time_s,note
q45,24ssd,singlebuffer,48,20.771,duckdb_log
q45,24ssd,doublebuffer,48,31.804,duckdb_log
```

### RUN_META.txt

记录本次运行的完整参数，便于复现。

---

## 耗时估算

每个组合需要启动 DuckDB 最多 4 次（基准 + on-CPU + perf stat + off-CPU）。

```
总组合数 = |QUERIES| × |SSD_MODES| × |BUFFER_MODES| × |THREAD_LIST|
每组合耗时 ≈ 单次查询时间 × 4
```

例如 q45 在 24ssd 48线程下单次约 25s，5种 buffer × 5种线程 = 25 组合，总耗时约 **50 分钟**。

建议先用 `THREAD_LIST="48" BUFFER_MODES="singlebuffer doublebuffer"` 试跑验证环境。

---

## 常见问题

**Q: off-CPU 火焰图全是 `do_futex`，占 90%+**

正常现象。DuckDB 线程池中大多数线程在空闲等待任务（`TaskScheduler::ExecuteForever` → futex）。真正有意义的阻塞在剩余的 <10% 里。若查询是 CPU 密集型，on-CPU 火焰图更有参考价值。

**Q: on-CPU 火焰图有大量 `[unknown]`**

二进制缺少帧指针。用 `RelWithDebInfo` 重新构建：
```bash
make relwithdebinfo GEN=ninja
```
或改用 DWARF 展开：`PERF_CALLGRAPH=dwarf`（较慢）。

**Q: off-CPU 报 `cannot attach kprobe`**

`finish_task_switch` 符号名在内核版本间有变化。脚本使用的 `testcase/offcputime_patched.py` 已处理此问题（正则匹配 `finish_task_switch.isra.N`）。若仍失败，检查该文件是否存在且可执行。

**Q: `perf record` 报权限错误**

```bash
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
```
或直接用 `sudo -E ./run_suite.sh`。

**Q: BCC 报 `undefined symbol: bpf_module_create_b`**

libbcc 版本不匹配（系统同时存在 0.18.0 和 0.32.0）。修复软链接：
```bash
sudo ln -sf /usr/lib/x86_64-linux-gnu/libbcc.so.0.18.0 /usr/lib/x86_64-linux-gnu/libbcc.so.0
sudo ln -sf /lib/x86_64-linux-gnu/libbcc.so.0.18.0 /lib/x86_64-linux-gnu/libbcc.so.0
```

**Q: 磁盘空间不足**

`.data` 文件较大（每个 on-CPU 约 100-500MB）。不需要 `perf report` 时可批量删除：
```bash
find results/ -name '*.data' -delete
```
