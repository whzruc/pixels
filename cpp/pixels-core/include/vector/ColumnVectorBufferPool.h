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
#ifndef PIXELS_COLUMNVECTORBUFFERPOOL_H
#define PIXELS_COLUMNVECTORBUFFERPOOL_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

/**
 * ColumnVectorBufferPool is a thread-local free-list pool for the large aligned
 * backing arrays used by column vectors (e.g. the duckdb::string_t array of
 * BinaryColumnVector, ~156KB at stride=10000).
 *
 * Motivation: such arrays exceed glibc's default M_MMAP_THRESHOLD (128KB), so
 * each per-file column vector allocation goes through mmap and each free through
 * munmap. Under high thread concurrency this causes the process-wide mmap_lock
 * (mmap_sem) to be contended between munmap/mmap (write lock) and first-touch
 * page faults (read lock), showing up as large off-CPU time blocked in
 * down_read on the setRef path.
 *
 * By reusing buffers across files within the same worker thread, the array is
 * allocated once and stays resident (warm pages), eliminating the mmap/munmap
 * churn and repeated first-touch faults.
 *
 * The pool is thread-local, so acquire/release are lock-free. Each scan thread
 * processes files sequentially, so the steady-state free-list size is small
 * (roughly the number of concurrently-live column vectors of a given capacity).
 */
class ColumnVectorBufferPool
{
 public:
  /**
   * Acquire an aligned buffer of at least `bytes` bytes. Reuses a buffer from
   * the thread-local free-list when an exact (alignment, bytes) match exists,
   * otherwise allocates a new one via posix_memalign. Returns nullptr on
   * allocation failure (callers should fall back to direct allocation).
   */
  static void *acquire(size_t bytes, size_t alignment);

  /**
   * Return a buffer previously obtained via acquire() (with the same `bytes`
   * and `alignment`) to the thread-local free-list for reuse. If the free-list
   * for this bucket is already at capacity, the buffer is freed instead.
   */
  static void release(void *ptr, size_t bytes, size_t alignment);

  /**
   * Whether pooling is enabled (config `pixels.columnvector.pool`, default on).
   * Read once and cached. When disabled, acquire/release behave as plain
   * posix_memalign/free so behavior matches the pre-pool code path.
   */
  static bool enabled();

 private:
  // Maximum number of cached buffers per (alignment, bytes) bucket.
  static constexpr size_t MAX_BUFFERS_PER_BUCKET = 8;

  // free-list keyed by alignment then by byte size.
  static thread_local std::map<size_t, std::map<size_t, std::vector<void *>>> freeBuffers;
};

#endif //PIXELS_COLUMNVECTORBUFFERPOOL_H
