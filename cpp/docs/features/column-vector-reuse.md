# ColumnVector 重用优化

## 问题描述

在原始实现中，每次调用 `createRowBatch()` 时都会：
1. 调用 `createColumn()` 为每一列创建新的 ColumnVector
2. 根据类型（如 IntColumnVector）分配内存空间
3. 调用 ColumnVector 构造函数

这导致频繁的内存分配和释放，影响性能。

## 优化方案

### 核心思想
由于 `batchSize` 在创建 PixelsRecordReaderImpl 时已知，我们可以：
1. 在 PixelsRecordReaderImpl 构造函数中预先创建所有 ColumnVector
2. 将这些 ColumnVector 存储在 `reusableColumns` 成员变量中
3. 在 `createRowBatch()` 中重用这些预分配的 ColumnVector

### 实现细节

#### 1. 添加成员变量（PixelsRecordReaderImpl.h）
```cpp
// Reusable column vectors to avoid repeated allocations
std::vector<std::shared_ptr<ColumnVector>> reusableColumns;
```

#### 2. 在构造函数中预分配（PixelsRecordReaderImpl.cpp）
```cpp
// Pre-allocate reusable column vectors
int numColumns = this->resultSchema->getChildren().size();
reusableColumns.reserve(numColumns);
for (int i = 0; i < numColumns; i++) {
    auto fieldType = this->resultSchema->getChildren().at(i);
    reusableColumns.push_back(createColumn(fieldType, this->batchSize));
}
```

#### 3. 修改 createRowBatch() 重用 ColumnVector
```cpp
std::shared_ptr<VectorizedRowBatch> PixelsRecordReaderImpl::createRowBatch() {
    int numColumns = resultSchema->getChildren().size();
    std::shared_ptr<VectorizedRowBatch> rowBatch = std::make_shared<VectorizedRowBatch>(numColumns, batchSize);
    // Reuse pre-allocated column vectors instead of creating new ones
    for (int i = 0; i < numColumns; i++) {
        rowBatch->cols[i] = reusableColumns[i];
        // Reset the column vector for reuse
        rowBatch->cols[i]->reset();
    }
    return rowBatch;
}
```

## 性能优势

1. **减少内存分配次数**：从每次 createRowBatch 都分配变为只在初始化时分配一次
2. **减少构造/析构开销**：避免频繁创建和销毁 ColumnVector 对象
3. **更好的内存局部性**：重用相同的内存区域，提高 CPU 缓存命中率
4. **减少内存碎片**：避免频繁的小块内存分配和释放

## 注意事项

1. **线程安全**：如果多个线程共享同一个 PixelsRecordReaderImpl 实例，需要额外的同步机制
2. **reset() 方法**：依赖 ColumnVector::reset() 方法正确清理状态，确保每次重用时数据是干净的
3. **内存占用**：预分配会在初始化时占用更多内存，但对于批处理场景这是可接受的权衡

## 测试建议

1. 验证功能正确性：确保重用 ColumnVector 后读取的数据与原实现一致
2. 性能测试：对比优化前后的性能差异，特别是在大量小批次读取场景
3. 内存测试：验证内存使用是否符合预期，没有内存泄漏
