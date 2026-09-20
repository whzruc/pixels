# CPU Affinity (线程绑核) 功能说明

## 概述

CPU Affinity（线程绑核）功能允许将 PixelsScan 的工作线程绑定到特定的 CPU 核心上，从而提高缓存局部性、减少上下文切换开销，并提升整体查询性能。

## 功能特性

- **多种绑核策略**：支持 round-robin、device-based 和 custom 三种绑核策略
- **灵活配置**：通过配置文件轻松启用/禁用和调整绑核行为
- **自动检测**：自动检测系统可用的 CPU 核心数
- **线程安全**：在多线程环境下安全运行
- **详细日志**：提供线程绑核状态的详细输出

## 配置参数

在配置文件（如 `pixels-cpp.properties` 或 `config.properties`）中添加以下参数：

### 基本配置

```properties
# 启用 CPU 亲和性绑定
pixels.enable.cpu.affinity=true

# 绑核策略（可选值：round-robin, device-based, custom）
pixels.cpu.affinity.strategy=round-robin
```

### 绑核策略说明

#### 1. Round-Robin 策略（默认）

将线程均匀分布到所有可用的 CPU 核心上。

```properties
pixels.cpu.affinity.strategy=round-robin
```

**适用场景**：
- 通用场景，适合大多数工作负载
- CPU 核心性能均衡的系统
- 希望充分利用所有 CPU 核心

**示例**：
- 系统有 24 个 CPU 核心
- 线程 0 绑定到核心 0
- 线程 1 绑定到核心 1
- ...
- 线程 24 绑定到核心 0（循环）

#### 2. Device-Based 策略

基于设备 ID（deviceID）进行绑核，适合多 SSD 阵列场景。

```properties
pixels.cpu.affinity.strategy=device-based
```

**适用场景**：
- 使用多个 SSD 设备的存储阵列
- 希望将处理特定设备数据的线程绑定到特定核心
- NUMA 架构系统，希望优化设备-CPU 的亲和性

#### 3. Custom 策略

自定义核心映射，完全控制线程到核心的绑定关系。

```properties
pixels.cpu.affinity.strategy=custom
# 指定核心列表（逗号分隔）
pixels.cpu.affinity.core.mapping=0,2,4,6,8,10,12,14,16,18,20,22
```

**适用场景**：
- 需要精确控制线程绑定
- 避开特定的 CPU 核心（如预留给其他任务）
- NUMA 架构下的精细调优
- 只使用物理核心，避开超线程核心

**示例配置**：

```properties
# 只使用偶数核心（物理核心）
pixels.cpu.affinity.core.mapping=0,2,4,6,8,10,12,14,16,18,20,22

# 只使用特定 NUMA 节点的核心
pixels.cpu.affinity.core.mapping=0,1,2,3,4,5,6,7,8,9,10,11

# 预留核心 0-3 给系统，使用核心 4-23
pixels.cpu.affinity.core.mapping=4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23
```

## 使用示例

### 示例 1：基本启用

```properties
# pixels-cpp.properties
pixels.enable.cpu.affinity=true
pixels.cpu.affinity.strategy=round-robin
```

### 示例 2：多 SSD 阵列优化

```properties
# 24 个 SSD，24 个线程
pixel.threads=24
pixels.enable.cpu.affinity=true
pixels.cpu.affinity.strategy=device-based
```

### 示例 3：NUMA 优化配置

```properties
# 假设系统有 2 个 NUMA 节点，每个节点 12 个核心
# 只使用 NUMA 节点 0 的核心
pixels.enable.cpu.affinity=true
pixels.cpu.affinity.strategy=custom
pixels.cpu.affinity.core.mapping=0,1,2,3,4,5,6,7,8,9,10,11
```

### 示例 4：避开超线程

```properties
# 24 核心 48 线程系统，只使用物理核心
pixels.enable.cpu.affinity=true
pixels.cpu.affinity.strategy=custom
pixels.cpu.affinity.core.mapping=0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46
```

## 运行时输出

启用 CPU 亲和性后，系统会输出线程绑核信息：

```
Thread 0 bound to CPU core 0
Thread 1 bound to CPU core 1
Thread 2 bound to CPU core 2
...
```

如果绑核失败，会输出警告信息：

```
Warning: Failed to bind thread to CPU core 5
```

## 验证绑核效果

### 方法 1：使用 taskset 命令

```bash
# 查看进程的 CPU 亲和性
taskset -cp <pid>
```

### 方法 2：使用 htop

```bash
htop
# 按 F2 进入设置
# 选择 "Display options"
# 启用 "Show custom thread names"
# 观察线程在哪些 CPU 核心上运行
```

### 方法 3：使用 perf

```bash
# 监控 CPU 迁移次数
perf stat -e sched:sched_migrate_task -p <pid>
```

绑核成功后，CPU 迁移次数应该显著减少。

## 性能调优建议

### 1. 确定最佳策略

不同的工作负载可能适合不同的绑核策略：

- **CPU 密集型查询**：使用 round-robin 或 custom 策略，充分利用所有核心
- **I/O 密集型查询**：使用 device-based 策略，优化设备-CPU 亲和性
- **混合负载**：先使用 round-robin，然后根据性能分析结果调整

### 2. NUMA 系统优化

在 NUMA 系统上：

```bash
# 查看 NUMA 拓扑
numactl --hardware

# 配置示例：将线程绑定到本地 NUMA 节点
pixels.cpu.affinity.strategy=custom
pixels.cpu.affinity.core.mapping=0,1,2,3,4,5,6,7,8,9,10,11  # NUMA node 0
```

### 3. 超线程考虑

- **启用超线程**：可以提高吞吐量，但可能增加延迟
- **禁用超线程**：使用 custom 策略只绑定物理核心，可以获得更稳定的性能

### 4. 与其他优化结合

CPU 亲和性应该与其他优化配合使用：

```properties
# 完整的性能优化配置示例
pixel.threads=24
pixels.enable.cpu.affinity=true
pixels.cpu.affinity.strategy=round-robin

# I/O 优化
localfs.enable.async.io=true
localfs.async.lib=iouring
pixels.doublebuffer=true

# 内存优化
pixel.enable.globalStaticBytebuffer=true
```

## 故障排查

### 问题 1：绑核失败

**症状**：看到 "Failed to bind thread to CPU core" 错误

**可能原因**：
- 权限不足
- 核心 ID 超出范围
- 系统不支持 CPU 亲和性

**解决方案**：
```bash
# 检查权限
ulimit -a

# 使用 root 权限运行（不推荐）
# 或者调整 cgroup 限制
```

### 问题 2：性能没有提升

**可能原因**：
- 工作负载不适合绑核
- 绑核策略选择不当
- 系统负载过高

**解决方案**：
1. 使用 perf 分析缓存命中率
2. 尝试不同的绑核策略
3. 检查系统整体负载

### 问题 3：配置解析错误

**症状**：看到 "Failed to parse custom CPU affinity mapping" 错误

**解决方案**：
- 检查 `pixels.cpu.affinity.core.mapping` 格式
- 确保使用逗号分隔，没有空格
- 确保核心 ID 在有效范围内

## 实现细节

### 核心类：CPUAffinity

位置：`pixels-duckdb/CPUAffinity.h`

主要方法：
- `BindToCore(int coreId)`：绑定到单个核心
- `BindToCores(const std::vector<int>& coreIds)`：绑定到多个核心
- `GetNumCores()`：获取系统核心数
- `GetCurrentAffinity()`：获取当前亲和性
- `ClearAffinity()`：清除亲和性限制

### 集成点

CPU 亲和性在 `PixelsScanInitLocal` 函数中设置，该函数在每个工作线程初始化时调用。

## 参考资料

- Linux CPU Affinity: `man pthread_setaffinity_np`
- NUMA 优化: `man numactl`
- 性能分析: `man perf`

## 版本历史

- v1.0 (2026-03-02): 初始实现，支持三种绑核策略
