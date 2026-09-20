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
#ifndef DUCKDB_DIRECTURINGRANDOMACCESSFILENONFIXED_H
#define DUCKDB_DIRECTURINGRANDOMACCESSFILENONFIXED_H

#include "liburing.h"
#include "liburing/io_uring.h"
#include "physical/natives/DirectRandomAccessFile.h"
#include "exception/InvalidArgumentException.h"
#include "DirectIoLib.h"

/**
 * DirectUringRandomAccessFileNonFixed provides asynchronous file I/O using io_uring
 * with io_uring_prep_read (non-fixed buffer version).
 * 
 * Unlike DirectUringRandomAccessFile which requires pre-registering buffers,
 * this version uses regular buffers without pre-registration, providing more flexibility
 * at a potential slight performance cost.
 */
class DirectUringRandomAccessFileNonFixed : public DirectRandomAccessFile
{
public:
    explicit DirectUringRandomAccessFileNonFixed(const std::string &file);

    /**
     * Initialize io_uring instance
     */
    static void Initialize();

    /**
     * Reset and cleanup io_uring instance
     */
    static void Reset();

    /**
     * Read data asynchronously using io_uring_prep_read (non-fixed buffer)
     * @param length Number of bytes to read
     * @param buffer Buffer to read into
     * @param index Unused in non-fixed version, kept for API compatibility
     * @return ByteBuffer containing the read data
     */
    std::shared_ptr<ByteBuffer> readAsync(int length, std::shared_ptr<ByteBuffer> buffer, int index);

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

    ~DirectUringRandomAccessFileNonFixed();

private:
    static thread_local struct io_uring *ring;
    static thread_local  bool first;
};

#endif // DUCKDB_DIRECTURINGRANDOMACCESSFILENONFIXED_H
