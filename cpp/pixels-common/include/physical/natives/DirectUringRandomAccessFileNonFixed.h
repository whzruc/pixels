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

#ifndef PIXELS_DIRECT_URING_RANDOM_ACCESS_FILE_NON_FIXED_H
#define PIXELS_DIRECT_URING_RANDOM_ACCESS_FILE_NON_FIXED_H

#include "liburing.h"
#include "physical/natives/DirectRandomAccessFile.h"

class DirectUringRandomAccessFileNonFixed : public DirectRandomAccessFile {
public:
  explicit DirectUringRandomAccessFileNonFixed(const std::string &file);

  static void Initialize(uint32_t queueDepth = 4096);
  static void Reset();
  static bool IsInitialized();

  std::shared_ptr<ByteBuffer>
  readAsync(int length, std::shared_ptr<ByteBuffer> buffer, int startOffset);
  void readAsyncSubmit(int count);
  void readAsyncComplete(int count);

private:
  static thread_local struct io_uring *ring;
};

#endif // PIXELS_DIRECT_URING_RANDOM_ACCESS_FILE_NON_FIXED_H
