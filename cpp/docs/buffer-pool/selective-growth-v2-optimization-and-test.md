# Selective Growth V2 与 Static 无锁查询：设计及全量测试说明

日期：2026-09-13

本轮改动包含四项可独立开关的 Selective 能力和一项 Static 优化。所有优化默认关闭，旧配置
保持 V1 行为。队列指标是观测功能，不作为性能优化；全量实验专门保留 metrics off/on 对照。

## 配置开关

| 能力 | 配置 | 默认值 | 全量实验值 |
|---|---|---:|---:|
| Selective 总开关 | `pixels.dynamic.selective.enabled` | `false` | 按模式 |
| 固定队列上限 | `pixels.dynamic.selective.queue.limit` | `1` | `4` |
| Growth-pressure gate | `pixels.dynamic.selective.growth_gate.enabled` | `false` | `true` |
| 最小可避免增长字节 | `pixels.dynamic.selective.growth_gate.min_bytes` | `1` | `4194304` |
| 要求该列已有真实增长历史 | `pixels.dynamic.selective.growth_gate.require_history` | `true` | `true` |
| 接收者成本模型 | `pixels.dynamic.selective.cost.enabled` | `false` | `true` |
| 队列长度权重 | `pixels.dynamic.selective.cost.queue_weight` | `1.0` | `1.0` |
| 已排队字节权重 | `pixels.dynamic.selective.cost.queued_bytes_weight` | `1.0` | `1.0` |
| 累计执行负载权重 | `pixels.dynamic.selective.cost.load_weight` | `0.25` | `0.25` |
| Buffer 容量浪费权重 | `pixels.dynamic.selective.cost.slack_weight` | `0.01` | `0.01` |
| 自适应队列 | `pixels.dynamic.selective.adaptive.enabled` | `false` | `true` |
| 自适应最小/最大队列 | `pixels.dynamic.selective.adaptive.queue_min/max` | `0/4` | `0/4` |
| 增长压力低/高阈值 | `pixels.dynamic.selective.adaptive.pressure_low/high` | `0.10/0.30` | 同默认 |
| EWMA 窗口 | `pixels.dynamic.selective.adaptive.window` | `64` | `64` |
| 事件指标 | `pixels.dynamic.selective.metrics.enabled` | `false` | V2 Selective 模式为 `true` |
| Static 无锁查询 | `pixels.static.buffer.lock_free_lookup` | `false` | 按模式 |

Growth gate 只在当前 worker 已有该列 Buffer、容量确实不足、预计可避免增长不少于阈值，并且
该列已经在本查询中发生过一次真实增长时允许转移。这样保证第一个遇到增长的线程先建立容量，
后续线程才能复用它，并阻止 q42 一类只有首次分配、没有增长的查询进行无效转移。

成本模型只在“同 storage group、两套 Buffer 均满足、队列未满”的候选中比较：

```text
cost = 1.00 × queue_depth
     + 1.00 × queued_demand_bytes / current_file_demand_bytes
     + 0.25 × worker_executed_bytes / group_average_executed_bytes
     + 0.01 × capacity_slack_bytes / current_file_demand_bytes
```

自适应队列使用“当前容量已存在但不足”的事件更新 EWMA。低于低阈值时队列为 0，处于低/高
阈值之间时为 1，高于高阈值时为 4。它不把首次分配当作增长压力。

Static 的无锁配置只改变 `GlobalStaticBufferPool::GetBuffer()`。初始化仍在 `poolMutex_` 保护下；
初始化完成后容器不再修改，每个 threadId 访问自己的 Buffer，因此查询路径可以进行无锁 const
查找。旧 mutex 路径保留用于同二进制 A/B。

## 指标及可视化

指标在 query-global scan lock 下写入内存，查询销毁时一次性输出，不在热路径执行文件 I/O。
环境变量 `PIXELS_SELECTIVE_METRICS_PREFIX` 指定文件前缀：

- `<prefix>.events.csv`：事件时间序列；
- `<prefix>.workers.csv`：逐 worker 查询汇总。

关键事件包括 `buffer_publish`、`enqueue`、`dequeue`、`execute`、`gate_reject`、
`queue_reject`、`no_receiver` 和 `queue_limit`。事件表可以恢复：

- 每个 worker 两套 Buffer 总容量随时间的变化；
- 每个接收者队列中等待文件的总 ColumnChunk demand；
- 每个转移文件从 enqueue 到 dequeue 的真实等待时间；
- 自适应 queue limit 和 growth-pressure EWMA；
- 每个 worker 执行的文件数及总 demand bytes。

绘图脚本：

```bash
MPLCONFIGDIR=/tmp/pixels-matplotlib \
python3 testcase/buffer-pool/plot_selective_v2_metrics.py \
  --results /absolute/path/to/selective-v2-results \
  --output /absolute/path/to/selective-v2-results/figures \
  --modes selective-base selective-gate selective-gate-cost selective-all
```

脚本生成两类图：

1. `selective-v2-comparison.{png,svg}`：44 查询 × 模式的三联热力图，依次展示逐 worker 执行
   demand 的变异系数、P95 排队等待和峰值排队 demand；用于判断负载不均与排队是否改善。
2. `<query>-<mode>-r0-telemetry.{png,svg}`：单查询四联图，包括 worker Buffer 容量热力图、
   队列总 demand 热力图（空心圆大小表示单个入队文件 demand）、排队等待 ECDF，以及逐 worker
   执行字节/文件数。队列图内嵌自适应 queue limit 时间线。
3. `selective-v2-performance.{png,svg}`：九种模式的逐查询耗时/Dynamic 和 RSS 双热力图，直接
   展示每一步消融是否改善时间以及是否以更多内存为代价。

同时输出 `selective-v2-metrics-summary.csv` 和 `selective-v2-performance-summary.csv`，便于计算
CV、P50/P95/max wait、max/min load、峰值排队字节、adaptive limit changes 及查询时间中位数。

## 44 查询 × 16 SSD 全量实验

全量脚本覆盖 q00–q43，共 44 条查询、16 个 SSD、48 个线程、3 次普通重复，并为每个
query/mode 额外执行一次 `perf stat` 和一次 `perf record`/火焰图：

```bash
testcase/buffer-pool/run_selective_v2_full_16ssd.sh \
  /home/whz/test/pixels/cpp/testcase/buffer-pool/results/selective-v2-16ssd-full \
  /home/whz/test/pixels/cpp/pixels-cpp.properties \
  /home/whz/FlameGraph
```

省略第一个参数时，脚本自动生成不含换行的时间戳目录。若显式指定的输出目录已存在，包装脚本
自动添加 `--resume`：只跳过已写入 `summary.csv` 且 timed/perf/火焰图等必需产物完整的 case；
若旧 summary 中已有 checkpoint 但产物为空或缺失，脚本会输出 `[redo]`、原子地移除该旧记录并
重新执行；失败或尚未开始的 case 也会继续执行。续跑参数、输入文件和 SQL 必须与原 manifest 一致，新的二进制
哈希会追加到 `manifest.json` 的 `resume_events`，以便识别跨构建续跑。

例如，中断后使用完全相同的命令即可续跑：

```bash
testcase/buffer-pool/run_selective_v2_full_16ssd.sh \
  /home/whz/test/pixels/cpp/testcase/buffer-pool/results/selective-v2-16ssd-full-20260913-005325 \
  /home/whz/test/pixels/cpp/pixels-cpp.properties \
  /home/whz/FlameGraph
```

实验包含九种模式：

| 模式 | 目的 |
|---|---|
| `legacy` | 当前低内存高性能基线 |
| `dynamic` | 无 metadata/任务转移的 Dynamic 基线 |
| `selective-4` | V1 queue=4，metrics off |
| `selective-base` | 与上一项相同策略，metrics on；测量指标开销 |
| `selective-gate` | base + growth gate |
| `selective-gate-cost` | gate + 接收者成本模型 |
| `selective-all` | gate + cost + adaptive queue |
| `static-locked` | Static 旧 mutex 查询路径 |
| `static-lockfree` | Static 无锁查询路径 |

成本模型和自适应队列也可分别单独运行：

```bash
python3 testcase/buffer-pool/run_selective_benchmark.py \
  --properties pixels-cpp.properties \
  --column-sizes /home/whz/pixels/clickbench-size-e0.csv \
  --ssds 16 --threads 48 --repeat 3 --query-set full \
  --modes selective-base selective-cost selective-adaptive \
  --output /absolute/new/output/path
```

主脚本共有 44 × 9 × 3 = 1,188 条普通计时/RSS 记录、396 份 perf stat、396 份 perf.data
和火焰图，总计启动 1,980 个查询进程。四个 metrics-on Selective 模式还会产生 880 对事件/worker
CSV（普通重复、stat 和 record 使用不同前缀，互不覆盖）。运行结束以根目录 `SUCCESS` 为准。

## 验收标准

1. `selective-4` 与 `selective-base` 的时间差用于量化指标开销，不能把 metrics-on 结果直接与
   V1 报告混比。
2. Gate 应显著降低 q01/q02/q07/q08/q30/q41/q42/q43 的 transferred 数，尤其 q42 应接近 0；
   高增长查询的增长次数或峰值 Buffer 不能明显恶化。
3. Cost 模型应降低 q25/q27 的 executed-byte CV、max/min load 或 P95 queue wait；如果只降低
   CV 却增加时间，需要检查 storage/NUMA locality。
4. Adaptive 应在无增长查询保持 queue limit=0，在 q21/q22/q23/q28/q37/q39/q40 中升至 1/4；
   其性能应不差于 gate+cost，同时降低固定 queue=4 的尾部回退。
5. `static-lockfree` 和 `static-locked` 的查询结果及 Buffer/RSS 应一致；无锁版本应降低短窄查询
   的查询时间。整进程 perf 仍包含 Static 初始化，必须结合 EXPLAIN ANALYZE query time 判断。
6. 优先复核收益组 q21/q22/q23/q28/q37/q39/q40，风险组 q25/q27/q42，高内存组 q24，
   以及零增长对照 q30，再汇总全部 44 查询。
