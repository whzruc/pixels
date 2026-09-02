/* Copyright 2026 PixelsDB. Licensed under AGPL-3.0. */
#include "vector/ColumnVectorBufferPool.h"
#include "vector/BinaryColumnVector.h"
#include "gtest/gtest.h"

TEST(ColumnVectorBufferPoolTest, ReusesBinaryBackingArrayOnCurrentThread) {
  ColumnVectorBufferPool::clear();
  pixels::string_t *first = nullptr;
  {
    BinaryColumnVector vector(10000);
    first = vector.vector;
    EXPECT_TRUE(vector.str_vec.empty());
  }
  {
    BinaryColumnVector vector(10000);
    EXPECT_EQ(first, vector.vector);
  }
  ColumnVectorBufferPool::clear();
}

TEST(ColumnVectorBufferPoolTest, AllocatesWriterStringsLazily) {
  BinaryColumnVector vector(8);
  EXPECT_TRUE(vector.str_vec.empty());
  std::string value = "pixels";
  vector.add(value);
  ASSERT_EQ(vector.str_vec.size(), 8);
  EXPECT_EQ(vector.str_vec[0], value);
}
