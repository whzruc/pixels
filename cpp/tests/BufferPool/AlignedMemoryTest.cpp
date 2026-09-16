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

#include "gtest/gtest.h"
#include "utils/AlignedMemory.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace
{
std::atomic<int> allocationCalls{0};
std::atomic<int> freeCalls{0};
}

// Test doubles for DuckDB's prefixed jemalloc entry points. The production
// jemalloc build resolves the same symbols from DuckDB and therefore fails at
// link time instead of silently mixing allocators if they are unavailable.
extern "C" int duckdb_je_posix_memalign(void **pointer, size_t alignment,
                                         size_t size) noexcept
{
    allocationCalls.fetch_add(1, std::memory_order_relaxed);
    return ::posix_memalign(pointer, alignment, size);
}

extern "C" void duckdb_je_free(void *pointer) noexcept
{
    freeCalls.fetch_add(1, std::memory_order_relaxed);
    ::free(pointer);
}

TEST(AlignedMemoryTest, UsesDuckDBEntryPointsAndPreservesAlignment)
{
    allocationCalls.store(0, std::memory_order_relaxed);
    freeCalls.store(0, std::memory_order_relaxed);

    void *pointer = nullptr;
    ASSERT_EQ(pixels::memory::AlignedAllocate(&pointer, 4096, 8192), 0);
    ASSERT_NE(pointer, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(pointer) % 4096, 0);
    EXPECT_TRUE(pixels::memory::UsesDuckDBJemalloc());
    EXPECT_EQ(allocationCalls.load(std::memory_order_relaxed), 1);

    pixels::memory::AlignedFree(pointer);
    EXPECT_EQ(freeCalls.load(std::memory_order_relaxed), 1);
}
