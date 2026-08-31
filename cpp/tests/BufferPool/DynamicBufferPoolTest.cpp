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
#include "physical/DynamicBufferPool.h"

#include <cstring>

class DynamicBufferPoolTest : public testing::Test
{
   protected:
    void SetUp() override
    {
        if (io_uring_queue_init(64, &ring, 0) < 0)
        {
            GTEST_SKIP() << "io_uring is unavailable";
        }
        ringInitialized = true;
        DynamicBufferPool::Initialize(&ring, 8);
    }

    void TearDown() override
    {
        if (DynamicBufferPool::IsInitialized())
        {
            DynamicBufferPool::Reset();
        }
        if (ringInitialized)
        {
            io_uring_queue_exit(&ring);
        }
    }

    struct io_uring ring{};
    bool ringInitialized = false;
};

TEST_F(DynamicBufferPoolTest, AllocatesAndReleasesSlots)
{
    auto first = DynamicBufferPool::AllocateBuffer(3, 4096);
    auto second = DynamicBufferPool::AllocateBuffer(7, 8192);

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(DynamicBufferPool::GetBufferCount(), 2);
    EXPECT_EQ(DynamicBufferPool::GetBuffer(3), first);

    int releasedSlot = DynamicBufferPool::GetBufferSlotIndex(3);
    DynamicBufferPool::ReleaseBuffer(3);
    EXPECT_EQ(DynamicBufferPool::GetBuffer(3), nullptr);
    EXPECT_EQ(DynamicBufferPool::GetBufferCount(), 1);

    for (uint32_t colId = 9; colId <= 15; colId++)
    {
        DynamicBufferPool::AllocateBuffer(colId, 4096);
    }
    EXPECT_EQ(DynamicBufferPool::GetBufferSlotIndex(15), releasedSlot);
}

TEST_F(DynamicBufferPoolTest, GrowsBufferAndPreservesData)
{
    auto buffer = DynamicBufferPool::AllocateBuffer(1, 4096);
    std::memset(buffer->getPointer(), 0x5a, 4096);

    auto grown = DynamicBufferPool::GrowBuffer(1, 16384);

    ASSERT_NE(grown, nullptr);
    EXPECT_GE(grown->size(), 16384);
    auto bytes = static_cast<const unsigned char *>(grown->getPointer());
    for (size_t i = 0; i < 4096; i++)
    {
        ASSERT_EQ(bytes[i], 0x5a);
    }
}

TEST_F(DynamicBufferPoolTest, RejectsDuplicateColumnsAndExhaustedSlots)
{
    DynamicBufferPool::AllocateBuffer(0, 4096);
    EXPECT_THROW(DynamicBufferPool::AllocateBuffer(0, 4096), InvalidArgumentException);

    for (uint32_t colId = 1; colId < 8; colId++)
    {
        DynamicBufferPool::AllocateBuffer(colId, 4096);
    }
    EXPECT_THROW(DynamicBufferPool::AllocateBuffer(8, 4096), InvalidArgumentException);
}
