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

#include <cstdint>
#include <map>
#include <memory>
#include <queue>
#include <vector>
#include "physical/natives/ByteBuffer.h"
#include "physical/natives/DirectIoLib.h"
#include "exception/InvalidArgumentException.h"
#include "liburing.h"
#include "liburing/io_uring.h"

constexpr uint32_t DEFAULT_MAX_BUFFER_SLOTS = 1024;

class DynamicBufferPool
{
   public:
    static void Initialize(struct io_uring *ring, uint32_t maxSlots = DEFAULT_MAX_BUFFER_SLOTS);

    static std::shared_ptr<ByteBuffer> AllocateBuffer(uint32_t colId, uint64_t size);

    static std::shared_ptr<ByteBuffer> GetBuffer(uint32_t colId);

    static int GetBufferSlotIndex(uint32_t colId);

    static std::shared_ptr<ByteBuffer> GrowBuffer(uint32_t colId, uint64_t newSize);

    static void ReleaseBuffer(uint32_t colId);

    static bool IsInitialized();

    static uint32_t GetBufferCount();

    static uint32_t GetMaxSlots();

    static void Reset();

    static std::shared_ptr<DirectIoLib> GetDirectIoLib();

   private:
    DynamicBufferPool() = default;

    static int AllocateSlot();

    static void FreeSlot(uint32_t slotIndex);

    static bool UpdateBufferRegistration(uint32_t slotIndex, const std::shared_ptr<ByteBuffer> &buffer);

    static thread_local struct io_uring *ring;
    static thread_local struct iovec *iovecs;
    static thread_local uint32_t maxBufferSlots;
    static thread_local uint32_t currentUsedSlots;
    static thread_local bool isInitialized;

    static thread_local std::vector<std::shared_ptr<ByteBuffer>> bufferSlots;
    static thread_local std::queue<uint32_t> freeSlots;
    static thread_local std::map<uint32_t, uint32_t> colToSlot;
    static thread_local std::map<uint32_t, uint32_t> slotToCol;

    static thread_local std::shared_ptr<DirectIoLib> directIoLib;
    static thread_local std::shared_ptr<ByteBuffer> placeholderBuffer;
};

#endif  // PIXELS_DYNAMICBUFFERPOOL_H
