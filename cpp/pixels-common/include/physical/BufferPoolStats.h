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

#ifndef PIXELS_BUFFER_POOL_STATS_H
#define PIXELS_BUFFER_POOL_STATS_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

enum class BufferPoolStatsMode : size_t
{
    Legacy = 0,
    Dynamic = 1,
    Static = 2,
    Count = 3
};

class BufferPoolStats
{
   public:
    static BufferPoolStats &Instance();

    bool IsEnabled() const;

    void RecordAllocation(BufferPoolStatsMode mode, uint64_t bytes);
    void RecordFree(BufferPoolStatsMode mode, uint64_t bytes);
    void RecordRegistration(BufferPoolStatsMode mode, uint64_t bytes);
    void RecordUnregistration(BufferPoolStatsMode mode, uint64_t bytes);
    void RecordRegistrationUpdate(BufferPoolStatsMode mode, uint64_t oldBytes, uint64_t newBytes);
    void RecordReuse(BufferPoolStatsMode mode);
    void RecordGrowth(BufferPoolStatsMode mode);
    void Print() const;

   private:
    struct Counters
    {
        std::atomic<uint64_t> currentAllocatedBytes{0};
        std::atomic<uint64_t> peakAllocatedBytes{0};
        std::atomic<uint64_t> currentRegisteredBytes{0};
        std::atomic<uint64_t> peakRegisteredBytes{0};
        std::atomic<uint64_t> allocationCount{0};
        std::atomic<uint64_t> freeCount{0};
        std::atomic<uint64_t> registrationCount{0};
        std::atomic<uint64_t> registrationUpdateCount{0};
        std::atomic<uint64_t> reuseCount{0};
        std::atomic<uint64_t> growthCount{0};
    };

    BufferPoolStats();

    static void UpdatePeak(std::atomic<uint64_t> &peak, uint64_t value);
    static void Subtract(std::atomic<uint64_t> &value, uint64_t amount);
    static const char *ModeName(BufferPoolStatsMode mode);

    bool enabled;
    std::array<Counters, static_cast<size_t>(BufferPoolStatsMode::Count)> counters;
};

#endif
