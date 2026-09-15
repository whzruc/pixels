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

#include <cstdio>
#include <cstring>
#include <unistd.h>

TEST(DirectIoLibTest, ReadViewRetainsBackingBuffer)
{
    FILE *file = std::tmpfile();
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(ftruncate(fileno(file), 4096), 0);

    DirectIoLib directIo(4096);
    auto owner = std::make_shared<ByteBuffer>(4096);
    std::weak_ptr<ByteBuffer> lifetime = owner;
    auto view = directIo.read(fileno(file), 0, owner, 64);
    owner.reset();

    EXPECT_FALSE(lifetime.expired());
    view.reset();
    EXPECT_TRUE(lifetime.expired());
    std::fclose(file);
}

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

TEST_F(DynamicBufferPoolTest, CapacitySnapshotUsesLogicalColumnIdsAcrossBothBuffers)
{
    // The reader encodes a logical column and its double-buffer parity in the
    // key.  The scheduler must see the same logical key in both capacity maps.
    DynamicBufferPool::AllocateBuffer(6 * 2 + 1, 4096);
    DynamicBufferPool::Switch();
    DynamicBufferPool::AllocateBuffer(6 * 2 + 1, 8192);

    auto capacities = DynamicBufferPool::GetCapacities();
    ASSERT_EQ(capacities[0].count(6), 1);
    ASSERT_EQ(capacities[1].count(6), 1);
    EXPECT_GE(capacities[0].at(6), 4096);
    EXPECT_GE(capacities[1].at(6), 8192);
    EXPECT_EQ(capacities[0].count(12), 0);
    EXPECT_EQ(capacities[1].count(13), 0);
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
