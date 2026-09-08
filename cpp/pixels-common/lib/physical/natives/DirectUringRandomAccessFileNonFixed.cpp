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

#include "exception/InvalidArgumentException.h"
#include "profiler/TimeProfiler.h"
#include "utils/ConfigFactory.h"

#include <cstring>

thread_local struct io_uring *DirectUringRandomAccessFileNonFixed::ring =
    nullptr;

DirectUringRandomAccessFileNonFixed::DirectUringRandomAccessFileNonFixed(
    const std::string &file)
    : DirectRandomAccessFile(file) {}

void DirectUringRandomAccessFileNonFixed::Initialize(uint32_t queueDepth) {
  if (ring != nullptr) {
    return;
  }

  auto *newRing = new io_uring();
  int flags = std::stoi(
      ConfigFactory::Instance().getProperty("pixels.io_uring.mode", "0"));
  if (io_uring_queue_init(queueDepth, newRing, flags) < 0) {
    delete newRing;
    throw InvalidArgumentException("DirectUringRandomAccessFileNonFixed::"
                                   "Initialize: failed to initialize io_uring");
  }
  ring = newRing;
}

void DirectUringRandomAccessFileNonFixed::Reset() {
  if (ring != nullptr) {
    io_uring_queue_exit(ring);
    delete ring;
    ring = nullptr;
  }
}

bool DirectUringRandomAccessFileNonFixed::IsInitialized() {
  return ring != nullptr;
}

std::shared_ptr<ByteBuffer> DirectUringRandomAccessFileNonFixed::readAsync(
    int length, std::shared_ptr<ByteBuffer> buffer, int startOffset) {
  if (ring == nullptr) {
    throw InvalidArgumentException(
        "DirectUringRandomAccessFileNonFixed::readAsync: not initialized");
  }
  if (buffer == nullptr) {
    throw InvalidArgumentException(
        "DirectUringRandomAccessFileNonFixed::readAsync: buffer is null");
  }

  auto *sqe = io_uring_get_sqe(ring);
  if (sqe == nullptr) {
    throw InvalidArgumentException(
        "DirectUringRandomAccessFileNonFixed::readAsync: failed to get sqe");
  }

  if (enableDirect) {
    uint64_t alignedOffset = directIoLib->blockStart(startOffset);
    uint64_t readLength =
        directIoLib->blockEnd(startOffset + length) - alignedOffset;
    if (readLength > buffer->size()) {
      throw InvalidArgumentException("DirectUringRandomAccessFileNonFixed::"
                                     "readAsync: buffer is too small");
    }
    io_uring_prep_read(sqe, fd, buffer->getPointer(), readLength,
                       alignedOffset);
    seek(startOffset + length);
    return std::make_shared<ByteBuffer>(*buffer, startOffset - alignedOffset,
                                        length);
  }

  if (static_cast<uint64_t>(length) > buffer->size()) {
    throw InvalidArgumentException(
        "DirectUringRandomAccessFileNonFixed::readAsync: buffer is too small");
  }
  io_uring_prep_read(sqe, fd, buffer->getPointer(), length, startOffset);
  seek(startOffset + length);
  return std::make_shared<ByteBuffer>(*buffer, 0, length);
}

void DirectUringRandomAccessFileNonFixed::readAsyncSubmit(int count) {
  if (ring == nullptr) {
    throw InvalidArgumentException("DirectUringRandomAccessFileNonFixed::"
                                   "readAsyncSubmit: not initialized");
  }
  PROFILE_START("Uring.NonFixed.AsyncSubmit.Total");
  int submitted = io_uring_submit(ring);
  PROFILE_END("Uring.NonFixed.AsyncSubmit.Total");
  if (submitted != count) {
    throw InvalidArgumentException(
        "DirectUringRandomAccessFileNonFixed::readAsyncSubmit: expected " +
        std::to_string(count) + " submissions, got " +
        std::to_string(submitted));
  }
}

void DirectUringRandomAccessFileNonFixed::readAsyncComplete(int count) {
  if (ring == nullptr) {
    throw InvalidArgumentException("DirectUringRandomAccessFileNonFixed::"
                                   "readAsyncComplete: not initialized");
  }
  PROFILE_START("Uring.NonFixed.AsyncComplete.Total");
  for (int i = 0; i < count; ++i) {
    struct io_uring_cqe *cqe = nullptr;
    if (io_uring_wait_cqe(ring, &cqe) != 0) {
      throw InvalidArgumentException("DirectUringRandomAccessFileNonFixed::"
                                     "readAsyncComplete: wait failed");
    }
    if (cqe->res < 0) {
      int errorNumber = -cqe->res;
      io_uring_cqe_seen(ring, cqe);
      throw InvalidArgumentException("DirectUringRandomAccessFileNonFixed::"
                                     "readAsyncComplete: read failed: " +
                                     std::string(std::strerror(errorNumber)));
    }
    io_uring_cqe_seen(ring, cqe);
  }
  PROFILE_END("Uring.NonFixed.AsyncComplete.Total");
}
