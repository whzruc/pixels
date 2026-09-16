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

#include "physical/ThreadContext.h"

thread_local int pixels::ThreadContext::threadId_ = -1;
thread_local struct io_uring *pixels::ThreadContext::currentRing_ = nullptr;
thread_local struct io_uring *pixels::ThreadContext::prefetchRing_ = nullptr;
thread_local struct io_uring *pixels::ThreadContext::activeRing_ = nullptr;
thread_local bool pixels::ThreadContext::initialized_ = false;

void pixels::ThreadContext::SetThreadId(int threadId)
{
    threadId_ = threadId;
    initialized_ = true;
}

void pixels::ThreadContext::SetRings(struct io_uring *currentRing, struct io_uring *prefetchRing)
{
    currentRing_ = currentRing;
    prefetchRing_ = prefetchRing;
    activeRing_ = currentRing;
    initialized_ = true;
}

void pixels::ThreadContext::SetRing(struct io_uring *ring)
{
    currentRing_ = ring;
    prefetchRing_ = ring;
    activeRing_ = ring;
    initialized_ = true;
}

int pixels::ThreadContext::GetThreadId() { return threadId_; }

struct io_uring *pixels::ThreadContext::GetRing() { return activeRing_; }

struct io_uring *pixels::ThreadContext::GetCurrentRing() { return currentRing_; }

struct io_uring *pixels::ThreadContext::GetPrefetchRing() { return prefetchRing_; }

void pixels::ThreadContext::UseCurrentRing() { activeRing_ = currentRing_; }

void pixels::ThreadContext::UsePrefetchRing() { activeRing_ = prefetchRing_; }

void pixels::ThreadContext::SwapRings()
{
    auto *tmp = currentRing_;
    currentRing_ = prefetchRing_;
    prefetchRing_ = tmp;
    activeRing_ = currentRing_;
}

bool pixels::ThreadContext::HasContext()
{
    return initialized_ && threadId_ >= 0 && activeRing_ != nullptr;
}

void pixels::ThreadContext::Clear()
{
    threadId_ = -1;
    currentRing_ = nullptr;
    prefetchRing_ = nullptr;
    activeRing_ = nullptr;
    initialized_ = false;
}
