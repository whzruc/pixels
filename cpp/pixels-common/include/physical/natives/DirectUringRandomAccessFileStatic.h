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

#ifndef PIXELS_DIRECT_URING_RANDOM_ACCESS_FILE_STATIC_H
#define PIXELS_DIRECT_URING_RANDOM_ACCESS_FILE_STATIC_H

#include "physical/natives/DirectRandomAccessFile.h"
#include "liburing.h"

class DirectUringRandomAccessFileStatic : public DirectRandomAccessFile
{
   public:
    DirectUringRandomAccessFileStatic(const std::string &file, struct io_uring *ring);

    std::shared_ptr<ByteBuffer> readAsync(int length, std::shared_ptr<ByteBuffer> buffer, int bufferIndex,
                                          int startOffset);

    void readAsyncSubmit(int count);

    void readAsyncComplete(int count);

   private:
    struct io_uring *ring;
};

#endif
