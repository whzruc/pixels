# Global ByteBuffer Pool 使用说明

## 概述

全局 ByteBuffer 池是一个预分配、可复用的内存缓冲区池，用于优化 Pixels 插件的内存使用。该池支持多种大小级别的缓冲区，可以显著减少频繁的内存分配开销。

## 架构设计

### 核心组件

1. **GlobalByteBufferPool** (`pixels-common/include/physical/GlobalByteBufferPool.h`)
   - 单例模式实现
   - 线程安全（使用 mutex 保护）
   - 支持多种 size class（1MB, 8MB, 64MB, 128MB）
   - 自动预分配策略

2. **配置文件** (`global-bufferpool.properties`)
   - 定义缓冲区大小级别
   - 配置预分配数量
   - 设置最大内存限制

3. **集成点**
   - `DynamicBufferPool`: 使用全局池进行 buffer 的分配和释放
   - `pixels_extension.cpp`: 在扩展加载时初始化全局池
   - `PixelsReadLocalState`: 在查询结束时统计和释放资源

## 配置说明

### global-bufferpool.properties

```properties
# Size Classes（缓冲区大小级别）
global.bufferpool.size.class.1=1048576      # 1MB
global.bufferpool.size.class.2=8388608      # 8MB
global.bufferpool.size.class.3=67108864     # 64MB
global.bufferpool.size.class.4=134217728    # 128MB

# Pre-allocation Multipliers（预分配倍数）
# 公式：预分配数量 = numThreads × multiplier
global.bufferpool.prealloc.multiplier.1=80  # 1MB: 线程数 × 80
global.bufferpool.prealloc.multiplier.2=40  # 8MB: 线程数 × 40
global.bufferpool.prealloc.multiplier.3=10  # 64MB: 线程数 × 10
global.bufferpool.prealloc.multiplier.4=5   # 128MB: 线程数 × 5

# Limits（限制）
global.bufferpool.max.buffer.size=134217728      # 最大缓冲区 128MB
global.bufferpool.max.memory.limit=0             # 0 = 无限制（预留功能）

# Statistics（统计）
global.bufferpool.enable.stats=false             # 是否启用内存统计
```

### pixels-cpp.properties

确保以下配置正确设置：

```properties
# 启用动态缓冲池
pixels.enable.dynamic.buffer=true

# 工作线程数（影响预分配数量）
pixel.threads=8

# 文件系统块大小（用于对齐）
localfs.block.size=4096
```

## 使用示例

### 1. 基本使用（自动）

只需正确配置文件，全局池会在 Pixels 扩展加载时自动初始化：

```bash
# 确保配置文件位于工作目录
ls global-bufferpool.properties

# 启动 DuckDB 并加载 Pixels 扩展
./duckdb
```

### 2. 启用内存统计

在 `global-bufferpool.properties` 中设置：

```properties
global.bufferpool.enable.stats=true
```

查询结束后会自动输出统计信息：

```
=== Query Finished - Memory Statistics ===
Total Allocated: 1024.00 MB
Currently Used: 256.00 MB
Fragmentation: 75.00 %
Buffer Counts by Size Class:
  1 MB: 640 total, 100 in use, 540 free
  8 MB: 320 total, 20 in use, 300 free
  64 MB: 80 total, 2 in use, 78 free
  128 MB: 40 total, 0 in use, 40 free
========================================
```

### 3. 调整缓冲区配置

根据工作负载调整 size class 和 multiplier：

**场景 1：小文件密集查询**
```properties
# 增加小缓冲区数量
global.bufferpool.prealloc.multiplier.1=160  # 原来是 80
global.bufferpool.prealloc.multiplier.2=80   # 原来是 40
```

**场景 2：大文件查询**
```properties
# 增加大缓冲区数量
global.bufferpool.prealloc.multiplier.3=20   # 原来是 10
global.bufferpool.prealloc.multiplier.4=10   # 原来是 5
```

## 内存管理流程

### 分配流程

1. `DynamicBufferPool::AllocateBuffer(colId, size)` 被调用
2. 检查 `GlobalByteBufferPool` 是否已初始化
3. 如果已初始化：
   - 调用 `GlobalByteBufferPool::AcquireBuffer(size)`
   - 从对应 size class 的空闲队列中获取 buffer
   - 如果队列为空，分配新的 buffer
4. 如果未初始化：
   - 降级到 `DirectIoLib::allocateDirectBuffer()` (旧行为)

### 释放流程

1. `DynamicBufferPool::ReleaseBuffer(colId)` 被调用
2. 获取 buffer 引用
3. 调用 `GlobalByteBufferPool::ReleaseBuffer(buffer)`
4. Buffer 被归还到对应 size class 的空闲队列
5. 更新使用计数

### 查询结束时

- `PixelsReadLocalState` 析构函数被调用
- 如果启用统计，打印内存使用信息
- 调用 `DirectUringRandomAccessFileDynamic::Reset()`
- 清理 io_uring 资源

## 性能优化建议

### 1. 预分配数量调整

根据并发查询数和数据规模调整 multiplier：

```
推荐值 = 每个查询平均使用的 buffer 数 × 最大并发查询数 / 线程数
```

### 2. Size Class 选择

- 分析实际查询中的 buffer 大小分布
- 确保 80% 的请求能命中现有 size class
- 避免过多的 GrowBuffer 操作

### 3. 监控碎片率

- 碎片率 > 50%：可能预分配过多，考虑减少 multiplier
- 碎片率 < 10%：可能预分配不足，考虑增加 multiplier

## 故障排查

### 问题 1：池初始化失败

**现象**：
```
Pixels Extension: Failed to initialize Global ByteBuffer Pool: Cannot open config file: global-bufferpool.properties
```

**解决**：
- 检查配置文件是否存在于工作目录
- 检查文件权限是否可读
- 可以使用绝对路径

### 问题 2：内存占用过高

**现象**：系统内存占用异常高

**解决**：
- 降低 `prealloc.multiplier` 值
- 减少线程数 `pixel.threads`
- 启用 `max.memory.limit`（未来功能）

### 问题 3：Buffer 请求超过最大限制

**现象**：
```
GlobalByteBufferPool: Requested size ... exceeds max buffer size 134217728
```

**解决**：
- 增加 `global.bufferpool.max.buffer.size`
- 或者添加更大的 size class

## 未来扩展

### 预留功能

1. **内存限制**
   - `SetMaxMemoryLimit()` 已实现接口
   - 需要实现超限时的驱逐策略

2. **自适应调整**
   - 根据运行时统计动态调整预分配数量
   - 自动识别热门 size class

3. **持久化统计**
   - 将统计信息写入日志文件
   - 支持长期性能分析

## 编译说明

确保 `CMakeLists.txt` 包含：

```cmake
lib/physical/GlobalByteBufferPool.cpp
include/physical/GlobalByteBufferPool.h
```

重新编译：

```bash
cd build
cmake ..
make -j
```

## 相关文件

- `pixels-common/include/physical/GlobalByteBufferPool.h` - 头文件
- `pixels-common/lib/physical/GlobalByteBufferPool.cpp` - 实现
- `pixels-common/lib/physical/DynamicBufferPool.cpp` - 集成点
- `pixels-duckdb/pixels_extension.cpp` - 初始化
- `include/PixelsReadLocalState.hpp` - 析构和统计
- `global-bufferpool.properties` - 配置文件

## 作者

- whz
- 创建日期：2026-01-31
