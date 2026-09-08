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

#ifndef PIXELS_GLOBAL_STATIC_BUFFER_POOL_H
#define PIXELS_GLOBAL_STATIC_BUFFER_POOL_H

#include "liburing.h"
#include "physical/natives/ByteBuffer.h"
#include "physical/natives/DirectIoLib.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class GlobalStaticBufferPool
{
   public:
    static GlobalStaticBufferPool &Instance();

    void Initialize(const std::string &columnSizePath, int blockSize, int maxThreads);

    int AcquireThreadId();

    struct io_uring *GetRing(int threadId);

    std::shared_ptr<ByteBuffer> GetBuffer(const std::string &columnName, int threadId, int bufferId);

    int GetBufferIndex(const std::string &columnName, int bufferId) const;

    bool IsInitialized() const;

    size_t GetTotalAllocatedBytes() const;

    void Reset();

   private:
    GlobalStaticBufferPool() = default;
    ~GlobalStaticBufferPool();

    GlobalStaticBufferPool(const GlobalStaticBufferPool &) = delete;
    GlobalStaticBufferPool &operator=(const GlobalStaticBufferPool &) = delete;

    mutable std::mutex mutex;
    std::map<std::string, std::vector<std::vector<std::shared_ptr<ByteBuffer>>>> buffers;
    std::map<std::string, int> columnIndexes;
    std::vector<std::string> columnNames;
    std::vector<struct io_uring *> rings;
    std::shared_ptr<DirectIoLib> directIoLib;
    std::atomic<int> nextThreadId{0};
    int maxThreads = 0;
    bool initialized = false;
};

#endif
