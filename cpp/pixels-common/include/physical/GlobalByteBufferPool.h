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
 * @create 2026-01-31
 */
#ifndef PIXELS_GLOBALBYTEBUFFERPOOL_H
#define PIXELS_GLOBALBYTEBUFFERPOOL_H

#include <memory>
#include <map>
#include <queue>
#include <mutex>
#include <vector>
#include <cstddef>
#include <string>
#include "physical/natives/ByteBuffer.h"
#include "physical/natives/DirectIoLib.h"
#include "exception/InvalidArgumentException.h"

/**
 * GlobalByteBufferPool provides a global buffer pool for reusing ByteBuffer objects.
 * 
 * Key features:
 * - Pre-allocates buffers in configurable size classes
 * - Thread-safe acquire/release operations
 * - Memory statistics tracking for debugging
 * - Configurable maximum buffer size limit
 * 
 * Configuration is read from global-bufferpool.properties file
 */
class GlobalByteBufferPool {
public:
    /**
     * Memory statistics structure
     */
    struct MemoryStats {
        size_t totalAllocatedBytes;   // Total bytes allocated in the pool
        size_t totalUsedBytes;         // Bytes currently in use (not in free queues)
        size_t totalRequestedBytes;    // Actual bytes requested by users
        double fragmentationRatio;     // (totalAllocated - totalUsed) / totalAllocated
        double internalFragmentationRatio;  // (totalAllocated - totalRequested) / totalAllocated
        std::map<size_t, size_t> bufferCountPerClass;  // Map of size class -> total count
        std::map<size_t, size_t> inUseCountPerClass;   // Map of size class -> in-use count
        std::map<size_t, size_t> preAllocatedCountPerClass;  // Map of size class -> pre-allocated count
        std::map<size_t, size_t> dynamicAllocatedCountPerClass;  // Map of size class -> dynamically allocated count
    };

    /**
     * Get the singleton instance
     */
    static GlobalByteBufferPool& Instance();

    /**
     * Initialize the buffer pool
     * Reads configuration from global-bufferpool.properties file
     * 
     * @param configFilePath Path to the configuration file
     * @param fsBlockSize File system block size for alignment
     * @param numThreads Number of worker threads (used to calculate pre-allocation count)
     */
    void Initialize(const std::string& configFilePath, int fsBlockSize, int numThreads);

    /**
     * Acquire a buffer from the pool
     * @param requestedSize Requested buffer size in bytes
     * @return A shared pointer to ByteBuffer
     * @throws InvalidArgumentException if requestedSize > max configured size
     */
    std::shared_ptr<ByteBuffer> AcquireBuffer(size_t requestedSize);

    /**
     * Release a buffer back to the pool
     * @param buffer The buffer to release
     */
    void ReleaseBuffer(std::shared_ptr<ByteBuffer> buffer);

    /**
     * Get current memory statistics
     */
    MemoryStats GetMemoryStats() const;

    /**
     * Reset and clear the entire pool
     */
    void Reset();

    /**
     * Check if the pool has been initialized
     */
    bool IsInitialized() const { return initialized_; }

    /**
     * Set maximum memory limit (reserved for future use)
     * @param maxBytes Maximum bytes allowed (0 = no limit)
     */
    void SetMaxMemoryLimit(size_t maxBytes) { maxMemoryLimit_ = maxBytes; }

    /**
     * Get maximum memory limit
     */
    size_t GetMaxMemoryLimit() const { return maxMemoryLimit_; }

    /**
     * Check if statistics are enabled
     */
    bool IsStatsEnabled() const { return enableStats_; }

private:
    // Singleton pattern
    GlobalByteBufferPool() = default;
    ~GlobalByteBufferPool() = default;
    GlobalByteBufferPool(const GlobalByteBufferPool&) = delete;
    GlobalByteBufferPool& operator=(const GlobalByteBufferPool&) = delete;

    /**
     * Load configuration from properties file
     */
    void LoadConfiguration(const std::string& configFilePath, int numThreads);

    /**
     * Parse a property file and return key-value pairs
     */
    std::map<std::string, std::string> ParsePropertiesFile(const std::string& filePath);

    /**
     * Find the smallest size class that can accommodate the requested size
     * @param requestedSize The requested size
     * @return The size class, or 0 if no suitable class exists
     */
    size_t FindSizeClass(size_t requestedSize) const;

    /**
     * Allocate a new buffer using DirectIoLib
     * @param sizeClass The size class to allocate
     * @return A new ByteBuffer
     */
    std::shared_ptr<ByteBuffer> AllocateNewBuffer(size_t sizeClass);

    // Configured size classes (loaded from config, sorted ascending)
    std::vector<size_t> sizeClasses_;

    // Maximum buffer size allowed (loaded from config)
    size_t maxBufferSize_;

    // Enable statistics collection
    bool enableStats_;

    // Free buffer queues for each size class
    std::map<size_t, std::queue<std::shared_ptr<ByteBuffer>>> freeBuffers_;

    // Track total allocated count per size class
    std::map<size_t, size_t> allocatedCount_;
    
    // Track pre-allocated count per size class (set during initialization)
    std::map<size_t, size_t> preAllocatedCount_;

    // Track currently in-use count per size class
    std::map<size_t, size_t> inUseCount_;

    // Track requested size for each buffer (key: buffer pointer, value: requested size)
    std::map<ByteBuffer*, size_t> requestedSizes_;

    // Mutex for thread safety
    mutable std::mutex poolMutex_;

    // DirectIoLib instance for aligned buffer allocation
    std::shared_ptr<DirectIoLib> directIoLib_;

    // Maximum memory limit (0 = no limit, reserved for future)
    size_t maxMemoryLimit_ = 0;

    // Initialization flag
    bool initialized_ = false;
};

#endif // PIXELS_GLOBALBYTEBUFFERPOOL_H
