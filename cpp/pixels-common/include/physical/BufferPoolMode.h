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

#ifndef PIXELS_BUFFER_POOL_MODE_H
#define PIXELS_BUFFER_POOL_MODE_H

#include "exception/InvalidArgumentException.h"
#include "utils/ConfigFactory.h"

enum class BufferPoolMode
{
    Legacy,
    Dynamic
};

inline BufferPoolMode GetBufferPoolMode()
{
    const auto mode = ConfigFactory::Instance().getProperty("pixel.bufferpool.mode", "legacy");
    if (mode == "legacy")
    {
        return BufferPoolMode::Legacy;
    }
    if (mode == "dynamic")
    {
        return BufferPoolMode::Dynamic;
    }
    throw InvalidArgumentException("Unknown pixel.bufferpool.mode: " + mode);
}

#endif  // PIXELS_BUFFER_POOL_MODE_H
