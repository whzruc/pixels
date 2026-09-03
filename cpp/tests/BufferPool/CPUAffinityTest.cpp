/* Copyright 2026 PixelsDB. Licensed under AGPL-3.0. */
#include "CPUAffinity.h"
#include "gtest/gtest.h"

TEST(CPUAffinityTest, ParsesExplicitMapping) {
  auto cpus = duckdb::ParseCPUAffinityMapping("3,7,11");
  ASSERT_EQ(cpus.size(), 3U);
  EXPECT_EQ(cpus[0], 3);
  EXPECT_EQ(cpus[1], 7);
  EXPECT_EQ(cpus[2], 11);
}

TEST(CPUAffinityTest, RejectsMalformedMapping) {
  EXPECT_THROW(duckdb::ParseCPUAffinityMapping("3,,7"), std::invalid_argument);
  EXPECT_THROW(duckdb::ParseCPUAffinityMapping("cpu7"), std::invalid_argument);
}

TEST(CPUAffinityTest, RejectsUnknownStrategy) {
  EXPECT_THROW(duckdb::ApplyCPUAffinity("random", "0", 0), std::invalid_argument);
}
