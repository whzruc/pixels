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
#include "physical/GlobalStaticBufferPool.h"

#include <fstream>

TEST(GlobalStaticBufferPoolTest, AllocatesRegisteredDoubleBuffers)
{
    const std::string sizePath = "/tmp/pixels-static-buffer-pool-test.csv";
    {
        std::ofstream output(sizePath);
        output << "first 4096\nsecond 8192\n";
    }

    auto &pool = GlobalStaticBufferPool::Instance();
    pool.Initialize(sizePath, 4096, 1);
    int threadId = pool.AcquireThreadId();

    EXPECT_EQ(threadId, 0);
    EXPECT_NE(pool.GetRing(threadId), nullptr);
    EXPECT_GE(pool.GetBuffer("first", threadId, 0)->size(), 4096);
    EXPECT_NE(pool.GetBuffer("first", threadId, 0), pool.GetBuffer("first", threadId, 1));
    EXPECT_NE(pool.GetBufferIndex("first", 0), pool.GetBufferIndex("first", 1));
    EXPECT_GE(pool.GetTotalAllocatedBytes(), 24576);

    pool.Reset();
}
