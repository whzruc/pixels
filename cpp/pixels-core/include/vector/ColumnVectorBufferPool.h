/* Copyright 2026 PixelsDB. Licensed under AGPL-3.0. */
#ifndef PIXELS_COLUMNVECTORBUFFERPOOL_H
#define PIXELS_COLUMNVECTORBUFFERPOOL_H

#include <cstddef>

class ColumnVectorBufferPool {
public:
  static void *acquire(size_t bytes, size_t alignment);
  static void release(void *ptr, size_t bytes, size_t alignment);
  static bool enabled();
  static void clear();
};

#endif
