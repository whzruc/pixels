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
#include "physical/GlobalByteBufferPool.h"
#include "profiler/TimeProfiler.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cstring>

GlobalByteBufferPool& GlobalByteBufferPool::Instance() {
    static GlobalByteBufferPool instance;
    return instance;
}

void GlobalByteBufferPool::Initialize(const std::string& configFilePath, int fsBlockSize, int numThreads) {
    std::lock_guard<std::mutex> lock(poolMutex_);
    
    if (initialized_) {
        std::cout << "GlobalByteBufferPool: Already initialized, skipping." << std::endl;
        return;
    }
    
    // Initialize DirectIoLib
    directIoLib_ = std::make_shared<DirectIoLib>(fsBlockSize);
    
    // Load configuration
    LoadConfiguration(configFilePath, numThreads);
    
    std::cout << "GlobalByteBufferPool: Initialized with " << sizeClasses_.size() 
              << " size classes, max buffer size: " << (maxBufferSize_ / 1024 / 1024) << " MB"
              << std::endl;
    
    initialized_ = true;
}

void GlobalByteBufferPool::LoadConfiguration(const std::string& configFilePath, int numThreads) {
    // Parse properties file
    auto properties = ParsePropertiesFile(configFilePath);
    
    // Load size classes
    std::map<int, size_t> sizeClassMap;  // index -> size
    std::map<int, int> multiplierMap;     // index -> multiplier
    
    for (const auto& pair : properties) {
        const std::string& key = pair.first;
        const std::string& value = pair.second;
        
        // Parse size classes: global.bufferpool.size.class.N
        if (key.find("global.bufferpool.size.class.") == 0) {
            int index = std::stoi(key.substr(std::string("global.bufferpool.size.class.").length()));
            size_t size = std::stoull(value);
            sizeClassMap[index] = size;
        }
        // Parse multipliers: global.bufferpool.prealloc.multiplier.N
        else if (key.find("global.bufferpool.prealloc.multiplier.") == 0) {
            int index = std::stoi(key.substr(std::string("global.bufferpool.prealloc.multiplier.").length()));
            int multiplier = std::stoi(value);
            multiplierMap[index] = multiplier;
        }
        // Parse max buffer size
        else if (key == "global.bufferpool.max.buffer.size") {
            maxBufferSize_ = std::stoull(value);
        }
        // Parse max memory limit
        else if (key == "global.bufferpool.max.memory.limit") {
            maxMemoryLimit_ = std::stoull(value);
        }
        // Parse stats enable flag
        else if (key == "global.bufferpool.enable.stats") {
            enableStats_ = (value == "true" || value == "1");
        }
    }
    
    // Build size classes vector (sorted)
    for (const auto& pair : sizeClassMap) {
        sizeClasses_.push_back(pair.second);
    }
    std::sort(sizeClasses_.begin(), sizeClasses_.end());
    
    // Pre-allocate buffers
    for (const auto& pair : sizeClassMap) {
        int index = pair.first;
        size_t sizeClass = pair.second;
        
        int multiplier = 1;
        auto it = multiplierMap.find(index);
        if (it != multiplierMap.end()) {
            multiplier = it->second;
        }
        
        int preAllocCount = numThreads * multiplier;
        
        std::cout << "GlobalByteBufferPool: Pre-allocating " << preAllocCount 
                  << " buffers of size " << (sizeClass / 1024 / 1024) << " MB" << std::endl;
        
        // Pre-allocate buffers for this size class
        for (int i = 0; i < preAllocCount; i++) {
            auto buffer = directIoLib_->allocateDirectBuffer(sizeClass);
            if (buffer) {
                freeBuffers_[sizeClass].push(buffer);
                allocatedCount_[sizeClass]++;
            }
        }
        
        // Record pre-allocated count for this size class
        preAllocatedCount_[sizeClass] = allocatedCount_[sizeClass];
        
        // Initialize in-use count
        inUseCount_[sizeClass] = 0;
    }
}

std::map<std::string, std::string> GlobalByteBufferPool::ParsePropertiesFile(const std::string& filePath) {
    std::map<std::string, std::string> properties;
    std::ifstream file(filePath);
    
    if (!file.is_open()) {
        throw InvalidArgumentException("GlobalByteBufferPool: Cannot open config file: " + filePath);
    }
    
    std::string line;
    while (std::getline(file, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        // Parse key=value
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            
            // Trim key and value
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            properties[key] = value;
        }
    }
    
    file.close();
    return properties;
}

std::shared_ptr<ByteBuffer> GlobalByteBufferPool::AcquireBuffer(size_t requestedSize) {
    // PROFILE_START("GlobalBufferPool.AcquireBuffer.Total");
    
    // PROFILE_START("GlobalBufferPool.AcquireBuffer.LockWait");
    std::lock_guard<std::mutex> lock(poolMutex_);
    // PROFILE_END("GlobalBufferPool.AcquireBuffer.LockWait");
    
    // PROFILE_START("GlobalBufferPool.AcquireBuffer.InLock");
    
    if (!initialized_) {
        throw InvalidArgumentException("GlobalByteBufferPool: Pool not initialized");
    }
    
    // Check if request exceeds max allowed size
    if (requestedSize > maxBufferSize_) {
        throw InvalidArgumentException(
            "GlobalByteBufferPool: Requested size " + std::to_string(requestedSize) +
            " exceeds max buffer size " + std::to_string(maxBufferSize_)
        );
    }
    
    // Find appropriate size class
    size_t sizeClass = FindSizeClass(requestedSize);
    
    if (sizeClass == 0) {
        throw InvalidArgumentException(
            "GlobalByteBufferPool: No suitable size class for requested size " + 
            std::to_string(requestedSize)
        );
    }
    
    std::shared_ptr<ByteBuffer> buffer;
    
    // Try to get from free queue
    if (!freeBuffers_[sizeClass].empty()) {
        // PROFILE_START("GlobalBufferPool.AcquireBuffer.GetFromQueue");
        buffer = freeBuffers_[sizeClass].front();
        freeBuffers_[sizeClass].pop();
        // PROFILE_END("GlobalBufferPool.AcquireBuffer.GetFromQueue");
    } else {
        // Allocate new buffer
        // PROFILE_START("GlobalBufferPool.AcquireBuffer.AllocateNew");
        buffer = AllocateNewBuffer(sizeClass);
        allocatedCount_[sizeClass]++;
        // PROFILE_END("GlobalBufferPool.AcquireBuffer.AllocateNew");
    }
    
    // Track statistics (only if enabled to reduce lock contention)
    if (enableStats_) {
        // PROFILE_START("GlobalBufferPool.AcquireBuffer.UpdateStats");
        // Track in-use count
        inUseCount_[sizeClass]++;
        
        // Track requested size for internal fragmentation calculation
        requestedSizes_[buffer.get()] = requestedSize;
        // PROFILE_END("GlobalBufferPool.AcquireBuffer.UpdateStats");
    }
    
    PROFILE_END("GlobalBufferPool.AcquireBuffer.InLock");
    PROFILE_END("GlobalBufferPool.AcquireBuffer.Total");
    
    return buffer;
}

void GlobalByteBufferPool::ReleaseBuffer(std::shared_ptr<ByteBuffer> buffer) {
    if (!buffer) {
        return;
    }
    
    PROFILE_START("GlobalBufferPool.ReleaseBuffer.Total");
    
    PROFILE_START("GlobalBufferPool.ReleaseBuffer.LockWait");
    std::lock_guard<std::mutex> lock(poolMutex_);
    PROFILE_END("GlobalBufferPool.ReleaseBuffer.LockWait");
    
    PROFILE_START("GlobalBufferPool.ReleaseBuffer.InLock");
    
    if (!initialized_) {
        PROFILE_END("GlobalBufferPool.ReleaseBuffer.InLock");
        PROFILE_END("GlobalBufferPool.ReleaseBuffer.Total");
        return;  // Pool already reset, just let buffer be destroyed
    }
    
    size_t bufferSize = buffer->size();
    
    // Find matching size class
    size_t sizeClass = 0;
    for (size_t sc : sizeClasses_) {
        if (bufferSize == sc) {
            sizeClass = sc;
            break;
        }
    }
    
    if (sizeClass == 0) {
        // Buffer size doesn't match any size class, just let it be destroyed
        PROFILE_END("GlobalBufferPool.ReleaseBuffer.InLock");
        PROFILE_END("GlobalBufferPool.ReleaseBuffer.Total");
        return;
    }
    
    // Reset buffer state before returning to pool
    PROFILE_START("GlobalBufferPool.ReleaseBuffer.ClearBuffer");
    buffer->clear();
    PROFILE_END("GlobalBufferPool.ReleaseBuffer.ClearBuffer");
    
    // Update statistics (only if enabled to reduce lock contention)
    if (enableStats_) {
        PROFILE_START("GlobalBufferPool.ReleaseBuffer.UpdateStats");
        // Remove requested size tracking
        requestedSizes_.erase(buffer.get());
        
        // Update in-use count
        if (inUseCount_[sizeClass] > 0) {
            inUseCount_[sizeClass]--;
        }
        PROFILE_END("GlobalBufferPool.ReleaseBuffer.UpdateStats");
    }
    
    // Return to free queue
    PROFILE_START("GlobalBufferPool.ReleaseBuffer.PushToQueue");
    freeBuffers_[sizeClass].push(buffer);
    PROFILE_END("GlobalBufferPool.ReleaseBuffer.PushToQueue");
    
    PROFILE_END("GlobalBufferPool.ReleaseBuffer.InLock");
    PROFILE_END("GlobalBufferPool.ReleaseBuffer.Total");
}

GlobalByteBufferPool::MemoryStats GlobalByteBufferPool::GetMemoryStats() const {
    std::lock_guard<std::mutex> lock(poolMutex_);
    
    MemoryStats stats;
    stats.totalAllocatedBytes = 0;
    stats.totalUsedBytes = 0;
    stats.totalRequestedBytes = 0;
    
    for (const auto& pair : allocatedCount_) {
        size_t sizeClass = pair.first;
        size_t count = pair.second;
        size_t totalBytes = sizeClass * count;
        
        stats.totalAllocatedBytes += totalBytes;
        stats.bufferCountPerClass[sizeClass] = count;
        
        // Get pre-allocated count
        size_t preAllocated = 0;
        auto preIt = preAllocatedCount_.find(sizeClass);
        if (preIt != preAllocatedCount_.end()) {
            preAllocated = preIt->second;
        }
        stats.preAllocatedCountPerClass[sizeClass] = preAllocated;
        
        // Calculate dynamically allocated count
        size_t dynamicAllocated = (count > preAllocated) ? (count - preAllocated) : 0;
        stats.dynamicAllocatedCountPerClass[sizeClass] = dynamicAllocated;
        
        // Statistics that require enableStats_ to be true
        if (enableStats_) {
            // Calculate used bytes
            size_t inUse = 0;
            auto it = inUseCount_.find(sizeClass);
            if (it != inUseCount_.end()) {
                inUse = it->second;
            }
            
            stats.totalUsedBytes += sizeClass * inUse;
            stats.inUseCountPerClass[sizeClass] = inUse;
        } else {
            // When stats disabled, in-use count is unavailable
            stats.inUseCountPerClass[sizeClass] = 0;
        }
    }
    
    // Calculate total requested bytes (only if stats enabled)
    if (enableStats_) {
        for (const auto& pair : requestedSizes_) {
            stats.totalRequestedBytes += pair.second;
        }
    }
    
    // Calculate external fragmentation ratio (allocated - used) / allocated
    if (enableStats_ && stats.totalAllocatedBytes > 0) {
        stats.fragmentationRatio = 
            static_cast<double>(stats.totalAllocatedBytes - stats.totalUsedBytes) / 
            stats.totalAllocatedBytes;
    } else {
        stats.fragmentationRatio = 0.0;
    }
    
    // Calculate internal fragmentation ratio (allocated - requested) / allocated
    if (enableStats_ && stats.totalAllocatedBytes > 0) {
        stats.internalFragmentationRatio = 
            static_cast<double>(stats.totalAllocatedBytes - stats.totalRequestedBytes) / 
            stats.totalAllocatedBytes;
    } else {
        stats.internalFragmentationRatio = 0.0;
    }
    
    return stats;
}

void GlobalByteBufferPool::Reset() {
    std::lock_guard<std::mutex> lock(poolMutex_);
    
    if (!initialized_) {
        return;
    }
    
    std::cout << "GlobalByteBufferPool: Resetting pool..." << std::endl;
    
    // Clear all free buffers
    for (auto& pair : freeBuffers_) {
        while (!pair.second.empty()) {
            pair.second.pop();
        }
    }
    freeBuffers_.clear();
    
    // Clear counters
    allocatedCount_.clear();
    inUseCount_.clear();
    requestedSizes_.clear();
    
    // Clear size classes
    sizeClasses_.clear();
    
    // Reset other state
    directIoLib_ = nullptr;
    maxBufferSize_ = 0;
    maxMemoryLimit_ = 0;
    // enableStats_ = ;
    initialized_ = false;
    
    std::cout << "GlobalByteBufferPool: Reset complete" << std::endl;
}

size_t GlobalByteBufferPool::FindSizeClass(size_t requestedSize) const {
    // Find the smallest size class >= requestedSize
    for (size_t sizeClass : sizeClasses_) {
        if (sizeClass >= requestedSize) {
            return sizeClass;
        }
    }
    return 0;  // No suitable size class
}

std::shared_ptr<ByteBuffer> GlobalByteBufferPool::AllocateNewBuffer(size_t sizeClass) {
    auto buffer = directIoLib_->allocateDirectBuffer(sizeClass);
    if (!buffer) {
        throw InvalidArgumentException(
            "GlobalByteBufferPool: Failed to allocate buffer of size " + 
            std::to_string(sizeClass)
        );
    }
    
    // Zero out the buffer
    std::memset(buffer->getPointer(), 0, buffer->size());
    
    return buffer;
}
