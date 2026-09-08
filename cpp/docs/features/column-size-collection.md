# Column Size Collection Feature

## 概述

这个功能允许在读取 Pixels 文件时收集每列的最大长度信息，并将结果保存到 CSV 文件中。这对于优化 BufferPool 预分配大小非常有用。

## 功能说明

当启用 `pixel.getColumnSize=true` 配置时：
1. **不执行** `BufferPool::Initialize()` - 避免实际分配缓冲区
2. 收集每个列的字节长度（bytes 参数）
3. 跟踪每列在所有文件/Row Groups 中看到的**最大长度**
4. 在查询结束时将结果写入 CSV 文件

## 配置参数

在配置文件（如 `pixels-cpp.properties`）中添加：

```properties
# 启用列大小收集模式
pixel.getColumnSize=true

# （可选）指定输出 CSV 文件名，默认为 column_sizes.csv
pixel.getColumnSize.output=/path/to/custom_output.csv
```

## 使用方法

### 1. 修改配置文件

编辑 `pixels-cpp.properties`：

```properties
# 启用列大小收集
pixel.getColumnSize=true

# 指定输出文件（可选）


# 其他必要配置
localfs.enable.async.io=true
localfs.async.lib=iouring
pixel.stride=10000
pixel.threads=4
```

### 2. 运行查询

在 DuckDB 中执行查询，例如：

```sql
-- 扫描 Pixels 文件以收集列大小信息
SELECT COUNT(*) FROM pixels_scan('/data/path/to/pixels/files/*');
```

### 3. 查看结果

查询完成后，会生成 CSV 文件，格式如下：

```csv
column_name,max_length_bytes
WatchID,8
JavaEnable,1
Title,524288
GoodEvent,1
EventTime,8
EventDate,4
CounterID,4
ClientIP,4
RegionID,4
UserID,8
CounterClass,1
...
```

## 输出说明

- **column_name**: 列名
- **max_length_bytes**: 该列在所有扫描的文件/Row Groups 中观察到的最大字节长度

## 应用场景

### 场景 1: BufferPool 预分配优化

使用收集到的列大小信息优化 BufferPool 配置：

```cpp
// 根据 column_sizes.csv 设置预分配大小
// 例如，如果 Title 列最大为 524288 字节
GlobalStaticBufferPool::PreAllocate("Title", 524288 * buffer_count);
```

### 场景 2: 多文件统计

扫描整个数据集以获得全局统计信息：

```sql
-- 扫描所有分区
SELECT COUNT(*) FROM pixels_scan('/data/clickbench/partition_*/*');
```

生成的 CSV 包含所有分区中每列的最大值。

## 注意事项

1. **性能影响**:
   - 在收集模式下，不会分配 BufferPool，节省内存
   - 但仍然会读取文件元数据和 Row Group 信息

2. **线程安全**:
   - `ColumnSizeCSVWriter` 是线程安全的单例
   - 支持多线程并发更新

3. **数据累积**:
   - CSV Writer 在进程生命周期内累积数据
   - 每个查询都会更新已有的最大值
   - 如需重置，重启进程或调用 `ColumnSizeCSVWriter::Instance().clear()`

4. **与 BufferPool 的互斥**:
   - 当 `pixel.getColumnSize=true` 时，不会初始化 BufferPool
   - 这是设计目的 - 仅收集信息，不执行实际 I/O

## 示例工作流程

```bash
# 1. 编辑配置文件
vim pixels-cpp.properties

# 2. 添加配置
echo "pixel.getColumnSize=true" >> pixels-cpp.properties
echo "pixel.getColumnSize.output=./clickbench_column_sizes.csv" >> pixels-cpp.properties

# 3. 运行 DuckDB 查询
./build/release/duckdb
> CREATE VIEW hits AS SELECT * FROM pixels_scan('/data/clickbench/*');
> SELECT COUNT(*) FROM hits;

# 4. 查看结果
cat clickbench_column_sizes.csv
```

## 实现细节

### 核心组件

1. **ColumnSizeCSVWriter** (`pixels-common/include/utils/ColumnSizeCSVWriter.h`)
   - 线程安全的单例类
   - 跟踪列名 → 最大字节数的映射
   - 提供 CSV 输出功能

2. **PixelsRecordReaderImpl** (`pixels-core/lib/reader/PixelsRecordReaderImpl.cpp`)
   - 在 `read()` 方法中检查配置
   - 收集列大小信息
   - 跳过 BufferPool 初始化

3. **PixelsScanFunction** (`pixels-duckdb/PixelsScanFunction.cpp`)
   - 在查询结束时触发 CSV 写入
   - 输出统计信息

### 数据流

```
Query Start
    ↓
PixelsRecordReaderImpl::read()
    ↓
检查 pixel.getColumnSize
    ↓ (true)
ColumnSizeCSVWriter::updateColumnSizes()
    ↓ (收集每列的 bytes)
继续处理其他 Row Groups...
    ↓
Query End (所有线程完成)
    ↓
PixelsScanFunction::PixelsParallelStateNext()
    ↓
ColumnSizeCSVWriter::writeToCSV()
    ↓
生成 column_sizes.csv
```

## 故障排查

### 问题: CSV 文件未生成

**可能原因**:
- 配置未正确设置
- 查询未完全执行完成
- 文件写入权限问题

**解决方案**:
```bash
# 检查配置
grep "pixel.getColumnSize" pixels-cpp.properties

# 确保查询完全执行
# 检查控制台输出是否有 "Column Size Collection Complete"

# 检查文件权限
ls -l column_sizes.csv
```

### 问题: 列大小看起来不正确

**可能原因**:
- 数据被累积（多次查询）
- 需要重置

**解决方案**:
- 重启 DuckDB 进程以清除累积的数据

## 未来改进

- [ ] 添加统计信息（平均值、中位数等）
- [ ] 支持增量更新现有 CSV
- [ ] 提供 JSON 输出格式选项
- [ ] 添加命令行工具模式
