# Parquet io_uring 读取与双缓冲

## 实现目标
parquet实现基于io_uring的async I/O机制,探究parquet异步读取的性能以及是否能从double-buffer的设计中受益


## 实现方法
 - 将其作为duckdb的插件
 - 内存分配复用pixels提供的bufferpool
 - io_uring接口复用pixels的实现
 - 以上两项保证了parquet 可以应用~/opt/pixels/etc/pixels-cpp.properties中关于异步/同步I/O 关于单双buffer的相关设置
 - 函数名称为read_parquet_uring 千万不要动pixels项目本身的任何代码,如果不得不改,一定要在实现中写清楚
 - 编译方法 沿用pixels相同的编译方法 直接在项目目录下make -j即可编译

## 测试方法
- 功能性测试 可以跑通clickbench 44条查询 同时需要提供相关的测试脚本
- 性能测试通过 `testcase/performance-test/run_suite_parquet_uring.sh` 运行，并输出 perf、iostat 和火焰图结果。
- 默认测试组合为 ClickBench `q24`、`24ssd`、`48` 线程，并依次运行
  `pq-async-doublebuffer`、`pq-async-singlebuffer` 和 `pq-pread`。运行命令：

```bash
cd cpp
sudo -E bash testcase/performance-test/run_suite_parquet_uring.sh
```

运行前应确认 `/data/9a3-01` 到 `/data/9a3-24` 均已挂载，且每块盘上存在
`clickbench/parquet-e0/hits/*.parquet`。对应数据集定义在
`testcase/benchmark.json` 的 `clickbench-parquet-uring-e0-24ssd` 项中。

## 阶段耗时分析

在 `pixels-cpp.properties` 中启用：

```properties
pixel.enable.profiler=true
```

查询结束后会输出两张累计 thread-time 汇总表：

- `Parquet Pipeline Profile Summary`：扫描总时间、RecordBatch 解码、Arrow 导出、Arrow→DuckDB 转换和文件切换。
- `Parquet I/O and Initialization Profile Summary`：io_uring 提交/等待、pread、Arrow Reader 构造、缓冲区分配和 io_uring 初始化。

`thread_time_s` 是所有扫描线程的累计时间，不是墙钟时间；`base_ratio_pct` 是相对该表 `base_total` 的比例。双缓冲模式下，`Parquet.IO.Wait` 只表示切换文件时仍未被后台预取覆盖的剩余等待时间。
