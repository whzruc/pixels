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
#include "physical/DynamicBufferPool.h"
#include "physical/BufferPoolStats.h"
#include "utils/ConfigFactory.h"
#include <cstring>

// Thread-local static member initialization
thread_local struct io_uring* DynamicBufferPool::ring = nullptr;
thread_local struct iovec* DynamicBufferPool::iovecs = nullptr;
thread_local uint32_t DynamicBufferPool::maxBufferSlots = 0;
thread_local uint32_t DynamicBufferPool::currentUsedSlots = 0;
thread_local bool DynamicBufferPool::isInitialized = false;

// Double buffer support - initialize currBufferIdx to 1, will be 0 after first Switch
thread_local int DynamicBufferPool::currBufferIdx = 1;
thread_local int DynamicBufferPool::nextBufferIdx = 0;
thread_local int DynamicBufferPool::colCount = 0;

// Double buffer arrays
thread_local std::vector<std::shared_ptr<ByteBuffer>> DynamicBufferPool::bufferSlots[2];
thread_local std::queue<uint32_t> DynamicBufferPool::freeSlots[2];
thread_local std::map<uint32_t, uint32_t> DynamicBufferPool::colToSlot[2];
thread_local std::map<uint32_t, uint32_t> DynamicBufferPool::slotToCol[2];
thread_local uint32_t DynamicBufferPool::currentUsedSlots_arr[2] = {0, 0};
thread_local std::shared_ptr<DirectIoLib> DynamicBufferPool::directIoLib = nullptr;

void DynamicBufferPool::Initialize(struct io_uring* uringRing, uint32_t maxSlots) {
    if (isInitialized) {
        return;
    }

    if (uringRing == nullptr) {
        throw InvalidArgumentException("DynamicBufferPool::Initialize: io_uring ring cannot be null");
    }

    ring = uringRing;
    maxBufferSlots = maxSlots;
    currentUsedSlots = 0;

    // Set buffer indexes: currBufferIdx starts at 0, nextBufferIdx at 1
    currBufferIdx = 0;
    nextBufferIdx = 1;
    colCount = 0;

    // Initialize DirectIoLib for aligned buffer allocation
    int fsBlockSize = 4096; // Default value
    try {
        fsBlockSize = std::stoi(ConfigFactory::Instance().getProperty("localfs.block.size"));
    } catch (...) {
        // Use default if config not available
    }
    directIoLib = std::make_shared<DirectIoLib>(fsBlockSize);

    // Allocate iovec array for all slots
    iovecs = (struct iovec*)calloc(maxSlots, sizeof(struct iovec));
    if (iovecs == nullptr) {
        throw InvalidArgumentException("DynamicBufferPool::Initialize: failed to allocate iovecs");
    }

    // Initialize all iovecs to null (sparse registration)
    for (uint32_t i = 0; i < maxSlots; i++) {
        iovecs[i].iov_base = nullptr;
        iovecs[i].iov_len = 0;
    }

    // Initialize both buffer sets with separate slot ranges
    // Buffer set 0: uses slots 0 to (maxSlots/2 - 1)
    // Buffer set 1: uses slots maxSlots/2 to (maxSlots - 1)
    uint32_t slotsPerSet = maxSlots / 2;

    for (int idx = 0; idx < 2; idx++) {
        bufferSlots[idx].resize(maxSlots, nullptr);

        // Initialize free slots for each buffer set with its own range
        uint32_t slotStart = idx * slotsPerSet;
        uint32_t slotEnd = slotStart + slotsPerSet;

        for (uint32_t i = slotStart; i < slotEnd; i++) {
            freeSlots[idx].push(i);
        }
        currentUsedSlots_arr[idx] = 0;
    }

    // Register buffers using sparse registration
    // Note: io_uring_register_buffers with all null entries creates sparse registration
    int ret = io_uring_register_buffers(ring, iovecs, maxSlots);
    if (ret != 0) {
        free(iovecs);
        iovecs = nullptr;
        throw InvalidArgumentException(
            "DynamicBufferPool::Initialize: failed to register buffers (sparse), error: " +
            std::to_string(ret)
        );
    }

    isInitialized = true;
}

std::shared_ptr<ByteBuffer> DynamicBufferPool::AllocateBuffer(uint32_t colId, uint64_t size) {
    if (!isInitialized) {
        throw InvalidArgumentException("DynamicBufferPool::AllocateBuffer: pool not initialized");
    }

    // Check if buffer already exists for this colId in current buffer set
    if (colToSlot[currBufferIdx].find(colId) != colToSlot[currBufferIdx].end()) {
        throw InvalidArgumentException(
            "DynamicBufferPool::AllocateBuffer: buffer already exists for colId " +
            std::to_string(colId)
        );
    }

    // Allocate a free slot from current buffer set
    int slotIndex = AllocateSlot();
    if (slotIndex < 0) {
        throw InvalidArgumentException("DynamicBufferPool::AllocateBuffer: no free slots available");
    }

    auto buffer = directIoLib->allocateDirectBuffer(size, false);
    if (buffer == nullptr) {
        FreeSlot(slotIndex);
        throw InvalidArgumentException("DynamicBufferPool::AllocateBuffer: failed to allocate buffer");
    }
    memset(buffer->getPointer(), 0, buffer->size());

    // Store buffer in current buffer set slot
    bufferSlots[currBufferIdx][slotIndex] = buffer;

    // Update mappings for current buffer set
    colToSlot[currBufferIdx][colId] = slotIndex;
    slotToCol[currBufferIdx][slotIndex] = colId;

    // Track column count
    if (colId >= static_cast<uint32_t>(colCount)) {
        colCount = colId + 1;
    }

    // Update io_uring buffer registration
    if (!UpdateBufferRegistration(slotIndex, buffer)) {
        // Rollback on failure
        bufferSlots[currBufferIdx][slotIndex] = nullptr;
        colToSlot[currBufferIdx].erase(colId);
        slotToCol[currBufferIdx].erase(slotIndex);
        FreeSlot(slotIndex);
        throw InvalidArgumentException("DynamicBufferPool::AllocateBuffer: failed to update buffer registration");
    }

    currentUsedSlots_arr[currBufferIdx]++;
    currentUsedSlots = currentUsedSlots_arr[currBufferIdx];
    BufferPoolStats::Instance().RecordAllocation(BufferPoolStatsMode::Dynamic, buffer->size());
    BufferPoolStats::Instance().RecordRegistrationUpdate(
        BufferPoolStatsMode::Dynamic, 0, buffer->size());

    return buffer;
}

std::shared_ptr<ByteBuffer> DynamicBufferPool::GetBuffer(uint32_t colId) {
    auto it = colToSlot[currBufferIdx].find(colId);
    if (it == colToSlot[currBufferIdx].end()) {
        return nullptr;
    }

    uint32_t slotIndex = it->second;
    return bufferSlots[currBufferIdx][slotIndex];
}

pixels::SelectiveBufferScheduler::Capacity DynamicBufferPool::GetCapacities() {
    pixels::SelectiveBufferScheduler::Capacity result;
    for (int idx = 0; idx < 2; ++idx) {
        for (const auto &entry : colToSlot[idx]) {
            const auto &buffer = bufferSlots[idx][entry.second];
            if (buffer) result[idx][entry.first] = buffer->size();
        }
    }
    return result;
}

int DynamicBufferPool::GetBufferSlotIndex(uint32_t colId) {
    auto it = colToSlot[currBufferIdx].find(colId);
    if (it == colToSlot[currBufferIdx].end()) {
        return -1;
    }
    return static_cast<int>(it->second);
}

int64_t DynamicBufferPool::GetBufferId(uint32_t index) {
    // Similar to BufferPool::GetBufferId: combine index with buffer set
    return index + currBufferIdx * colCount;
}

void DynamicBufferPool::Switch() {
    // Switch between buffer sets for double buffering
    currBufferIdx = 1 - currBufferIdx;
    nextBufferIdx = 1 - nextBufferIdx;
    currentUsedSlots = currentUsedSlots_arr[currBufferIdx];
}

std::shared_ptr<ByteBuffer> DynamicBufferPool::GrowBuffer(uint32_t colId, uint64_t newSize) {
    if (!isInitialized) {
        throw InvalidArgumentException("DynamicBufferPool::GrowBuffer: pool not initialized");
    }

    auto it = colToSlot[currBufferIdx].find(colId);
    if (it == colToSlot[currBufferIdx].end()) {
        throw InvalidArgumentException("DynamicBufferPool::GrowBuffer: buffer not found for colId " + std::to_string(colId));
    }

    uint32_t slotIndex = it->second;
    auto oldBuffer = bufferSlots[currBufferIdx][slotIndex];

    if (oldBuffer == nullptr) {
        throw InvalidArgumentException("DynamicBufferPool::GrowBuffer: buffer slot is null");
    }

    if (newSize <= oldBuffer->size()) {
        BufferPoolStats::Instance().RecordReuse(BufferPoolStatsMode::Dynamic);
        return oldBuffer;
    }

    auto newBuffer = directIoLib->allocateDirectBuffer(newSize, false);
    if (newBuffer == nullptr) {
        throw InvalidArgumentException("DynamicBufferPool::GrowBuffer: failed to allocate new buffer");
    }

    // Update buffer slot in current buffer set
    bufferSlots[currBufferIdx][slotIndex] = newBuffer;

    // Update io_uring registration
    if (!UpdateBufferRegistration(slotIndex, newBuffer)) {
        // Rollback on failure
        bufferSlots[currBufferIdx][slotIndex] = oldBuffer;
        throw InvalidArgumentException("DynamicBufferPool::GrowBuffer: failed to update buffer registration");
    }
    BufferPoolStats::Instance().RecordFree(BufferPoolStatsMode::Dynamic, oldBuffer->size());
    BufferPoolStats::Instance().RecordAllocation(BufferPoolStatsMode::Dynamic, newBuffer->size());
    BufferPoolStats::Instance().RecordRegistrationUpdate(
        BufferPoolStatsMode::Dynamic, oldBuffer->size(), newBuffer->size());
    BufferPoolStats::Instance().RecordGrowth(BufferPoolStatsMode::Dynamic);

    return newBuffer;
}

void DynamicBufferPool::ReleaseBuffer(uint32_t colId) {
    auto it = colToSlot[currBufferIdx].find(colId);
    if (it == colToSlot[currBufferIdx].end()) {
        return; // Already released or never allocated
    }

    uint32_t slotIndex = it->second;
    const auto buffer = bufferSlots[currBufferIdx][slotIndex];
    const uint64_t bufferSize = buffer ? buffer->size() : 0;

    // Clear buffer slot in current buffer set
    bufferSlots[currBufferIdx][slotIndex] = nullptr;

    // Update iovec to null (unregister from io_uring)
    iovecs[slotIndex].iov_base = nullptr;
    iovecs[slotIndex].iov_len = 0;

    // Update io_uring registration (set to null)
    struct iovec nullIov;
    nullIov.iov_base = nullptr;
    nullIov.iov_len = 0;
    io_uring_register_buffers_update_tag(ring, slotIndex, &nullIov, NULL, 1);

    // Remove mappings from current buffer set
    slotToCol[currBufferIdx].erase(slotIndex);
    colToSlot[currBufferIdx].erase(colId);

    // Return slot to free pool of current buffer set
    FreeSlot(slotIndex);
    BufferPoolStats::Instance().RecordUnregistration(BufferPoolStatsMode::Dynamic, bufferSize);
    BufferPoolStats::Instance().RecordFree(BufferPoolStatsMode::Dynamic, bufferSize);

    if (currentUsedSlots_arr[currBufferIdx] > 0) {
        currentUsedSlots_arr[currBufferIdx]--;
    }
    currentUsedSlots = currentUsedSlots_arr[currBufferIdx];
}

bool DynamicBufferPool::IsInitialized() {
    return isInitialized;
}

uint32_t DynamicBufferPool::GetBufferCount() {
    return currentUsedSlots;
}

uint32_t DynamicBufferPool::GetMaxSlots() {
    return maxBufferSlots;
}

void DynamicBufferPool::Reset() {
    if (!isInitialized) {
        return;
    }

    uint64_t allocatedBytes = 0;
    for (const auto &set : bufferSlots)
        for (const auto &buffer : set)
            if (buffer) allocatedBytes += buffer->size();

    // Unregister buffers from io_uring
    if (ring != nullptr) {
        io_uring_unregister_buffers(ring);
    }
    BufferPoolStats::Instance().RecordUnregistration(
        BufferPoolStatsMode::Dynamic, allocatedBytes);
    for (const auto &set : bufferSlots)
        for (const auto &buffer : set)
            if (buffer)
                BufferPoolStats::Instance().RecordFree(
                    BufferPoolStatsMode::Dynamic, buffer->size());

    // Clear all data structures for both buffer sets
    for (int idx = 0; idx < 2; idx++) {
        bufferSlots[idx].clear();
        while (!freeSlots[idx].empty()) {
            freeSlots[idx].pop();
        }
        colToSlot[idx].clear();
        slotToCol[idx].clear();
        currentUsedSlots_arr[idx] = 0;
    }

    // Free iovecs
    if (iovecs != nullptr) {
        free(iovecs);
        iovecs = nullptr;
    }

    // Reset state
    ring = nullptr;
    maxBufferSlots = 0;
    currentUsedSlots = 0;
    currBufferIdx = 1;
    nextBufferIdx = 0;
    colCount = 0;
    isInitialized = false;
    directIoLib = nullptr;
}

std::shared_ptr<DirectIoLib> DynamicBufferPool::GetDirectIoLib() {
    return directIoLib;
}

// Private methods

int DynamicBufferPool::AllocateSlot() {
    if (freeSlots[currBufferIdx].empty()) {
        return -1; // No free slots available
    }

    uint32_t slotIndex = freeSlots[currBufferIdx].front();
    freeSlots[currBufferIdx].pop();

    // Slot index is already in the correct range for the current buffer set
    return static_cast<int>(slotIndex);
}

void DynamicBufferPool::FreeSlot(uint32_t slotIndex) {
    if (slotIndex >= maxBufferSlots) {
        return;
    }
    freeSlots[currBufferIdx].push(slotIndex);
}

bool DynamicBufferPool::UpdateBufferRegistration(uint32_t slotIndex, std::shared_ptr<ByteBuffer> buffer) {
    if (slotIndex >= maxBufferSlots || buffer == nullptr) {
        return false;
    }

    // Update iovec
    iovecs[slotIndex].iov_base = buffer->getPointer();
    iovecs[slotIndex].iov_len = buffer->size();

    // Use io_uring_register_buffers_update_tag to dynamically update the buffer
    // This updates a single buffer at the specified offset
    int ret = io_uring_register_buffers_update_tag(ring, slotIndex, &iovecs[slotIndex], NULL, 1);

    if (ret < 0) {
        // Rollback iovec on failure
        iovecs[slotIndex].iov_base = nullptr;
        iovecs[slotIndex].iov_len = 0;
        return false;
    }

    return true;
}
