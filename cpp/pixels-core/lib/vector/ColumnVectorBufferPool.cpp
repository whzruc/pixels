/* Copyright 2026 PixelsDB. Licensed under AGPL-3.0. */
#include "vector/ColumnVectorBufferPool.h"
#include "utils/ConfigFactory.h"

#include <cstdlib>
#include <map>
#include <vector>

namespace {
using Pool = std::map<size_t, std::map<size_t, std::vector<void *>>>;
constexpr size_t MAX_BUFFERS_PER_BUCKET = 8;

struct ThreadLocalBuffers {
  Pool pool;
  ~ThreadLocalBuffers() {
    for (auto &[alignment, bySize] : pool)
      for (auto &[bytes, bucket] : bySize)
        for (auto *ptr : bucket)
          free(ptr);
  }
};

thread_local ThreadLocalBuffers buffers;
} // namespace

bool ColumnVectorBufferPool::enabled() {
  static const bool value = [] {
    try {
      return ConfigFactory::Instance().getBoolProperty(
          "pixels.columnvector.pool", true);
    } catch (...) {
      return true;
    }
  }();
  return value;
}

void *ColumnVectorBufferPool::acquire(size_t bytes, size_t alignment) {
  if (bytes == 0)
    return nullptr;
  if (enabled()) {
    auto &bucket = buffers.pool[alignment][bytes];
    if (!bucket.empty()) {
      auto *result = bucket.back();
      bucket.pop_back();
      return result;
    }
  }
  void *result = nullptr;
  return posix_memalign(&result, alignment, bytes) == 0 ? result : nullptr;
}

void ColumnVectorBufferPool::release(void *ptr, size_t bytes,
                                     size_t alignment) {
  if (ptr == nullptr)
    return;
  if (enabled()) {
    auto &bucket = buffers.pool[alignment][bytes];
    if (bucket.size() < MAX_BUFFERS_PER_BUCKET) {
      bucket.push_back(ptr);
      return;
    }
  }
  free(ptr);
}

void ColumnVectorBufferPool::clear() {
  for (auto &[alignment, bySize] : buffers.pool) {
    for (auto &[bytes, bucket] : bySize) {
      for (auto *ptr : bucket)
        free(ptr);
    }
  }
  buffers.pool.clear();
}
