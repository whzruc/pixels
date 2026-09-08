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

#include "physical/natives/DirectUringRandomAccessFileNonFixed.h"
#include "gtest/gtest.h"

TEST(DirectUringNonFixedTest, InitializesAndResetsThreadLocalRing) {
  struct io_uring probeRing{};
  if (io_uring_queue_init(2, &probeRing, 0) < 0) {
    GTEST_SKIP() << "io_uring is unavailable";
  }
  io_uring_queue_exit(&probeRing);

  DirectUringRandomAccessFileNonFixed::Initialize(8);
  EXPECT_TRUE(DirectUringRandomAccessFileNonFixed::IsInitialized());
  DirectUringRandomAccessFileNonFixed::Reset();
  EXPECT_FALSE(DirectUringRandomAccessFileNonFixed::IsInitialized());
}
