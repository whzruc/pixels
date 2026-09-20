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
#ifndef PIXELS_DIRECTURINGRANDOMACCESSFILESTATIC_H
#define PIXELS_DIRECTURINGRANDOMACCESSFILESTATIC_H

#include "liburing.h"
#include "liburing/io_uring.h"
#include "physical/natives/DirectRandomAccessFile.h"
#include "physical/GlobalStaticBufferPool.h"
#include "exception/InvalidArgumentException.h"

/**
 * DirectUringRandomAccessFileStatic provides asynchronous file I/O using io_uring
 * with pre-allocated ring and pre-registered buffers from GlobalStaticBufferPool.
 * 
 * Key features:
 * - Uses io_uring ring from GlobalStaticBufferPool (no thread_local ring creation)
 * - Uses pre-registered buffers (io_uring_prep_read_fixed)
 * - Zero allocation overhead during query execution
 * - Thread-safe through thread-specific ring and buffers
 */
class DirectUringRandomAccessFileStatic : public DirectRandomAccessFile
{
public:
    /**
     * Constructor
     * @param file File path
     * @param threadId Thread ID assigned by GlobalStaticBufferPool
     * @param ring io_uring ring from GlobalStaticBufferPool
     */
    DirectUringRandomAccessFileStatic(const std::string& file, int threadId, struct io_uring* ring);

    /**
     * Read data asynchronously using io_uring_prep_read_fixed
     * @param length Number of bytes to read
     * @param buffer Buffer to read into (from GlobalStaticBufferPool)
     * @param columnName Column name for buffer index lookup
     * @param bufferIdx Buffer index (0 or 1 for double buffering)
     * @return ByteBuffer containing the read data
     */
    std::shared_ptr<ByteBuffer> readAsync(int length, std::shared_ptr<ByteBuffer> buffer, 
                                         const std::string& columnName, int bufferIdx);

    /**
     * Submit pending async read operations
     * @param size Number of operations to submit
     */
    void readAsyncSubmit(int size);

    /**
     * Wait for async read operations to complete
     * @param size Number of operations to wait for
     */
    void readAsyncComplete(int size);

    ~DirectUringRandomAccessFileStatic();

private:
    int threadId_;
    struct io_uring* ring_;
};

#endif // PIXELS_DIRECTURINGRANDOMACCESSFILESTATIC_H
