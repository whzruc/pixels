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

#include "physical/BufferPoolStats.h"

#include "utils/ConfigFactory.h"

#include <cstdlib>
#include <iostream>

BufferPoolStats &BufferPoolStats::Instance()
{
    static BufferPoolStats *instance = new BufferPoolStats();
    return *instance;
}

BufferPoolStats::BufferPoolStats()
    : enabled(ConfigFactory::Instance().getProperty("pixel.bufferpool.stats.enabled", "false") == "true")
{
    if (enabled)
    {
        std::atexit([]() { BufferPoolStats::Instance().Print(); });
    }
}

bool BufferPoolStats::IsEnabled() const { return enabled; }

void BufferPoolStats::UpdatePeak(std::atomic<uint64_t> &peak, uint64_t value)
{
    uint64_t previous = peak.load(std::memory_order_relaxed);
    while (previous < value &&
           !peak.compare_exchange_weak(previous, value, std::memory_order_relaxed))
    {
    }
}

void BufferPoolStats::Subtract(std::atomic<uint64_t> &value, uint64_t amount)
{
    uint64_t current = value.load(std::memory_order_relaxed);
    while (!value.compare_exchange_weak(current, current > amount ? current - amount : 0,
                                        std::memory_order_relaxed))
    {
    }
}

void BufferPoolStats::RecordAllocation(BufferPoolStatsMode mode, uint64_t bytes)
{
    if (!enabled)
    {
        return;
    }
    auto &counter = counters[static_cast<size_t>(mode)];
    uint64_t current = counter.currentAllocatedBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    UpdatePeak(counter.peakAllocatedBytes, current);
    counter.allocationCount.fetch_add(1, std::memory_order_relaxed);
}

void BufferPoolStats::RecordFree(BufferPoolStatsMode mode, uint64_t bytes)
{
    if (!enabled)
    {
        return;
    }
    auto &counter = counters[static_cast<size_t>(mode)];
    Subtract(counter.currentAllocatedBytes, bytes);
    counter.freeCount.fetch_add(1, std::memory_order_relaxed);
}

void BufferPoolStats::RecordRegistration(BufferPoolStatsMode mode, uint64_t bytes)
{
    if (!enabled)
    {
        return;
    }
    auto &counter = counters[static_cast<size_t>(mode)];
    uint64_t current = counter.currentRegisteredBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    UpdatePeak(counter.peakRegisteredBytes, current);
    counter.registrationCount.fetch_add(1, std::memory_order_relaxed);
}

void BufferPoolStats::RecordUnregistration(BufferPoolStatsMode mode, uint64_t bytes)
{
    if (!enabled)
    {
        return;
    }
    Subtract(counters[static_cast<size_t>(mode)].currentRegisteredBytes, bytes);
}

void BufferPoolStats::RecordRegistrationUpdate(BufferPoolStatsMode mode, uint64_t oldBytes, uint64_t newBytes)
{
    if (!enabled)
    {
        return;
    }
    auto &counter = counters[static_cast<size_t>(mode)];
    Subtract(counter.currentRegisteredBytes, oldBytes);
    uint64_t current = counter.currentRegisteredBytes.fetch_add(newBytes, std::memory_order_relaxed) + newBytes;
    UpdatePeak(counter.peakRegisteredBytes, current);
    counter.registrationUpdateCount.fetch_add(1, std::memory_order_relaxed);
}

void BufferPoolStats::RecordReuse(BufferPoolStatsMode mode)
{
    if (enabled)
    {
        counters[static_cast<size_t>(mode)].reuseCount.fetch_add(1, std::memory_order_relaxed);
    }
}

void BufferPoolStats::RecordGrowth(BufferPoolStatsMode mode)
{
    if (enabled)
    {
        counters[static_cast<size_t>(mode)].growthCount.fetch_add(1, std::memory_order_relaxed);
    }
}

const char *BufferPoolStats::ModeName(BufferPoolStatsMode mode)
{
    switch (mode)
    {
        case BufferPoolStatsMode::Legacy:
            return "legacy";
        case BufferPoolStatsMode::Dynamic:
            return "dynamic";
        case BufferPoolStatsMode::Static:
            return "static";
        default:
            return "unknown";
    }
}

void BufferPoolStats::Print() const
{
    if (!enabled)
    {
        return;
    }
    for (size_t index = 0; index < counters.size(); ++index)
    {
        const auto &counter = counters[index];
        if (counter.allocationCount.load(std::memory_order_relaxed) == 0)
        {
            continue;
        }
        std::cerr << "[BufferPoolStats] mode=" << ModeName(static_cast<BufferPoolStatsMode>(index))
                  << " current_allocated_bytes=" << counter.currentAllocatedBytes.load(std::memory_order_relaxed)
                  << " peak_allocated_bytes=" << counter.peakAllocatedBytes.load(std::memory_order_relaxed)
                  << " current_registered_bytes=" << counter.currentRegisteredBytes.load(std::memory_order_relaxed)
                  << " peak_registered_bytes=" << counter.peakRegisteredBytes.load(std::memory_order_relaxed)
                  << " allocations=" << counter.allocationCount.load(std::memory_order_relaxed)
                  << " frees=" << counter.freeCount.load(std::memory_order_relaxed)
                  << " registrations=" << counter.registrationCount.load(std::memory_order_relaxed)
                  << " registration_updates=" << counter.registrationUpdateCount.load(std::memory_order_relaxed)
                  << " reuses=" << counter.reuseCount.load(std::memory_order_relaxed)
                  << " growths=" << counter.growthCount.load(std::memory_order_relaxed) << std::endl;
    }
}
