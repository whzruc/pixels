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

#include "physical/natives/DirectUringRandomAccessFileStatic.h"
#include "exception/InvalidArgumentException.h"
#include "profiler/TimeProfiler.h"

DirectUringRandomAccessFileStatic::DirectUringRandomAccessFileStatic(const std::string &file,
                                                                     struct io_uring *uringRing)
    : DirectRandomAccessFile(file), ring(uringRing)
{
    if (ring == nullptr)
    {
        throw InvalidArgumentException("DirectUringRandomAccessFileStatic: ring cannot be null");
    }
}

std::shared_ptr<ByteBuffer> DirectUringRandomAccessFileStatic::readAsync(int length, std::shared_ptr<ByteBuffer> buffer,
                                                                         int bufferIndex, int startOffset)
{
    auto sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr)
    {
        throw InvalidArgumentException("DirectUringRandomAccessFileStatic::readAsync: failed to get sqe");
    }
    uint64_t fileOffset = startOffset;
    uint64_t readLength = length;
    uint64_t bufferOffset = 0;
    if (enableDirect)
    {
        fileOffset = directIoLib->blockStart(startOffset);
        readLength = directIoLib->blockEnd(startOffset + length) - fileOffset;
        bufferOffset = startOffset - fileOffset;
    }
    if (readLength > buffer->size())
    {
        throw InvalidArgumentException("DirectUringRandomAccessFileStatic::readAsync: buffer is too small");
    }
    io_uring_prep_read_fixed(sqe, fd, buffer->getPointer(), readLength, fileOffset, bufferIndex);
    seek(startOffset + length);
    return std::make_shared<ByteBuffer>(*buffer, bufferOffset, length);
}

void DirectUringRandomAccessFileStatic::readAsyncSubmit(int count)
{
    PROFILE_START("Uring.Static.AsyncSubmit.Total");
    int ret = io_uring_submit(ring);
    PROFILE_END("Uring.Static.AsyncSubmit.Total");
    if (ret != count)
    {
        throw InvalidArgumentException("DirectUringRandomAccessFileStatic::readAsyncSubmit: unexpected submit count");
    }
}

void DirectUringRandomAccessFileStatic::readAsyncComplete(int count)
{
    PROFILE_START("Uring.Static.AsyncComplete.Total");
    for (int i = 0; i < count; i++)
    {
        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(ring, &cqe) != 0)
        {
            throw InvalidArgumentException("DirectUringRandomAccessFileStatic::readAsyncComplete: wait failed");
        }
        int result = cqe->res;
        io_uring_cqe_seen(ring, cqe);
        if (result < 0)
        {
            throw InvalidArgumentException("DirectUringRandomAccessFileStatic::readAsyncComplete: read failed");
        }
    }
    PROFILE_END("Uring.Static.AsyncComplete.Total");
}
