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
#ifndef PIXELS_DIRECTURINGRANDOMACCESSFILEDYNAMIC_H
#define PIXELS_DIRECTURINGRANDOMACCESSFILEDYNAMIC_H

#include "liburing.h"
#include "liburing/io_uring.h"
#include "physical/natives/DirectRandomAccessFile.h"
#include "physical/DynamicBufferPool.h"
#include "exception/InvalidArgumentException.h"
#include <memory>

class DirectUringRandomAccessFileDynamic : public DirectRandomAccessFile
{
   public:
    explicit DirectUringRandomAccessFileDynamic(const std::string &file);

    static void Initialize(uint32_t queueDepth = 4096, uint32_t maxBufferSlots = DEFAULT_MAX_BUFFER_SLOTS);

    static void Reset();

    std::shared_ptr<ByteBuffer> readAsync(int length, uint32_t colId);

    std::shared_ptr<ByteBuffer> readAsync(int length, std::shared_ptr<ByteBuffer> buffer, int slotIndex,
                                          int startOffset);

    void readAsyncSubmit(int count);

    void readAsyncComplete(int count);

    void readAsyncExecute(int count);

    static bool IsInitialized();

    ~DirectUringRandomAccessFileDynamic() = default;

   private:
    static thread_local struct io_uring *ring;
    static thread_local bool isInitialized;
    static thread_local uint32_t queueDepth;
};

#endif  // PIXELS_DIRECTURINGRANDOMACCESSFILEDYNAMIC_H
