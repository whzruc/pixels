/*
 * Copyright 2026 PixelsDB.
 *
 * Experimental aligned-allocation bridge for sharing DuckDB's statically
 * linked, prefixed jemalloc instance with Pixels.
 */
#ifndef PIXELS_ALIGNED_MEMORY_H
#define PIXELS_ALIGNED_MEMORY_H

#include <cstddef>
#include <cstdlib>

#if defined(PIXELS_USE_DUCKDB_JEMALLOC)
extern "C" int duckdb_je_posix_memalign(void **memptr, size_t alignment,
                                         size_t size) noexcept;
extern "C" void duckdb_je_free(void *ptr) noexcept;
#endif

namespace pixels::memory
{

inline int AlignedAllocate(void **pointer, size_t alignment, size_t size) noexcept
{
#if defined(PIXELS_USE_DUCKDB_JEMALLOC)
    return duckdb_je_posix_memalign(pointer, alignment, size);
#else
    return ::posix_memalign(pointer, alignment, size);
#endif
}

inline void AlignedFree(void *pointer) noexcept
{
#if defined(PIXELS_USE_DUCKDB_JEMALLOC)
    duckdb_je_free(pointer);
#else
    ::free(pointer);
#endif
}

inline bool UsesDuckDBJemalloc() noexcept
{
#if defined(PIXELS_USE_DUCKDB_JEMALLOC)
    return true;
#else
    return false;
#endif
}

} // namespace pixels::memory

#endif // PIXELS_ALIGNED_MEMORY_H
