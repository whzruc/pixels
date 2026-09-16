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

#ifndef PIXELS_THREAD_CONTEXT_H
#define PIXELS_THREAD_CONTEXT_H

#include "liburing.h"

namespace pixels
{
class ThreadContext
{
   public:
    static void SetThreadId(int threadId);
    static void SetRings(struct io_uring *currentRing, struct io_uring *prefetchRing);
    static void SetRing(struct io_uring *ring);
    static int GetThreadId();
    static struct io_uring *GetRing();
    static struct io_uring *GetCurrentRing();
    static struct io_uring *GetPrefetchRing();
    static void UseCurrentRing();
    static void UsePrefetchRing();
    static void SwapRings();
    static bool HasContext();
    static void Clear();

   private:
    static thread_local int threadId_;
    static thread_local struct io_uring *currentRing_;
    static thread_local struct io_uring *prefetchRing_;
    static thread_local struct io_uring *activeRing_;
    static thread_local bool initialized_;
};
}  // namespace pixels

#endif  // PIXELS_THREAD_CONTEXT_H
