# Global Static Buffer Pool with Ring Pool 实现文档

## 概述

本次实现完成了一个改进的全局静态缓冲池（GlobalStaticBufferPool），在extension load阶段预创建io_uring ring pool并预注册所有buffer，每个线程从pool中获取预配置好的ring，实现零运行时开销的高性能I/O。

## 核心架构

### 设计理念

**原架构问题**：
- io_uring ring是thread_local的，每个线程首次使用时才创建
- buffer注册发生在线程初始化时
- GlobalStaticBufferPool已预分配buffer，但未与ring绑定

**新架构方案**：
1. **Extension Load阶段**：预创建所有ring + 预注册所有buffer
2. **Thread Pool**：每个线程获取一个预配置的ring（非thread_local创建）
3. **Buffer索引**：每个ring只注册属于该线程的buffer（方案B）

### 架构组件

```
┌─────────────────────────────────────────────────────────────┐
│              GlobalStaticBufferPool (Singleton)              │
├─────────────────────────────────────────────────────────────┤
│  Buffer Pool: columnName -> [thread][bufferIdx] -> Buffer   │
│  Ring Pool:   threadId -> io_uring*                         │
│  ThreadId Allocator: atomic<int>                            │
└─────────────────────────────────────────────────────────────┘
                           │
                           ↓
          ┌────────────────┴────────────────┐
          │                                 │
    ┌─────▼─────┐                    ┌─────▼─────┐
    │  Thread 0  │                    │  Thread N  │
    ├───────────┤                    ├───────────┤
    │ Ring 0    │                    │ Ring N    │
    │ Buffer[0] │                    │ Buffer[N] │
    └───────────┘                    └───────────┘
```

## 实现细节

### 1. GlobalStaticBufferPool 增强

**文件**:
- `pixels-common/include/physical/GlobalStaticBufferPool.h`
- `pixels-common/lib/physical/GlobalStaticBufferPool.cpp`

**新增功能**:

#### Ring Pool管理
```cpp
std::vector<struct io_uring*> rings_;        // Ring pool
std::atomic<int> nextThreadId_;              // 线程ID分配器
std::map<std::string, int> columnIndex_;     // 列名到索引的映射
std::vector<std::string> columnNames_;       // 有序列名列表
```

#### 核心方法
```cpp
// 获取线程ID（原子递增）
int AcquireThreadId();

// 获取指定线程的ring
struct io_uring* GetRing(int threadId);

// 获取buffer在注册数组中的索引
int GetBufferIndex(const std::string& columnName, int threadId, int bufferIdx);
```

#### Initialize流程
1. 预分配所有buffer（maxThreads * 2 * numColumns）
2. 建立列名到索引的映射
3. 为每个线程创建io_uring ring
4. 为每个ring注册该线程的buffer（每列2个buffer用于double buffering）

**Buffer索引计算**:
```cpp
int bufferIndex = columnIndex * 2 + bufferIdx;
// 每个线程的ring注册：numColumns * 2 个buffer
```

### 2. DirectUringRandomAccessFileStatic

**文件**:
- `pixels-common/include/physical/natives/DirectUringRandomAccessFileStatic.h`
- `pixels-common/lib/physical/natives/DirectUringRandomAccessFileStatic.cpp`

**特点**:
- 构造时接收threadId和ring（不使用thread_local）
- 使用`io_uring_prep_read_fixed`与预注册的buffer
- readAsync接口需要columnName和bufferIdx来计算buffer索引

```cpp
std::shared_ptr<ByteBuffer> readAsync(
    int length,
    std::shared_ptr<ByteBuffer> buffer,
    const std::string& columnName,  // 新增：用于查找buffer索引
    int bufferIdx                    // 新增：0或1（double buffering）
);
```

### 3. ThreadContext 辅助类

**文件**:
- `pixels-common/include/physical/ThreadContext.h`
- `pixels-common/lib/physical/ThreadContext.cpp`

**作用**: thread_local存储，用于在LocalFS::openRaf()时传递thread context

```cpp
class ThreadContext {
    static thread_local int threadId_;
    static thread_local struct io_uring* ring_;

public:
    static void SetThreadId(int threadId);
    static void SetRing(struct io_uring* ring);
    static int GetThreadId();
    static struct io_uring* GetRing();
    static bool HasContext();
};
```

### 4. 集成流程

#### pixels_extension.cpp (Extension Load)
```cpp
if (pixel.enable.globalStaticBytebuffer) {
    GlobalStaticBufferPool::Instance().Initialize(
        columnSizeCSVPath,
        fsBlockSize,
        maxThreads  // 48
    );
}
```

#### PixelsScanInitLocal (Thread Initialization)
```cpp
result->threadId = GlobalStaticBufferPool::Instance().AcquireThreadId();
result->ring = GlobalStaticBufferPool::Instance().GetRing(result->threadId);

// 设置thread-local context
ThreadContext::SetThreadId(result->threadId);
ThreadContext::SetRing(result->ring);
```

#### LocalFS::openRaf (File Open)
```cpp
if (useStaticBufferPool && ThreadContext::HasContext()) {
    int threadId = ThreadContext::GetThreadId();
    struct io_uring* ring = ThreadContext::GetRing();
    return std::make_shared<DirectUringRandomAccessFileStatic>(path, threadId, ring);
}
```

#### PhysicalLocalReader (I/O Operations)
```cpp
if (useStaticBufferPool && ThreadContext::HasContext()) {
    auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileStatic>(raf);
    directRaf->readAsyncSubmit(size);
    directRaf->readAsyncComplete(size);
}
```

## 配置说明

### pixels-cpp.properties

```properties
# 启用全局静态缓冲池
pixel.enable.globalStaticBytebuffer=true

# 线程数（影响ring pool大小）
pixel.threads=48

# 列大小CSV文件路径
pixel.globalStaticBytebuffer.columnSize=/path/to/column_sizes.csv

# 文件系统块大小
localfs.block.size=4096

# 启用异步I/O
localfs.enable.async.io=true
localfs.async.lib=iouring
```

### column_sizes.csv 格式

```csv
column_name,size_bytes
WatchID,8388608
JavaEnable,1048576
Title,67108864
...
```

## 性能优势

### 1. 零运行时分配开销
- ✅ 所有buffer在extension load时预分配
- ✅ 所有ring在extension load时创建
- ✅ Buffer注册在extension load时完成
- ✅ 线程只需获取threadId（原子递增）

### 2. 完美的资源隔离
- ✅ 每个线程独占一个ring（无竞争）
- ✅ 每个线程独占自己的buffer
- ✅ Buffer注册只包含该线程的buffer（方案B）

### 3. 最优的内存布局
- ✅ Buffer紧密排列，cache友好
- ✅ 每个ring的注册buffer数量最小化

## 文件清单

### 新增文件
1. `pixels-common/include/physical/ThreadContext.h`
2. `pixels-common/lib/physical/ThreadContext.cpp`
3. `pixels-common/include/physical/natives/DirectUringRandomAccessFileStatic.h`
4. `pixels-common/lib/physical/natives/DirectUringRandomAccessFileStatic.cpp`

### 修改文件
1. `pixels-common/include/physical/GlobalStaticBufferPool.h` - 添加Ring Pool支持
2. `pixels-common/lib/physical/GlobalStaticBufferPool.cpp` - 实现Ring创建和buffer注册
3. `include/PixelsReadLocalState.hpp` - 添加threadId和ring字段
4. `pixels-duckdb/PixelsScanFunction.cpp` - 线程上下文获取
5. `pixels-common/lib/physical/storage/LocalFS.cpp` - 支持DirectUringRandomAccessFileStatic
6. `pixels-common/lib/physical/io/PhysicalLocalReader.cpp` - 支持静态buffer模式
7. `pixels-common/CMakeLists.txt` - 添加新源文件

## 使用流程

### 1. 准备列大小CSV文件
运行ColumnSizeAnalyzer工具分析数据集：
```bash
cd /path/to/pixels/cpp/build
./tests/column-size-analyzer/ColumnSizeAnalyzer /path/to/data/*.pxl output.csv
```

### 2. 配置启用
在`pixels-cpp.properties`中设置：
```properties
pixel.enable.globalStaticBytebuffer=true
pixel.globalStaticBytebuffer.columnSize=/path/to/output.csv
pixel.threads=48
```

### 3. 加载Extension
```sql
LOAD 'build/release/extension/pixels/pixels.duckdb_extension';
```

### 4. 运行查询
```sql
SELECT * FROM 'data/*.pxl' WHERE ...;
```

### 5. 观察日志
```
GlobalStaticBufferPool: Buffer allocation complete
  Total columns: 105
  Total buffers: 10080
  Total memory: 12288.00 MB
GlobalStaticBufferPool: Creating io_uring ring pool...
GlobalStaticBufferPool: Ring pool created successfully
  Total rings: 48
  Buffers per ring: 210
GlobalStaticBufferPool: Initialization complete

PixelsScanInitLocal: Thread acquired threadId=0
PixelsScanInitLocal: Thread acquired threadId=1
...
```

## 注意事项

### 1. 线程数限制
- maxThreads必须覆盖实际使用的最大并发线程数
- 超过maxThreads会抛出异常
- 建议设置为DuckDB线程池大小

### 2. 内存消耗
```
总内存 = numColumns * maxThreads * 2 * avgColumnSize
```
示例：105列 × 48线程 × 2缓冲 × 平均120MB ≈ 12GB

### 3. API不兼容性
- 当前实现中，`PhysicalLocalReader::readAsync`在GlobalStatic模式下会抛出异常
- 需要使用不同的代码路径（通过thread context判断）
- 未来可能需要修改PixelsRecordReaderImpl以支持新API

## 未来扩展

### 1. 动态调整
- 支持运行时增加/减少ring数量
- 支持buffer pool的动态扩展

### 2. 统计信息
- 添加每个ring的使用统计
- 监控buffer命中率

### 3. 自适应优化
- 根据查询模式自动调整buffer大小
- 智能线程分配策略

## 作者

- **whz**
- **创建日期**: 2026-02-10

## 参考文档

- [Global ByteBuffer Pool](global-buffer-pool.md)
- [列大小收集工具](../features/column-size-collection.md)
