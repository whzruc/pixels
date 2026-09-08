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

#ifdef PIXELS_ENABLE_SPDK

#include "physical/SpdkBufferPool.h"
#include "profiler/TimeProfiler.h"
#include "utils/ColumnSizeCSVReader.h"
#include "utils/ConfigFactory.h"
#include <stdexcept>

// ── Thread-local member definitions ────────────────────────────────────────

thread_local bool SpdkBufferPool::isInitialized = false;
// Start at 1 so the first Switch() lands on index 0 (same convention as BufferPool)
thread_local int  SpdkBufferPool::currBufferIdx = 1;
thread_local int  SpdkBufferPool::nextBufferIdx = 0;
thread_local int  SpdkBufferPool::colCount      = 0;
thread_local std::map<uint32_t, std::shared_ptr<ByteBuffer>> SpdkBufferPool::buffers[2];
thread_local std::map<uint32_t, uint64_t> SpdkBufferPool::nrBytes[2];

// ── Internal helper: resolve allocation size for one column ────────────────
//
// If a ColumnSizeCSVReader is provided and the column name appears in it,
// use the CSV value (pre-computed dataset-wide maximum).  Otherwise fall
// back to the per-file chunk size + 2 extra LBA blocks for alignment.
static uint64_t resolveAllocSize(const std::string& columnName,
                                 uint64_t           chunkBytes,
                                 ColumnSizeCSVReader* csv)
{
    uint64_t allocSize;
    if (csv) {
        try {
            // Treat the CSV as a sizing hint, not an unchecked guarantee: a
            // stale CSV must not make the DMA target smaller than this chunk.
            const uint64_t csvBytes = static_cast<uint64_t>(csv->get(columnName));
            allocSize = (csvBytes > chunkBytes ? csvBytes : chunkBytes) +
                        SPDK_POOL_EXTRA_SIZE;
        } catch (...) {
            allocSize = chunkBytes + SPDK_POOL_EXTRA_SIZE;
        }
    } else {
        allocSize = chunkBytes + SPDK_POOL_EXTRA_SIZE;
    }
    // Round up to LBA boundary (4 KiB).
    return ((allocSize + 4095ULL) / 4096ULL) * 4096ULL;
}

// ── Public API ──────────────────────────────────────────────────────────────

void SpdkBufferPool::Initialize(std::vector<uint32_t>    colIds,
                                 std::vector<uint64_t>    bytes,
                                 std::vector<std::string> columnNames)
{
    PROFILE_START("Spdk.BufferPool.Initialize.Total");
    if (colIds.size() != bytes.size())
        throw InvalidArgumentException(
            "SpdkBufferPool::Initialize: colIds/bytes size mismatch");

    // Load column-size CSV once (same property as BufferPool uses).
    std::unique_ptr<ColumnSizeCSVReader> csv;
    try {
        std::string csvPath =
            ConfigFactory::Instance().getProperty("pixel.column.size.path");
        if (!csvPath.empty())
            csv = std::make_unique<ColumnSizeCSVReader>(csvPath);
    } catch (...) {}

    if (!isInitialized) {
        PROFILE_START("Spdk.BufferPool.Initialize.Allocate");
        currBufferIdx = 0;
        nextBufferIdx = 1;

        for (int i = 0; i < (int)colIds.size(); i++) {
            uint32_t colId     = colIds.at(i);
            uint64_t allocSize = resolveAllocSize(columnNames.at(colId), bytes.at(i), csv.get());

            for (int idx = 0; idx < 2; idx++) {
                void* ptr = spdk_dma_malloc(allocSize, 4096, nullptr);
                if (!ptr)
                    throw std::runtime_error(
                        "SpdkBufferPool::Initialize: spdk_dma_malloc failed for colId=" +
                        std::to_string(colId) +
                        " size=" + std::to_string(allocSize) +
                        " (hugepages exhausted?)");

                buffers[idx][colId] = std::make_shared<ByteBuffer>(
                    static_cast<uint8_t*>(ptr),
                    static_cast<uint32_t>(allocSize),
                    ByteBuffer::AllocType::BY_SPDK_DMA);
            }
            nrBytes[0][colId] = allocSize;
            nrBytes[1][colId] = allocSize;
        }

        colCount      = (int)colIds.size();
        isInitialized = true;
        PROFILE_END("Spdk.BufferPool.Initialize.Allocate");
    } else {
        // Already initialized — verify the buffer set selected for this I/O.
        // The other set belongs to the file currently being decoded.  Its
        // chunk ByteBuffers are non-owning views, so releasing that set here
        // would leave the current reader with dangling pointers.
        for (int i = 0; i < (int)colIds.size(); i++) {
            uint32_t colId     = colIds.at(i);
            uint64_t allocSize = resolveAllocSize(columnNames.at(colId), bytes.at(i), csv.get());

            if (nrBytes[currBufferIdx].find(colId) == nrBytes[currBufferIdx].end() ||
                nrBytes[currBufferIdx][colId] < allocSize)
            {
                PROFILE_START("Spdk.BufferPool.Initialize.Grow");
                buffers[currBufferIdx].erase(colId);
                void* ptr = spdk_dma_malloc(allocSize, 4096, nullptr);
                if (!ptr)
                    throw std::runtime_error(
                        "SpdkBufferPool::Initialize: spdk_dma_malloc failed "
                        "(grow) for colId=" + std::to_string(colId) +
                        " size=" + std::to_string(allocSize));
                buffers[currBufferIdx][colId] = std::make_shared<ByteBuffer>(
                    static_cast<uint8_t*>(ptr),
                    static_cast<uint32_t>(allocSize),
                    ByteBuffer::AllocType::BY_SPDK_DMA);
                nrBytes[currBufferIdx][colId] = allocSize;
                PROFILE_END("Spdk.BufferPool.Initialize.Grow");
            }
        }
    }
    PROFILE_END("Spdk.BufferPool.Initialize.Total");
}

std::shared_ptr<ByteBuffer> SpdkBufferPool::GetBuffer(uint32_t colId)
{
    return buffers[currBufferIdx][colId];
}

std::shared_ptr<ByteBuffer> SpdkBufferPool::GetBufferAt(uint32_t colId, int bufIdx)
{
    return buffers[bufIdx][colId];
}

void SpdkBufferPool::Switch()
{
    currBufferIdx = 1 - currBufferIdx;
    nextBufferIdx = 1 - nextBufferIdx;
}

void SpdkBufferPool::Reset()
{
    // shared_ptr destructors will call ByteBuffer::~ByteBuffer which invokes
    // spdk_dma_free via the BY_SPDK_DMA AllocType path.
    for (int idx = 0; idx < 2; idx++)
        buffers[idx].clear();
    nrBytes[0].clear();
    nrBytes[1].clear();
    colCount      = 0;
    currBufferIdx = 1;
    nextBufferIdx = 0;
    isInitialized = false;
}

bool SpdkBufferPool::IsInitialized() { return isInitialized; }
int  SpdkBufferPool::GetCurrentBufferIdx() { return currBufferIdx; }
int  SpdkBufferPool::GetNextBufferIdx()    { return nextBufferIdx; }

#endif // PIXELS_ENABLE_SPDK
