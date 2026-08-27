/*
 * Copyright 2026 PixelsDB.
 *
 * This file is part of Pixels.
 *
 * Pixels is free software: you can redistribute it and/or modify
 * it under the terms of the Affero GNU General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#include "physical/GlobalStaticBufferPool.h"
#include "exception/InvalidArgumentException.h"
#include "utils/ColumnSizeCSVReader.h"
#include "utils/ConfigFactory.h"

#include <cstring>
#ifdef __linux__
#include <sys/mman.h>
#endif

GlobalStaticBufferPool &GlobalStaticBufferPool::Instance()
{
    static GlobalStaticBufferPool instance;
    return instance;
}

GlobalStaticBufferPool::~GlobalStaticBufferPool() { Reset(); }

void GlobalStaticBufferPool::Initialize(const std::string &columnSizePath, int blockSize, int threadCount)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (initialized)
    {
        return;
    }
    if (threadCount <= 0)
    {
        throw InvalidArgumentException("GlobalStaticBufferPool::Initialize: thread count must be positive");
    }

    ColumnSizeCSVReader sizeReader(columnSizePath);
    const auto &columnSizes = sizeReader.getAllColumnSizes();
    if (columnSizes.empty())
    {
        throw InvalidArgumentException("GlobalStaticBufferPool::Initialize: column size file is empty");
    }

    maxThreads = threadCount;
    directIoLib = std::make_shared<DirectIoLib>(blockSize);
    bool useHugePage = ConfigFactory::Instance().getProperty("pixel.static.buffer.hugepage", "false") == "true";
    int columnIndex = 0;
    for (const auto &entry : columnSizes)
    {
        columnNames.emplace_back(entry.first);
        columnIndexes.emplace(entry.first, columnIndex++);
        auto &columnBuffers = buffers[entry.first];
        columnBuffers.resize(maxThreads);
        for (int threadId = 0; threadId < maxThreads; threadId++)
        {
            columnBuffers[threadId].resize(2);
            for (int bufferId = 0; bufferId < 2; bufferId++)
            {
                auto buffer = directIoLib->allocateDirectBuffer(entry.second, false);
#if defined(__linux__) && defined(MADV_HUGEPAGE)
                if (useHugePage && madvise(buffer->getPointer(), buffer->size(), MADV_HUGEPAGE) != 0)
                {
                    throw InvalidArgumentException("GlobalStaticBufferPool::Initialize: MADV_HUGEPAGE failed");
                }
#else
                if (useHugePage)
                {
                    throw InvalidArgumentException(
                        "GlobalStaticBufferPool::Initialize: HugePage advice is unsupported on this platform");
                }
#endif
                std::memset(buffer->getPointer(), 0, buffer->size());
                columnBuffers[threadId][bufferId] = buffer;
            }
        }
    }

    rings.resize(maxThreads, nullptr);
    for (int threadId = 0; threadId < maxThreads; threadId++)
    {
        rings[threadId] = new io_uring();
        int ret = io_uring_queue_init(4096, rings[threadId], 0);
        if (ret < 0)
        {
            throw InvalidArgumentException("GlobalStaticBufferPool::Initialize: failed to initialize io_uring");
        }
        std::vector<struct iovec> iovecs(columnNames.size() * 2);
        for (const auto &columnName : columnNames)
        {
            int index = columnIndexes.at(columnName) * 2;
            for (int bufferId = 0; bufferId < 2; bufferId++)
            {
                auto buffer = buffers.at(columnName)[threadId][bufferId];
                iovecs[index + bufferId].iov_base = buffer->getPointer();
                iovecs[index + bufferId].iov_len = buffer->size();
            }
        }
        ret = io_uring_register_buffers(rings[threadId], iovecs.data(), iovecs.size());
        if (ret != 0)
        {
            throw InvalidArgumentException("GlobalStaticBufferPool::Initialize: failed to register buffers");
        }
    }
    initialized = true;
}

int GlobalStaticBufferPool::AcquireThreadId()
{
    static thread_local int assignedThreadId = -1;
    if (assignedThreadId >= 0)
    {
        return assignedThreadId;
    }
    int threadId = nextThreadId.fetch_add(1);
    if (threadId >= maxThreads)
    {
        nextThreadId.fetch_sub(1);
        throw InvalidArgumentException("GlobalStaticBufferPool::AcquireThreadId: thread limit exceeded");
    }
    assignedThreadId = threadId;
    return assignedThreadId;
}

struct io_uring *GlobalStaticBufferPool::GetRing(int threadId) { return rings.at(threadId); }

std::shared_ptr<ByteBuffer> GlobalStaticBufferPool::GetBuffer(const std::string &columnName, int threadId, int bufferId)
{
    return buffers.at(columnName).at(threadId).at(bufferId);
}

int GlobalStaticBufferPool::GetBufferIndex(const std::string &columnName, int bufferId) const
{
    return columnIndexes.at(columnName) * 2 + bufferId;
}

bool GlobalStaticBufferPool::IsInitialized() const { return initialized; }

size_t GlobalStaticBufferPool::GetTotalAllocatedBytes() const
{
    size_t total = 0;
    for (const auto &column : buffers)
    {
        for (const auto &threadBuffers : column.second)
        {
            for (const auto &buffer : threadBuffers)
            {
                total += buffer->size();
            }
        }
    }
    return total;
}

void GlobalStaticBufferPool::Reset()
{
    std::lock_guard<std::mutex> lock(mutex);
    for (auto ring : rings)
    {
        if (ring != nullptr)
        {
            io_uring_queue_exit(ring);
            delete ring;
        }
    }
    rings.clear();
    buffers.clear();
    columnIndexes.clear();
    columnNames.clear();
    directIoLib = nullptr;
    nextThreadId.store(0);
    maxThreads = 0;
    initialized = false;
}
