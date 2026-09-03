# PR7：DuckDB Pixels 并行扫描

本报告总结 PR7 相对于 Footer Cache 生命周期整理（`2a225ca6`）增加的内容。
原始测试日志和数据集不纳入 Git。

## 修改说明

- 加固 DuckDB bind/scan 路径中的并行扫描状态初始化、进度推进、文件结束和空存储处理。
- 保留按设备分配存储任务的调度逻辑，并使共享扫描状态适用于并行 worker。
- 增加可选的 worker CPU-Affinity 配置和解析测试；生产默认仍为关闭。
- 增加独立的 native io_uring 扫描基准程序，覆盖 non-fixed、dynamic、static 三种
  buffer 后端、shared/device-affine 两种调度、重复执行、返回码检查和自动发现数据根目录。
- 补充可复现的验证流程和结论；生成的结果目录仅保留在本地。

## 正确性测试

以下定向测试均通过（`PIXELS_SRC` 和 `PIXELS_HOME` 指向同一份宿主机 checkout）：

- `CPUAffinityTest`：3/3 通过
- `PixelsFooterCacheTest`：4/4 通过
- `ColumnVectorBufferPoolTest`：2/2 通过
- `DynamicBufferPoolTest`：3/3 通过
- `GlobalStaticBufferPoolTest`：1/1 通过
- `DirectUringNonFixedTest`：1/1 通过

完整 ClickBench 查询集（`q01`–`q43`）在 6 块 SSD、24 个 DuckDB 线程下执行一次，
43/43 成功，退出码均为 0；未观察到 SQE、队列满或 I/O 错误。Q24 另外在 24 块
SSD、48 线程下执行成功。

## 性能测试

Native 扫描基准读取 24 块 SSD 上的 3072 个文件（368.77 GB，十进制），每个配置
重复 3 次。正式运行中位耗时如下：

| 线程数 | Non-fixed | Dynamic | Static |
|---:|---:|---:|---:|
| 12 | 5.91 秒 | 5.63 秒 | 5.72 秒 |
| 24 | 3.72 秒 | 3.86 秒 | 4.06 秒 |
| 48 | 3.90 秒 | 4.10 秒 | 4.50 秒 |

24 线程、device-affine 调度下，中位耗时为 non-fixed 3.64 秒、dynamic 3.79 秒、
static 4.08 秒。所有基准执行返回 0。上述结果用于描述独立 I/O 路径，不宣称为
DuckDB 端到端查询的普遍加速。

端到端 Q24 在 24 块 SSD、48 线程下读取约 1.924 TB，用时 140.07 秒，约为
13.73 GB/s，进程常驻内存峰值约 74.1 GB。6 块 SSD 下 Q24 在 1、12、24、48
线程配置也均成功。

## 范围和后续工作

本 PR 不解决 Dynamic BufferPool 已知的性能退化问题，也不改变不支持的 96 线程配置，
并且不包含原始测试数据。
