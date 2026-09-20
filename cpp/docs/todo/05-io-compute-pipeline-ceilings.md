# I/O、Compute 与 Pipeline 性能边界

## 为什么做

当前 q24、24 SSD、12/24/48 worker 数据可以观察 wall time、累计阶段时间和设备吞吐，但还不能回答：

- 24块SSD对真实Pixels请求能提供多少极限吞吐；
- Pixels软件I/O路径能利用多少设备能力，又消耗多少CPU；
- Decode、Filter和Transform脱离设备等待后的计算极限是多少；
- I/O和计算同时运行时互相干扰多少；
- 完整流水线距离理论下界还有多少bubble、backpressure和调度损失。

在继续研究decode限流、shared poller、metadata异步化前，应先建立统一的性能边界和阶段资源需求模型。否则单个优化只能用wall time试错，无法估计收益上限，也无法判断瓶颈是否已经转移。

## 核心模型

需要分别测量：

```text
Device-only I/O ceiling
        ↓ 软件I/O效率
Pixels I/O-only ceiling
        ↓ 与计算的资源干扰
Compute-only ceiling
        ↓ overlap、queueing、backpressure
Full pipeline performance
```

对于固定工作量：

```text
T_ideal              = max(T_io_only, T_compute_only)
PipelineEfficiency   = T_ideal / T_pipeline
OverlapLoss          = T_pipeline - T_ideal
OverlapLossRatio     = T_pipeline / T_ideal - 1
```

不能将累计I/O thread-time与compute thread-time直接相加；不同线程和阶段可能重叠。`T_ideal`也是实验下界，不保证完整系统一定能够达到。

## 边界一：Device-only I/O

### 标准设备基线

使用fio建立24 SSD的基础能力曲线：

```text
pattern: sequential read, random read
block size: 4K, 64K, 256K, 实际chunk中位数
iodepth: 1, 4, 8, 16, 32, 64
jobs: 1, 12, 24, 48
```

报告最大GB/s、IOPS、平均和p95/p99延迟，以及吞吐开始进入平台期的queue depth。

### q24请求回放

fio不能代表真实Pixels请求。正常运行一次q24，记录：

```text
backend/device/file
offset
requested length
aligned length
submit/completion timestamp
worker/qpair/ring
```

回放器只提交相同I/O，不执行metadata解析、decode、filter或output。分别实现io_uring与SPDK回放，并保持原始磁盘、地址、对齐和请求大小分布。

核心指标：

```text
device_throughput       = completed_bytes / device_elapsed
device_speedup(N)       = throughput(N) / throughput(baseline)
device_scale_efficiency = speedup(N) / concurrency_scale
```

## 边界二：Pixels Software I/O-only

保留完整Pixels软件I/O路径，但在chunk ready后丢弃数据，不进入decode：

```text
metadata
buffer lookup/allocation
request construction
submit
device service
completion/poll
buffer release
```

由此区分设备能力和软件I/O能力：

```text
SoftwareIOEfficiency = Pixels I/O-only throughput / request-replay throughput
I/O CPU seconds/GB   = I/O task-clock / completed GB
Submit cycles/request
Poll cycles/completion
Metadata latency/file
```

io_uring必须记录submitted、completed、bytes和completion batch；SPDK还要记录poll iterations、DMA allocate/free和qpair queue depth。所有外层和内层标签应保留层级关系，聚合时只能选择互斥阶段。

## 边界三：Compute-only

正常读取一次q24的压缩column chunks，将其保存为内存中的只读replay数据。后续运行不提交设备I/O，直接执行：

```text
compressed chunks in memory
        ↓
Decode → Filter → Transform/Output
```

必须保留真实压缩数据、encoding、batch size、投影列、filter selectivity和输出行数。随机生成数据或仅依赖Linux page cache都会改变工作量；当前direct I/O路径也不会被page cache完整替代。

核心指标：

```text
rows/s
compressed GB/s
decoded GB/s
decode cycles/input byte
compute task-clock/input GB
compute speedup(N)
compute parallel efficiency(N)
```

同时采集IPC、cycles、instructions、LLC miss、DRAM read/write bandwidth、NUMA local/remote bytes和dTLB miss，用于区分指令吞吐、LLC容量、内存带宽与NUMA瓶颈。

## 边界四：完整 Pipeline

完整运行：

```text
Metadata → Submit → Device → Completion
                              ↓
                         Decode → Filter → Transform/Output
```

将完整pipeline与相同线程数、相同工作量的I/O-only和compute-only比较，计算`PipelineEfficiency`和`OverlapLoss`。额外记录每阶段begin/end低开销事件，构造并发时间线，识别：

- pipeline bubble；
- buffer unavailable/backpressure；
- metadata文件边界停顿；
- I/O completion与decode争抢CPU；
- DMA与column vector争抢内存带宽；
- 跨NUMA buffer访问。

## 阶段资源需求向量

阶段不能只标记为“I/O”或“计算”。每个阶段应输出一个资源向量：

| 阶段 | Device bytes | CPU cycles | Memory demand | Stall | Work unit |
|---|---:|---:|---:|---:|---|
| Metadata | 有 | 有 | 低 | 有 | file/footer |
| Buffer管理 | 无 | 有 | 高 | 可能 | buffer |
| Submit | 无 | 有 | 低 | 少 | request |
| WaitCQE | 有 | interrupt模式低 | 低 | 高 | completion |
| SPDK Poll | 有 | 高 | 低 | 高 | completion |
| Completion | 无 | 有 | 低 | 少 | completion |
| Decode | 无 | 高 | 很高 | 少 | input byte/row |
| Filter | 无 | 高 | 高 | 少 | input row |
| Transform/Output | 无 | 有 | 高 | 可能 | output row |

统一输出：

```text
stage_thread_s
stage_equivalent_concurrency = stage_thread_s / wall_s
cycles、instructions、task-clock
bytes、requests、batches、input/output rows
cycles/GB、CPU-s/GB、requests/s、rows/s
LLC misses/GB、DRAM bytes/GB、remote NUMA bytes/GB
```

硬件计数器难以直接按阶段归因时，使用阶段隔离运行，或采用低频采样/线程级perf并与阶段时间线关联，不能按wall占比机械拆分全进程cycles。

## I/O 与 Compute 相互干扰

隔离极限不能预测二者同时运行时的表现。增加以下矩阵：

```text
A. I/O-only
B. Compute-only
C. I/O-only + synthetic/recorded compute load
D. Compute-only + recorded I/O load
E. Full pipeline
```

派生指标：

```text
ComputeInterference = compute throughput under I/O / isolated compute throughput
IOInterference      = I/O throughput under compute / isolated I/O throughput
```

该实验用于量化double buffer的收益与代价：它可能增加overlap，同时让completion、poll、DMA和decode竞争CPU、LLC、DRAM及NUMA链路。

## 验证矩阵

第一阶段使用最小矩阵：

```text
backend: io_uring, SPDK
workload: request replay, I/O-only, compute-only, full pipeline
mode: singlebuffer, doublebuffer
threads: 12, 24, 48
repeat: 3
query: q24
ssd: 24
```

根据第一阶段曲线，在可能的knee point附近补充16、32、40线程。并发拐点定义为：继续增加并发后，吞吐提升不足5%，但延迟或单位工作量资源成本增加超过10%。

## 工作量一致性

每次运行必须验证：

```text
requested/completed bytes
request count和请求尺寸分布
metadata request count
batch count
decoded input rows
filter selectivity
output rows
校验结果
```

I/O-only允许跳过decode，但必须提交与完整q24相同的请求；compute-only允许跳过设备I/O，但必须消费相同的压缩chunks。否则吞吐变化可能仅代表少做了工作。

## Profiler 与正式性能数据

分开运行三类测量：

1. 低开销正式运行：wall、bytes、rows、request/batch counters，至少三次；
2. 阶段profiler运行：thread-time、begin/end事件和等价并发度；
3. 硬件运行：perf、内存带宽和NUMA计数器。

不能将同时启用所有重型profiler的wall time作为正式性能结论。不同运行必须验证工作量一致，并报告采集边界。

## 实施顺序

1. 补齐统一的bytes、request、batch、row和阶段计数器；
2. 实现q24真实请求trace与I/O replay；
3. 实现compressed chunk内存replay和compute-only入口；
4. 建立12/24/48线程的四类性能边界；
5. 增加阶段时间线和I/O/compute相互干扰实验；
6. 在拐点附近补16/32/40线程；
7. 根据结果再实施decode limit、shared poller、metadata async和NUMA优化。

## 验收

- I/O-only、compute-only和full pipeline的输入工作量可以逐项对账；
- 得到io_uring与SPDK真实q24请求的设备与软件I/O效率；
- 得到compute吞吐曲线、并行效率和硬件饱和原因；
- 每个阶段至少有CPU、I/O、内存、等待和工作量归一化指标；
- 能计算并解释`PipelineEfficiency`、`OverlapLoss`和资源干扰系数；
- wall结果至少重复三次并报告中位数、最小值及离散程度；
- 后续每项优化均能对应一个已测量的瓶颈和收益上限。
