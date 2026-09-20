/*
 * Copyright 2023 PixelsDB.
 *
 * This file is part of Pixels.
 *
 * Pixels is free software: you can redistribute it and/or modify
 * it under the terms of the Affero GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Pixels is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Affero GNU General Public License for more details.
 *
 * You should have received a copy of the Affero GNU General Public
 * License along with Pixels.  If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "profiler/ProfilerSwitch.h"

#include "utils/ConfigFactory.h"

#include <mutex>

namespace
{
bool g_pixelsProfilerEnabled = true;
std::once_flag g_pixelsProfilerInitFlag;

void InitPixelsProfilerSwitch()
{
    g_pixelsProfilerEnabled =
            ConfigFactory::Instance().getBoolProperty("pixel.enable.profiler", false);
}
} // namespace

bool IsPixelsProfilerEnabled()
{
    std::call_once(g_pixelsProfilerInitFlag, InitPixelsProfilerSwitch);
    return g_pixelsProfilerEnabled;
}
