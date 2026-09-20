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
 * @create 2026-02-09
 */
#include "physical/natives/DirectUringRandomAccessFileNonFixed.h"
#include "profiler/TimeProfiler.h"

thread_local struct io_uring *DirectUringRandomAccessFileNonFixed::ring = nullptr;
thread_local bool DirectUringRandomAccessFileNonFixed::first=true;

DirectUringRandomAccessFileNonFixed::DirectUringRandomAccessFileNonFixed(const std::string &file) 
    : DirectRandomAccessFile(file)
{
}

void DirectUringRandomAccessFileNonFixed::Initialize()
{
    // initialize io_uring ring
    if (ring == nullptr)
    {
        ring = new io_uring();
        if (io_uring_queue_init(4096, ring, 0) < 0)
        {
            throw InvalidArgumentException("DirectUringRandomAccessFileNonFixed: initialize io_uring fails.");
        }
    }
}

void DirectUringRandomAccessFileNonFixed::Reset()
{
    // Important! Because sometimes ring is nullptr here.
    // For example, two threads A and B share the same global state. If A finish all files while B just starts,
    // B would execute Reset function from InitLocal. If we don't set this 'if' branch, ring would be double freed.
    if (ring != nullptr)
    {
        io_uring_queue_exit(ring);
        delete ring;
        ring = nullptr;
    }
}

DirectUringRandomAccessFileNonFixed::~DirectUringRandomAccessFileNonFixed()
{
}

std::shared_ptr<ByteBuffer>
DirectUringRandomAccessFileNonFixed::readAsync(int length, std::shared_ptr<ByteBuffer> buffer, int index)
{
    if (enableDirect)
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        
        // the file will be read from blockStart(fileOffset), and the first fileDelta bytes should be ignored.
        uint64_t fileOffsetAligned = directIoLib->blockStart(offset);
        uint64_t toRead = directIoLib->blockEnd(offset + length) - directIoLib->blockStart(offset);
        
        // Use io_uring_prep_read instead of io_uring_prep_read_fixed
        // No need for buffer index parameter - buffer is passed directly
        if (first)
        {
            std::cout<<"read_unfixed"<<std::endl;
            first=false;
        }
        io_uring_prep_read(sqe, fd, buffer->getPointer(), toRead, fileOffsetAligned);
        auto bb = std::make_shared<ByteBuffer>(*buffer, offset - fileOffsetAligned, length);
        seek(offset + length);
        return bb;
    }
    else
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        
        // Use io_uring_prep_read instead of io_uring_prep_read_fixed
        // No need for buffer index parameter - buffer is passed directly
        io_uring_prep_read(sqe, fd, buffer->getPointer(), length, offset);
        
        seek(offset + length);
        auto result = std::make_shared<ByteBuffer>(*buffer, 0, length);
        return result;
    }
}

void DirectUringRandomAccessFileNonFixed::readAsyncSubmit(int size)
{
    PROFILE_START("Uring.AsyncSubmit.Total");
    int ret = io_uring_submit(ring);
    PROFILE_END("Uring.AsyncSubmit.Total");
    if (ret != size)
    {
        throw InvalidArgumentException("DirectUringRandomAccessFileNonFixed::readAsyncSubmit: submit fails");
    }
}

void DirectUringRandomAccessFileNonFixed::readAsyncComplete(int size)
{
    PROFILE_START("Uring.AsyncComplete.Total");
    // Important! We cannot write the code as io_uring_wait_cqe_nr(ring, &cqe, size).
    // The reason is unclear, but some random bugs would happen. It takes me nearly a week to find this bug
    struct io_uring_cqe *cqe;
    for (int i = 0; i < size; i++)
    {
        PROFILE_START("Uring.AsyncComplete.WaitCQE");
        if (io_uring_wait_cqe_nr(ring, &cqe, 1) != 0)
        {
            throw InvalidArgumentException("DirectUringRandomAccessFileNonFixed::readAsyncComplete: wait cqe fails");
        }
        PROFILE_END("Uring.AsyncComplete.WaitCQE");
        PROFILE_START("Uring.AsyncComplete.ProcessCQE");
        io_uring_cqe_seen(ring, cqe);
        PROFILE_END("Uring.AsyncComplete.ProcessCQE");
    }
    PROFILE_END("Uring.AsyncComplete.Total");
}
