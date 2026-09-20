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
 * @create 2026-02-10
 */
#include "physical/natives/DirectUringRandomAccessFileStatic.h"
#include "profiler/TimeProfiler.h"

DirectUringRandomAccessFileStatic::DirectUringRandomAccessFileStatic(
    const std::string& file, int threadId, struct io_uring* ring)
    : DirectRandomAccessFile(file), threadId_(threadId), ring_(ring)
{
    if (ring_ == nullptr) {
        throw InvalidArgumentException("DirectUringRandomAccessFileStatic: ring cannot be null");
    }
}

DirectUringRandomAccessFileStatic::~DirectUringRandomAccessFileStatic()
{
    // Ring is managed by GlobalStaticBufferPool, don't clean up here
}

std::shared_ptr<ByteBuffer>
DirectUringRandomAccessFileStatic::
readAsync(int length, std::shared_ptr<ByteBuffer> buffer,
                                            const std::string& columnName, int bufferIdx)
{
    // Get the buffer index in the registered buffer array
    int registeredBufferIndex = GlobalStaticBufferPool::Instance().GetBufferIndex(
        columnName, threadId_, bufferIdx);
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) {
        throw InvalidArgumentException(
            "DirectUringRandomAccessFileStatic::readAsync: failed to get SQE");
    }
    
    if (enableDirect)
    {
        // Direct I/O: align offset and length
        uint64_t fileOffsetAligned = directIoLib->blockStart(offset);
        uint64_t toRead = directIoLib->blockEnd(offset + length) - directIoLib->blockStart(offset);
        
        // Use io_uring_prep_read_fixed with pre-registered buffer
        bool useFixedBuffer = true;
        try {
            useFixedBuffer = ConfigFactory::Instance().boolCheckProperty("localfs.iouring.use.fixed.buffer");
        } catch (...) {
            useFixedBuffer = true; // default to fixed buffer for backward compatibility
        }
        if (useFixedBuffer)
        {
            io_uring_prep_read_fixed(sqe, fd, buffer->getPointer(), toRead,
                                     fileOffsetAligned, registeredBufferIndex);
        }else
        {
            io_uring_prep_read(sqe,fd,buffer->getPointer(),
                toRead,
                fileOffsetAligned);
        }
        
        // Create ByteBuffer view with correct offset
        auto bb = std::make_shared<ByteBuffer>(*buffer, offset - fileOffsetAligned, length);
        seek(offset + length);
        return bb;
    }
    else
    {
        // Non-direct I/O
        io_uring_prep_read_fixed(sqe, fd, buffer->getPointer(), length, 
                                 offset, registeredBufferIndex);
        
        seek(offset + length);
        auto result = std::make_shared<ByteBuffer>(*buffer, 0, length);
        return result;
    }
}

void DirectUringRandomAccessFileStatic::readAsyncSubmit(int size)
{
    PROFILE_START("Uring.AsyncSubmit.Total");
    int ret = io_uring_submit(ring_);
    PROFILE_END("Uring.AsyncSubmit.Total");
    if (ret != size)
    {
        throw InvalidArgumentException(
            "DirectUringRandomAccessFileStatic::readAsyncSubmit: submit fails, expected " +
            std::to_string(size) + " but got " + std::to_string(ret));
    }
}

void DirectUringRandomAccessFileStatic::readAsyncComplete(int size)
{
    PROFILE_START("Uring.AsyncComplete.Total");
    // Wait for completion events one by one
    // Important: Cannot use io_uring_wait_cqe_nr(ring, &cqe, size) due to potential bugs
    struct io_uring_cqe* cqe;
    for (int i = 0; i < size; i++)
    {
        PROFILE_START("Uring.AsyncComplete.WaitCQE");
        if (io_uring_wait_cqe_nr(ring_, &cqe, 1) != 0)
        {
            throw InvalidArgumentException(
                "DirectUringRandomAccessFileStatic::readAsyncComplete: wait cqe fails");
        }
        PROFILE_END("Uring.AsyncComplete.WaitCQE");
        PROFILE_START("Uring.AsyncComplete.ProcessCQE");
        io_uring_cqe_seen(ring_, cqe);
        PROFILE_END("Uring.AsyncComplete.ProcessCQE");
    }
    PROFILE_END("Uring.AsyncComplete.Total");
}
