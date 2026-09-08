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

#pragma once
#ifdef PIXELS_ENABLE_SPDK

#include <spdk/env.h>
#include <vector>
#include <map>
#include <string>
#include <memory>
#include <cstdint>
#include "physical/natives/ByteBuffer.h"
#include "exception/InvalidArgumentException.h"

// Extra bytes added to each buffer to absorb LBA-alignment read-ahead.
// SPDK reads in 4 KiB blocks; at most one extra block can be read for
// alignment purposes on each side → 2 * 4096 is a safe margin.
#define SPDK_POOL_EXTRA_SIZE (2 * 4096)

/**
 * SpdkBufferPool — thread-local pool of spdk_dma_malloc'd buffers.
 *
 * Mirrors the double-buffer design of BufferPool but allocates every
 * buffer from SPDK hugepage memory so they can be passed directly as
 * the NVMe DMA target (zero-copy, no per-read spdk_dma_malloc/free).
 *
 * Lifecycle (per worker thread):
 *   Initialize()  — first call allocates and owns the DMA buffers
 *   GetBufferAt() — returns the buffer for (colId, bufIdx)
 *   Switch()      — swaps curr/next buffer index (double-buffering)
 *   Reset()       — frees all DMA buffers via spdk_dma_free
 */
class SpdkBufferPool {
public:
    static void Initialize(std::vector<uint32_t> colIds,
                           std::vector<uint64_t> bytes,
                           std::vector<std::string> columnNames);

    static std::shared_ptr<ByteBuffer> GetBuffer(uint32_t colId);
    static std::shared_ptr<ByteBuffer> GetBufferAt(uint32_t colId, int bufIdx);

    static void Switch();
    static void Reset();
    static bool IsInitialized();
    static int  GetCurrentBufferIdx();
    static int  GetNextBufferIdx();

private:
    SpdkBufferPool() = default;

    static thread_local bool isInitialized;
    static thread_local int  currBufferIdx;
    static thread_local int  nextBufferIdx;
    static thread_local int  colCount;
    // Two buffer sets for double buffering (index 0 and 1)
    static thread_local std::map<uint32_t, std::shared_ptr<ByteBuffer>> buffers[2];
    // Capacity is tracked independently for the two buffer sets.  Growing the
    // set selected for the next I/O must never release the other set: its
    // ByteBuffer views can still be consumed by the current reader.
    static thread_local std::map<uint32_t, uint64_t> nrBytes[2];
};

#endif // PIXELS_ENABLE_SPDK
