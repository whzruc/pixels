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
#include <cerrno>
#include <cstring>

thread_local struct io_uring *DynamicBufferPool::ring = nullptr;
thread_local struct iovec *DynamicBufferPool::iovecs = nullptr;
thread_local uint32_t DynamicBufferPool::maxBufferSlots = 0;
thread_local uint32_t DynamicBufferPool::currentUsedSlots = 0;
thread_local bool DynamicBufferPool::isInitialized = false;
thread_local std::vector<std::shared_ptr<ByteBuffer>> DynamicBufferPool::bufferSlots;
thread_local std::queue<uint32_t> DynamicBufferPool::freeSlots;
thread_local std::map<uint32_t, uint32_t> DynamicBufferPool::colToSlot;
thread_local std::map<uint32_t, uint32_t> DynamicBufferPool::slotToCol;
thread_local std::shared_ptr<DirectIoLib> DynamicBufferPool::directIoLib = nullptr;
thread_local std::shared_ptr<ByteBuffer> DynamicBufferPool::placeholderBuffer = nullptr;

void DynamicBufferPool::Initialize(struct io_uring *uringRing, uint32_t maxSlots)
{
    if (isInitialized)
    {
        return;
    }

    if (uringRing == nullptr)
    {
        throw InvalidArgumentException("DynamicBufferPool::Initialize: io_uring ring cannot be null");
    }

    ring = uringRing;
    maxBufferSlots = maxSlots;
    currentUsedSlots = 0;

    if (maxSlots == 0)
    {
        throw InvalidArgumentException("DynamicBufferPool::Initialize: max slots must be positive");
    }

    int fsBlockSize = std::stoi(ConfigFactory::Instance().getProperty("localfs.block.size", "4096"));
    directIoLib = std::make_shared<DirectIoLib>(fsBlockSize);

    iovecs = static_cast<struct iovec *>(calloc(maxSlots, sizeof(struct iovec)));
    if (iovecs == nullptr)
    {
        throw InvalidArgumentException("DynamicBufferPool::Initialize: failed to allocate iovecs");
    }

    for (uint32_t i = 0; i < maxSlots; i++)
    {
        iovecs[i].iov_base = nullptr;
        iovecs[i].iov_len = 0;
    }

    bufferSlots.resize(maxSlots, nullptr);
    for (uint32_t i = 0; i < maxSlots; i++)
    {
        freeSlots.push(i);
    }

    int ret = io_uring_register_buffers_sparse(ring, maxSlots);
    if (ret == -EINVAL || ret == -EOPNOTSUPP)
    {
        placeholderBuffer = directIoLib->allocateDirectBuffer(fsBlockSize, false);
        for (uint32_t i = 0; i < maxSlots; i++)
        {
            iovecs[i].iov_base = placeholderBuffer->getPointer();
            iovecs[i].iov_len = placeholderBuffer->size();
        }
        ret = io_uring_register_buffers(ring, iovecs, maxSlots);
    }
    if (ret != 0)
    {
        free(iovecs);
        iovecs = nullptr;
        throw InvalidArgumentException("DynamicBufferPool::Initialize: failed to register sparse buffers, error: " +
                                       std::to_string(ret));
    }

    isInitialized = true;
}

std::shared_ptr<ByteBuffer> DynamicBufferPool::AllocateBuffer(uint32_t colId, uint64_t size)
{
    if (!isInitialized)
    {
        throw InvalidArgumentException("DynamicBufferPool::AllocateBuffer: pool not initialized");
    }

    if (colToSlot.find(colId) != colToSlot.end())
    {
        throw InvalidArgumentException("DynamicBufferPool::AllocateBuffer: buffer already exists for colId " +
                                       std::to_string(colId));
    }

    int slotIndex = AllocateSlot();
    if (slotIndex < 0)
    {
        throw InvalidArgumentException("DynamicBufferPool::AllocateBuffer: no free slots available");
    }

    auto buffer = directIoLib->allocateDirectBuffer(size, false);
    if (buffer == nullptr)
    {
        FreeSlot(slotIndex);
        throw InvalidArgumentException("DynamicBufferPool::AllocateBuffer: failed to allocate buffer");
    }

    memset(buffer->getPointer(), 0, buffer->size());
    bufferSlots[slotIndex] = buffer;
    colToSlot[colId] = slotIndex;
    slotToCol[slotIndex] = colId;
    if (!UpdateBufferRegistration(slotIndex, buffer))
    {
        bufferSlots[slotIndex] = nullptr;
        colToSlot.erase(colId);
        slotToCol.erase(slotIndex);
        FreeSlot(slotIndex);
        throw InvalidArgumentException("DynamicBufferPool::AllocateBuffer: failed to update buffer registration");
    }

    currentUsedSlots++;
    BufferPoolStats::Instance().RecordAllocation(BufferPoolStatsMode::Dynamic, buffer->size());
    BufferPoolStats::Instance().RecordRegistrationUpdate(BufferPoolStatsMode::Dynamic, 0, buffer->size());

    return buffer;
}

std::shared_ptr<ByteBuffer> DynamicBufferPool::GetBuffer(uint32_t colId)
{
    auto it = colToSlot.find(colId);
    if (it == colToSlot.end())
    {
        return nullptr;
    }

    uint32_t slotIndex = it->second;
    return bufferSlots[slotIndex];
}

int DynamicBufferPool::GetBufferSlotIndex(uint32_t colId)
{
    auto it = colToSlot.find(colId);
    if (it == colToSlot.end())
    {
        return -1;
    }
    return static_cast<int>(it->second);
}

std::shared_ptr<ByteBuffer> DynamicBufferPool::GrowBuffer(uint32_t colId, uint64_t newSize)
{
    if (!isInitialized)
    {
        throw InvalidArgumentException("DynamicBufferPool::GrowBuffer: pool not initialized");
    }

    auto it = colToSlot.find(colId);
    if (it == colToSlot.end())
    {
        throw InvalidArgumentException("DynamicBufferPool::GrowBuffer: buffer not found for colId " +
                                       std::to_string(colId));
    }

    uint32_t slotIndex = it->second;
    auto oldBuffer = bufferSlots[slotIndex];

    if (oldBuffer == nullptr)
    {
        throw InvalidArgumentException("DynamicBufferPool::GrowBuffer: buffer slot is null");
    }

    if (newSize <= oldBuffer->size())
    {
        BufferPoolStats::Instance().RecordReuse(BufferPoolStatsMode::Dynamic);
        return oldBuffer;
    }

    auto newBuffer = directIoLib->allocateDirectBuffer(newSize, false);
    if (newBuffer == nullptr)
    {
        throw InvalidArgumentException("DynamicBufferPool::GrowBuffer: failed to allocate new buffer");
    }

    memcpy(newBuffer->getPointer(), oldBuffer->getPointer(), oldBuffer->size());
    memset(static_cast<uint8_t *>(newBuffer->getPointer()) + oldBuffer->size(), 0, newSize - oldBuffer->size());
    if (!UpdateBufferRegistration(slotIndex, newBuffer))
    {
        throw InvalidArgumentException("DynamicBufferPool::GrowBuffer: failed to update buffer registration");
    }
    bufferSlots[slotIndex] = newBuffer;
    BufferPoolStats::Instance().RecordFree(BufferPoolStatsMode::Dynamic, oldBuffer->size());
    BufferPoolStats::Instance().RecordAllocation(BufferPoolStatsMode::Dynamic, newBuffer->size());
    BufferPoolStats::Instance().RecordRegistrationUpdate(BufferPoolStatsMode::Dynamic, oldBuffer->size(),
                                                         newBuffer->size());
    BufferPoolStats::Instance().RecordGrowth(BufferPoolStatsMode::Dynamic);

    return newBuffer;
}

void DynamicBufferPool::ReleaseBuffer(uint32_t colId)
{
    auto it = colToSlot.find(colId);
    if (it == colToSlot.end())
    {
        return;
    }

    uint32_t slotIndex = it->second;
    uint64_t bufferSize = bufferSlots[slotIndex] == nullptr ? 0 : bufferSlots[slotIndex]->size();

    struct iovec nullIov;
    nullIov.iov_base = nullptr;
    nullIov.iov_len = 0;
    int ret = io_uring_register_buffers_update_tag(ring, slotIndex, &nullIov, nullptr, 1);
    if (ret != 1)
    {
        throw InvalidArgumentException("DynamicBufferPool::ReleaseBuffer: failed to unregister buffer slot " +
                                       std::to_string(slotIndex));
    }

    iovecs[slotIndex] = nullIov;
    bufferSlots[slotIndex] = nullptr;
    slotToCol.erase(slotIndex);
    colToSlot.erase(colId);
    FreeSlot(slotIndex);
    BufferPoolStats::Instance().RecordUnregistration(BufferPoolStatsMode::Dynamic, bufferSize);
    BufferPoolStats::Instance().RecordFree(BufferPoolStatsMode::Dynamic, bufferSize);

    if (currentUsedSlots > 0)
    {
        currentUsedSlots--;
    }
}

bool DynamicBufferPool::IsInitialized() { return isInitialized; }

uint32_t DynamicBufferPool::GetBufferCount() { return currentUsedSlots; }

uint32_t DynamicBufferPool::GetMaxSlots() { return maxBufferSlots; }

void DynamicBufferPool::Reset()
{
    if (!isInitialized)
    {
        return;
    }

    uint64_t allocatedBytes = 0;
    for (const auto &buffer : bufferSlots)
    {
        if (buffer != nullptr)
        {
            allocatedBytes += buffer->size();
        }
    }
    if (ring != nullptr)
    {
        io_uring_unregister_buffers(ring);
    }
    BufferPoolStats::Instance().RecordUnregistration(BufferPoolStatsMode::Dynamic, allocatedBytes);
    for (const auto &buffer : bufferSlots)
    {
        if (buffer != nullptr)
        {
            BufferPoolStats::Instance().RecordFree(BufferPoolStatsMode::Dynamic, buffer->size());
        }
    }

    bufferSlots.clear();
    while (!freeSlots.empty())
    {
        freeSlots.pop();
    }
    colToSlot.clear();
    slotToCol.clear();

    if (iovecs != nullptr)
    {
        free(iovecs);
        iovecs = nullptr;
    }

    ring = nullptr;
    maxBufferSlots = 0;
    currentUsedSlots = 0;
    isInitialized = false;
    directIoLib = nullptr;
    placeholderBuffer = nullptr;
}

std::shared_ptr<DirectIoLib> DynamicBufferPool::GetDirectIoLib() { return directIoLib; }

int DynamicBufferPool::AllocateSlot()
{
    if (freeSlots.empty())
    {
        return -1;
    }

    uint32_t slotIndex = freeSlots.front();
    freeSlots.pop();
    return static_cast<int>(slotIndex);
}

void DynamicBufferPool::FreeSlot(uint32_t slotIndex)
{
    if (slotIndex >= maxBufferSlots)
    {
        return;
    }
    freeSlots.push(slotIndex);
}

bool DynamicBufferPool::UpdateBufferRegistration(uint32_t slotIndex, const std::shared_ptr<ByteBuffer> &buffer)
{
    if (slotIndex >= maxBufferSlots || buffer == nullptr)
    {
        return false;
    }

    struct iovec newIovec;
    newIovec.iov_base = buffer->getPointer();
    newIovec.iov_len = buffer->size();
    int ret = io_uring_register_buffers_update_tag(ring, slotIndex, &newIovec, nullptr, 1);
    if (ret != 1)
    {
        return false;
    }
    iovecs[slotIndex] = newIovec;
    return true;
}
