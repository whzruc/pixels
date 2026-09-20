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
 * @create 2026-06-14
 */
#include "vector/ColumnVectorBufferPool.h"
#include "utils/ConfigFactory.h"

#include <cstdlib>

thread_local std::map<size_t, std::map<size_t, std::vector<void *>>>
    ColumnVectorBufferPool::freeBuffers;

bool ColumnVectorBufferPool::enabled()
{
  // Cache once per process: config is immutable for the run.
  static const bool isEnabled = []() {
    try
    {
      return ConfigFactory::Instance().getBoolProperty("pixels.columnvector.pool", true);
    }
    catch (...)
    {
      return true;
    }
  }();
  return isEnabled;
}

void *ColumnVectorBufferPool::acquire(size_t bytes, size_t alignment)
{
  if (bytes == 0)
  {
    return nullptr;
  }

  if (enabled())
  {
    auto alignIt = freeBuffers.find(alignment);
    if (alignIt != freeBuffers.end())
    {
      auto &bySize = alignIt->second;
      auto sizeIt = bySize.find(bytes);
      if (sizeIt != bySize.end() && !sizeIt->second.empty())
      {
        void *ptr = sizeIt->second.back();
        sizeIt->second.pop_back();
        return ptr;
      }
    }
  }

  void *ptr = nullptr;
  if (posix_memalign(&ptr, alignment, bytes) != 0)
  {
    return nullptr;
  }
  return ptr;
}

void ColumnVectorBufferPool::release(void *ptr, size_t bytes, size_t alignment)
{
  if (ptr == nullptr)
  {
    return;
  }

  if (enabled() && bytes != 0)
  {
    auto &bucket = freeBuffers[alignment][bytes];
    if (bucket.size() < MAX_BUFFERS_PER_BUCKET)
    {
      bucket.push_back(ptr);
      return;
    }
  }

  free(ptr);
}
