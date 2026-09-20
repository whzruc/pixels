/*
 * Copyright 2026 PixelsDB.
 *
 * This file is part of Pixels.
 *
 * Pixels is free software: you can redistribute it and/or modify
 * it under the terms of the Affero GNU General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Pixels is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Affero GNU General Public License for more details.
 *
 * You should have received a copy of the Affero GNU General Public
 * License along with Pixels.  If not, see
 * <https://www.gnu.org/licenses/>.
 */

/*
 * @author whz
 * @create 2026-02-01
 */
#ifndef PIXELS_GLOBALSTATICBUFFERPOOL_H
#define PIXELS_GLOBALSTATICBUFFERPOOL_H

#include <memory>
#include <map>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include "liburing.h"
#include "physical/natives/ByteBuffer.h"
#include "physical/natives/DirectIoLib.h"
#include "utils/ColumnSizeCSVReader.h"
#include "exception/InvalidArgumentException.h"

/**
 * GlobalStaticBufferPool pre-allocates all buffers and io_uring rings at extension load time.
 * 
 * Architecture:
 * - Pre-allocates buffers based on column sizes from CSV file
 * - Each column gets (maxThreads * 2) buffers for double buffering
 * - Creates io_uring ring pool (one ring per thread)
 * - Registers buffers to each ring during initialization
 * - Thread-local access with no allocation overhead at query time
 */
class GlobalStaticBufferPool {
public:
    static constexpr int RINGS_PER_THREAD = 2;

    /**
     * Get the singleton instance
     */
    static GlobalStaticBufferPool& Instance();

    /**
     * Initialize the global static buffer pool
     * 
     * @param columnSizeCSVPath Path to CSV file containing column sizes
     * @param fsBlockSize File system block size for alignment
     * @param maxThreads Maximum number of threads (default 48)
     */
    void Initialize(const std::string& columnSizeCSVPath, int fsBlockSize, int maxThreads = 48);

    /**
     * Acquire a thread context (threadId and ring)
     * This should be called once per thread at initialization
     * 
     * @return threadId assigned to this thread
     */
    int AcquireThreadId();

    /**
     * Get the io_uring ring for a specific thread
     * 
     * @param threadId Thread ID (0 to maxThreads-1)
     * @return Pointer to io_uring ring
     */
    struct io_uring* GetRing(int threadId);
    struct io_uring* GetRing(int threadId, int ringSlot);

    /**
     * Get a buffer for a specific column, thread, and buffer index
     * 
     * @param columnName The column name
     * @param threadId Thread ID (0 to maxThreads-1)
     * @param bufferIdx Buffer index (0 or 1 for double buffering)
     * @return Shared pointer to ByteBuffer
     */
    std::shared_ptr<ByteBuffer> GetBuffer(const std::string& columnName, int threadId, int bufferIdx);

    /**
     * Get the buffer index in the registered buffer array
     * Used for io_uring_prep_read_fixed
     * 
     * @param columnName The column name
     * @param threadId Thread ID (0 to maxThreads-1)
     * @param bufferIdx Buffer index (0 or 1 for double buffering)
     * @return Index in the registered buffer array for this thread's ring
     */
    int GetBufferIndex(const std::string& columnName, int threadId, int bufferIdx);

    /**
     * Get buffer size for a column
     */
    uint64_t GetBufferSize(const std::string& columnName) const;

    /**
     * Check if initialized
     */
    bool IsInitialized() const { return initialized_; }

    /**
     * Reset the pool
     */
    void Reset();

    /**
     * Get total allocated memory in bytes
     */
    size_t GetTotalAllocatedBytes() const;

private:
    // Singleton pattern
    GlobalStaticBufferPool() : nextThreadId_(0), maxThreads_(0), initialized_(false) {}
    ~GlobalStaticBufferPool() { if (initialized_) Reset(); }
    GlobalStaticBufferPool(const GlobalStaticBufferPool&) = delete;
    GlobalStaticBufferPool& operator=(const GlobalStaticBufferPool&) = delete;

    // Buffer storage: columnName -> threadId -> bufferIdx(0/1) -> ByteBuffer
    std::map<std::string, std::vector<std::vector<std::shared_ptr<ByteBuffer>>>> buffers_;

    // Column sizes from CSV
    std::map<std::string, uint64_t> columnSizes_;
    
    // Column name to index mapping (for buffer index calculation)
    std::map<std::string, int> columnIndex_;
    
    // Ordered column names (to maintain consistent buffer index)
    std::vector<std::string> columnNames_;

    // DirectIoLib for aligned allocation
    std::shared_ptr<DirectIoLib> directIoLib_;

    // Ring pool: [threadId][ringSlot]
    std::vector<std::vector<struct io_uring*>> rings_;

    // Maximum threads
    int maxThreads_;
    
    // Thread ID allocator (atomic counter)
    std::atomic<int> nextThreadId_;

    // Thread safety
    mutable std::mutex poolMutex_;

    // Initialization flag
    bool initialized_;
};

#endif // PIXELS_GLOBALSTATICBUFFERPOOL_H
