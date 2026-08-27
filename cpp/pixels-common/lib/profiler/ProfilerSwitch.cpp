/*
 * Copyright 2023 PixelsDB.
 *
 * This file is part of Pixels.
 *
 * Pixels is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Pixels is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public
 * License along with Pixels.  If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "profiler/ProfilerSwitch.h"

#include "utils/ConfigFactory.h"

#include <mutex>

namespace
{
bool profilerEnabled = false;
std::once_flag profilerInitFlag;

void InitializeProfilerSwitch()
{
    profilerEnabled = ConfigFactory::Instance().getBoolProperty(
            "pixel.enable.profiler", false);
}
}

bool IsPixelsProfilerEnabled()
{
    std::call_once(profilerInitFlag, InitializeProfilerSwitch);
    return profilerEnabled;
}
