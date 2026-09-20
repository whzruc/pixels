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
 * @create 2026-01-23
 */
#ifndef PIXELS_DYNAMICBUFFERPOOL_H
#define PIXELS_DYNAMICBUFFERPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <map>
#include <memory>
#include <mutex>
#include "physical/natives/ByteBuffer.h"
#include "physical/natives/DirectIoLib.h"
#include "exception/InvalidArgumentException.h"
#include "liburing.h"
#include "liburing/io_uring.h"

// Maximum number of buffer slots to pre-register (using sparse registration)
#define DEFAULT_MAX_BUFFER_SLOTS 1024

/**
 * DynamicBufferPool provides a scalable buffer pool implementation using io_uring's
 * advanced features: IORING_REGISTER_BUFFERS2, IORING_REGISTER_BUFFERS_UPDATE, 
 * and IORING_RSRC_REGISTER_SPARSE.
 * 
 * Key features:
 * - Sparse registration: Pre-register many slots without allocating actual buffers
 * - Dynamic allocation: Allocate buffers on-demand
 * - Dynamic updates: Add/remove buffers at runtime using update operations
 * - Automatic growth: Automatically expand when more buffers are needed
 */
class DynamicBufferPool {
public:
    /**
     * Initialize the buffer pool with io_uring ring
     * @param ring The io_uring instance to use
     * @param maxSlots Maximum number of buffer slots (default: 1024)
     */
    static void Initialize(struct io_uring* ring, uint32_t maxSlots = DEFAULT_MAX_BUFFER_SLOTS);
    
    /**
     * Allocate a new buffer for the specified column ID
     * @param colId Column identifier
     * @param size Buffer size in bytes
     * @return Allocated buffer
     */
    static std::shared_ptr<ByteBuffer> AllocateBuffer(uint32_t colId, uint64_t size);
    
    /**
     * Get buffer by column ID from current buffer set
     * @param colId Column identifier
     * @return Buffer or nullptr if not found
     */
    static std::shared_ptr<ByteBuffer> GetBuffer(uint32_t colId);
    
    /**
     * Get the buffer slot index for a column ID from current buffer set
     * @param colId Column identifier
     * @return Slot index, or -1 if not found
     */
    static int GetBufferSlotIndex(uint32_t colId);
    
    /**
     * Get buffer ID combining column index and current buffer set (for io_uring)
     * Similar to BufferPool::GetBufferId
     * @param index Column index
     * @return Buffer ID for io_uring operations
     */
    static int64_t GetBufferId(uint32_t index);
    
    /**
     * Switch between double buffers for async I/O
     * Similar to BufferPool::Switch
     */
    static void Switch();
    
    /**
     * Grow an existing buffer to a new size
     * @param colId Column identifier
     * @param newSize New buffer size
     * @return Resized buffer
     */
    static std::shared_ptr<ByteBuffer> GrowBuffer(uint32_t colId, uint64_t newSize);
    
    /**
     * Release a buffer and return its slot to the free pool
     * @param colId Column identifier
     */
    static void ReleaseBuffer(uint32_t colId);
    
    /**
     * Check if the pool has been initialized
     */
    static bool IsInitialized();
    
    /**
     * Get current buffer count
     */
    static uint32_t GetBufferCount();
    
    /**
     * Get maximum buffer slots
     */
    static uint32_t GetMaxSlots();
    
    /**
     * Reset the buffer pool (deallocate all buffers)
     */
    static void Reset();
    
    /**
     * Get DirectIoLib instance
     */
    static std::shared_ptr<DirectIoLib> GetDirectIoLib();

private:
    DynamicBufferPool() = default;
    
    // Friend declarations for direct access to buffer indices
    friend class DirectUringRandomAccessFileDynamic;
    
    /**
     * Allocate a free slot from the pool
     * @return Slot index, or -1 if no free slots available
     */
    static int AllocateSlot();
    
    /**
     * Free a slot and return it to the pool
     * @param slotIndex The slot to free
     */
    static void FreeSlot(uint32_t slotIndex);
    
    /**
     * Update io_uring buffer registration for a specific slot
     * @param slotIndex Slot to update
     * @param buffer Buffer to register
     * @return true if successful
     */
    static bool UpdateBufferRegistration(uint32_t slotIndex, std::shared_ptr<ByteBuffer> buffer);

    // Thread-local storage for per-thread buffer pools
    static thread_local struct io_uring* ring;
    static thread_local struct iovec* iovecs;
    static thread_local uint32_t maxBufferSlots;
    static thread_local uint32_t currentUsedSlots;
    static thread_local bool isInitialized;
    
    // Double buffer support for async I/O (similar to BufferPool)
    static thread_local int currBufferIdx;
    static thread_local int nextBufferIdx;
    static thread_local int colCount;
    
    // Buffer management structures - doubled for async support
    // Each buffer set has its own slots, mapping, and registration
    static thread_local std::vector<std::shared_ptr<ByteBuffer>> bufferSlots[2];
    static thread_local std::queue<uint32_t> freeSlots[2];
    static thread_local std::map<uint32_t, uint32_t> colToSlot[2];  // colId -> slotIndex
    static thread_local std::map<uint32_t, uint32_t> slotToCol[2];  // slotIndex -> colId
    static thread_local uint32_t currentUsedSlots_arr[2];
    
    // DirectIoLib for aligned buffer allocation
    static thread_local std::shared_ptr<DirectIoLib> directIoLib;
};

#endif // PIXELS_DYNAMICBUFFERPOOL_H
