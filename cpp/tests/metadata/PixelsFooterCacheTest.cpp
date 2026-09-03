/* Copyright 2026 PixelsDB. Licensed under AGPL-3.0. */
#include "PixelsFooterCache.h"
#include "gtest/gtest.h"

#include <thread>
#include <vector>

TEST(PixelsFooterCacheTest, OwnsFileTailBackingBuffer) {
  PixelsFooterCache cache;
  auto buffer = std::make_shared<ByteBuffer>(64);
  std::weak_ptr<ByteBuffer> lifetime = buffer;
  auto file_tail = reinterpret_cast<const pixels::fb::FileTail *>(buffer->getPointer());

  EXPECT_EQ(file_tail, cache.putFileTailIfAbsent("/disk0/a.pxl", buffer, file_tail));
  buffer.reset();

  EXPECT_FALSE(lifetime.expired());
  EXPECT_EQ(file_tail, cache.getFileTail("/disk0/a.pxl"));
}

TEST(PixelsFooterCacheTest, KeepsFirstConcurrentRowGroupFooter) {
  PixelsFooterCache cache;
  constexpr int thread_count = 16;
  std::vector<const pixels::fb::RowGroupFooter *> results(thread_count);
  std::vector<std::thread> threads;

  for (int i = 0; i < thread_count; ++i) {
    threads.emplace_back([&, i]() {
      auto buffer = std::make_shared<ByteBuffer>(64);
      auto footer = reinterpret_cast<const pixels::fb::RowGroupFooter *>(buffer->getPointer());
      results[i] = cache.putRGFooterIfAbsent("/disk0/a.pxl-0", buffer, footer);
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  for (auto result : results) {
    EXPECT_EQ(results.front(), result);
  }
  EXPECT_EQ(results.front(), cache.getRGFooter("/disk0/a.pxl-0"));
}

TEST(PixelsFooterCacheTest, DistinguishesEqualBasenamesByFullPath) {
  PixelsFooterCache cache;
  auto first_buffer = std::make_shared<ByteBuffer>(64);
  auto second_buffer = std::make_shared<ByteBuffer>(64);
  auto first = reinterpret_cast<const pixels::fb::FileTail *>(first_buffer->getPointer());
  auto second = reinterpret_cast<const pixels::fb::FileTail *>(second_buffer->getPointer());

  cache.putFileTailIfAbsent("/disk0/hits.pxl", first_buffer, first);
  cache.putFileTailIfAbsent("/disk1/hits.pxl", second_buffer, second);

  EXPECT_EQ(first, cache.getFileTail("/disk0/hits.pxl"));
  EXPECT_EQ(second, cache.getFileTail("/disk1/hits.pxl"));
}

TEST(PixelsFooterCacheTest, DistinguishesRowGroupsOnDifferentStoragePaths) {
  PixelsFooterCache cache;
  auto first_buffer = std::make_shared<ByteBuffer>(64);
  auto second_buffer = std::make_shared<ByteBuffer>(64);
  auto first = reinterpret_cast<const pixels::fb::RowGroupFooter *>(first_buffer->getPointer());
  auto second = reinterpret_cast<const pixels::fb::RowGroupFooter *>(second_buffer->getPointer());

  cache.putRGFooterIfAbsent("/disk0/hits.pxl-0", first_buffer, first);
  cache.putRGFooterIfAbsent("/disk1/hits.pxl-0", second_buffer, second);

  EXPECT_EQ(first, cache.getRGFooter("/disk0/hits.pxl-0"));
  EXPECT_EQ(second, cache.getRGFooter("/disk1/hits.pxl-0"));
}
