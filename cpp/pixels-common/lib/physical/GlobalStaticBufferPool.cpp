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
#include "physical/GlobalStaticBufferPool.h"
#include <iostream>
#include <cstring>
#if defined(__linux__)
#include <sys/mman.h>
#endif

GlobalStaticBufferPool& GlobalStaticBufferPool::Instance() {
    static GlobalStaticBufferPool instance;
    return instance;
}

void GlobalStaticBufferPool::Initialize(const std::string& columnSizeCSVPath, int fsBlockSize, int maxThreads) {
    std::lock_guard<std::mutex> lock(poolMutex_);
    
    if (initialized_) {
        std::cout << "GlobalStaticBufferPool: Already initialized, skipping." << std::endl;
        return;
    }
    
    maxThreads_ = maxThreads;
    nextThreadId_.store(0);
    
    // Initialize DirectIoLib
    directIoLib_ = std::make_shared<DirectIoLib>(fsBlockSize);
    
    // Read column sizes from CSV
    std::cout << "GlobalStaticBufferPool: Reading column sizes from " << columnSizeCSVPath << std::endl;
    auto csvReader = std::make_shared<ColumnSizeCSVReader>(columnSizeCSVPath);
    
    // Get all column names and sizes
    auto columnMap = csvReader->getAllColumnSizes();
    
    // Optional: back the persistent content buffers with transparent huge pages.
    // perf stat on q45/doublebuffer/t48 showed dTLB-load-misses ~5.79%; 2MB pages cut
    // both TLB misses and the number of first-touch faults on these large buffers.
    // Default off (THP behavior is environment-sensitive); enable via
    // pixels.static.buffer.hugepage=true.
    bool useHugePage = false;
    try {
        useHugePage = ConfigFactory::Instance().getBoolProperty("pixels.static.buffer.hugepage", false);
    } catch (...) {
        useHugePage = false;
    }

    size_t totalAllocatedBytes = 0;
    int totalBuffers = 0;
    
    // Build ordered column list for consistent buffer indexing
    int colIdx = 0;
    for (const auto& pair : columnMap) {
        columnNames_.push_back(pair.first);
        columnIndex_[pair.first] = colIdx++;
    }


    
    // Pre-allocate buffers for each column
    for (const auto& pair : columnMap) {
        const std::string& columnName = pair.first;
        uint64_t columnSize = pair.second;
        
        columnSizes_[columnName] = columnSize;
        
        // Allocate maxThreads * 2 buffers for this column
        // [threadId][bufferIdx]
        std::vector<std::vector<std::shared_ptr<ByteBuffer>>> threadBuffers;
        threadBuffers.resize(maxThreads_);
        
        for (int threadId = 0; threadId < maxThreads_; threadId++) {
            threadBuffers[threadId].resize(2);
            
            for (int bufferIdx = 0; bufferIdx < 2; bufferIdx++) {
                // Allocate aligned buffer
                auto buffer = directIoLib_->allocateDirectBuffer(columnSize);
                if (!buffer) {
                    throw InvalidArgumentException(
                        "GlobalStaticBufferPool: Failed to allocate buffer for column " + columnName
                    );
                }
                
#if defined(__linux__) && defined(MADV_HUGEPAGE)
                // Advise huge pages before the pre-touch memset so the zeroing faults
                // in 2MB pages instead of many 4KB pages.
                if (useHugePage) {
                    madvise(buffer->getPointer(), buffer->size(), MADV_HUGEPAGE);
                }
#endif
                // Zero out the buffer (also serves as persistent pre-touch: pages stay
                // resident and warm, so reads never first-touch-fault on them at runtime).
                std::memset(buffer->getPointer(), 0, buffer->size());
                
                threadBuffers[threadId][bufferIdx] = buffer;
                totalAllocatedBytes += buffer->size();
                totalBuffers++;
            }
        }
        
        buffers_[columnName] = threadBuffers;
    }
    
    std::cout << "GlobalStaticBufferPool: Buffer allocation complete" << std::endl;
    std::cout << "  Total columns: " << columnMap.size() << std::endl;
    std::cout << "  Total buffers: " << totalBuffers << std::endl;
    std::cout << "  Total memory: " << (totalAllocatedBytes / 1024.0 / 1024.0) << " MB" << std::endl;
    
    // Create io_uring rings and register buffers for each thread.
    // Each scan thread gets a current ring and a prefetch ring.
    std::cout << "GlobalStaticBufferPool: Creating io_uring ring pool..." << std::endl;
    rings_.resize(maxThreads_);

    bool useFixedBuffer = true;
    try {
        useFixedBuffer = ConfigFactory::Instance().boolCheckProperty("localfs.iouring.use.fixed.buffer");
    } catch (...) {
        useFixedBuffer = true; // default to fixed buffer for backward compatibility
    }
    for (int threadId = 0; threadId < maxThreads_; threadId++)
    {
        rings_[threadId].resize(RINGS_PER_THREAD, nullptr);
        for (int ringSlot = 0; ringSlot < RINGS_PER_THREAD; ringSlot++)
        {
            rings_[threadId][ringSlot] = new io_uring();
            if (io_uring_queue_init(4096, rings_[threadId][ringSlot], 0) < 0) {
                throw InvalidArgumentException(
                    "GlobalStaticBufferPool: Failed to initialize io_uring for thread " +
                    std::to_string(threadId) + ", ring slot " + std::to_string(ringSlot)
                );
            }

            // if use none-fixed do not register
            if (useFixedBuffer){
                // Prepare buffer registration for this thread
                // Each ring gets all columns, 2 buffers per column (for double buffering)
                int numBuffers = columnNames_.size() * 2;
                struct iovec* iovecs = (struct iovec*)calloc(numBuffers, sizeof(struct iovec));

                int bufIdx = 0;
                for (const auto& columnName : columnNames_) {
                    for (int bufferIdx = 0; bufferIdx < 2; bufferIdx++) {
                        auto buffer = buffers_[columnName][threadId][bufferIdx];
                        iovecs[bufIdx].iov_base = buffer->getPointer();
                        iovecs[bufIdx].iov_len = buffer->size();
                        bufIdx++;
                    }
                }

                int ret = io_uring_register_buffers(rings_[threadId][ringSlot], iovecs, numBuffers);
                free(iovecs);

                if (ret != 0) {
                    throw InvalidArgumentException(
                        "GlobalStaticBufferPool: Failed to register buffers for thread " +
                        std::to_string(threadId) + ", ring slot " + std::to_string(ringSlot) +
                        ", error: " + std::to_string(ret)
                    );
                }
            }
        }
    }

    if (!useFixedBuffer)
    {
        std::cout<<"NONE FIXED MODEL: DO NOT register buffers"<<std::endl;
    }
    std::cout << "GlobalStaticBufferPool: Ring pool created successfully" << std::endl;
    std::cout << "  Total rings: " << (maxThreads_ * RINGS_PER_THREAD) << std::endl;
    std::cout << "  Buffers per ring: " << (columnNames_.size() * 2) << std::endl;
    std::cout << "GlobalStaticBufferPool: Initialization complete" << std::endl;
    
    initialized_ = true;
}

std::shared_ptr<ByteBuffer> GlobalStaticBufferPool::GetBuffer(
    const std::string& columnName, int threadId, int bufferIdx) {
    
    std::lock_guard<std::mutex> lock(poolMutex_);
    
    if (!initialized_) {
        throw InvalidArgumentException("GlobalStaticBufferPool: Pool not initialized");
    }
    
    // Validate parameters
    if (threadId < 0 || threadId >= maxThreads_) {
        throw InvalidArgumentException(
            "GlobalStaticBufferPool: Invalid threadId " + std::to_string(threadId) +
            " (max: " + std::to_string(maxThreads_ - 1) + ")"
        );
    }
    
    if (bufferIdx < 0 || bufferIdx > 1) {
        throw InvalidArgumentException(
            "GlobalStaticBufferPool: Invalid bufferIdx " + std::to_string(bufferIdx) +
            " (must be 0 or 1)"
        );
    }
    
    // Find column buffers
    auto it = buffers_.find(columnName);
    if (it == buffers_.end()) {
        throw InvalidArgumentException(
            "GlobalStaticBufferPool: Column '" + columnName + "' not found in buffer pool"
        );
    }
    
    return it->second[threadId][bufferIdx];
}

uint64_t GlobalStaticBufferPool::GetBufferSize(const std::string& columnName) const {
    std::lock_guard<std::mutex> lock(poolMutex_);
    
    auto it = columnSizes_.find(columnName);
    if (it == columnSizes_.end()) {
        throw InvalidArgumentException(
            "GlobalStaticBufferPool: Column '" + columnName + "' not found"
        );
    }
    
    return it->second;
}

size_t GlobalStaticBufferPool::GetTotalAllocatedBytes() const {
    std::lock_guard<std::mutex> lock(poolMutex_);
    
    size_t total = 0;
    for (const auto& columnPair : buffers_) {
        for (const auto& threadBuffers : columnPair.second) {
            for (const auto& buffer : threadBuffers) {
                if (buffer) {
                    total += buffer->size();
                }
            }
        }
    }
    
    return total;
}

int GlobalStaticBufferPool::AcquireThreadId() {
    if (!initialized_) {
        throw InvalidArgumentException("GlobalStaticBufferPool: Pool not initialized");
    }
    
    int threadId = nextThreadId_.fetch_add(1);
    
    if (threadId >= maxThreads_) {
        throw InvalidArgumentException(
            "GlobalStaticBufferPool: Thread limit exceeded (max: " + 
            std::to_string(maxThreads_) + ")"
        );
    }
    
    return threadId;
}

struct io_uring* GlobalStaticBufferPool::GetRing(int threadId) {
    return GetRing(threadId, 0);
}

struct io_uring* GlobalStaticBufferPool::GetRing(int threadId, int ringSlot) {
    if (!initialized_) {
        throw InvalidArgumentException("GlobalStaticBufferPool: Pool not initialized");
    }
    
    if (threadId < 0 || threadId >= maxThreads_) {
        throw InvalidArgumentException(
            "GlobalStaticBufferPool: Invalid threadId " + std::to_string(threadId) +
            " (max: " + std::to_string(maxThreads_ - 1) + ")"
        );
    }

    if (ringSlot < 0 || ringSlot >= RINGS_PER_THREAD) {
        throw InvalidArgumentException(
            "GlobalStaticBufferPool: Invalid ring slot " + std::to_string(ringSlot)
        );
    }
    
    return rings_[threadId][ringSlot];
}

int GlobalStaticBufferPool::GetBufferIndex(const std::string& columnName, int threadId, int bufferIdx) {
    if (!initialized_) {
        throw InvalidArgumentException("GlobalStaticBufferPool: Pool not initialized");
    }
    
    // Validate parameters
    if (threadId < 0 || threadId >= maxThreads_) {
        throw InvalidArgumentException(
            "GlobalStaticBufferPool: Invalid threadId " + std::to_string(threadId)
        );
    }
    
    if (bufferIdx < 0 || bufferIdx > 1) {
        throw InvalidArgumentException(
            "GlobalStaticBufferPool: Invalid bufferIdx " + std::to_string(bufferIdx)
        );
    }
    
    auto it = columnIndex_.find(columnName);
    if (it == columnIndex_.end()) {
        throw InvalidArgumentException(
            "GlobalStaticBufferPool: Column '" + columnName + "' not found"
        );
    }
    
    // Buffer index calculation: columnIndex * 2 + bufferIdx
    // Each thread's ring has all columns registered, with 2 buffers per column
    return it->second * 2 + bufferIdx;
}

void GlobalStaticBufferPool::Reset() {
    std::lock_guard<std::mutex> lock(poolMutex_);
    
    if (!initialized_) {
        return;
    }
    
    std::cout << "GlobalStaticBufferPool: Resetting pool..." << std::endl;
    
    // Clean up io_uring rings
    for (auto& threadRings : rings_) {
        for (auto* ring : threadRings) {
            if (ring != nullptr) {
                io_uring_queue_exit(ring);
                delete ring;
            }
        }
    }
    rings_.clear();
    
    // Clear all buffers
    buffers_.clear();
    columnSizes_.clear();
    columnIndex_.clear();
    columnNames_.clear();
    
    directIoLib_ = nullptr;
    maxThreads_ = 0;
    nextThreadId_.store(0);
    initialized_ = false;
    
    std::cout << "GlobalStaticBufferPool: Reset complete" << std::endl;
}
