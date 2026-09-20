/*
 * Copyright 2023 PixelsDB.
 *
 * This file is part of Pixels.
 *
 * Pixels is free software: you can redistribute it and/or modify
 * it under the terms of the Affero GNU General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
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

/*
 * @author liyu
 * @create 2023-02-28
 */
#include "physical/storage/LocalFS.h"
#include "physical/natives/DirectRandomAccessFile.h"
#include "physical/natives/DirectUringRandomAccessFile.h"  // Static buffer pool version with fixed buffers
#include "physical/natives/DirectUringRandomAccessFileNonFixed.h"  // Static buffer pool version without fixed buffers
#include "physical/natives/DirectUringRandomAccessFileDynamic.h"  // Dynamic buffer pool version
#include "physical/natives/DirectUringRandomAccessFileStatic.h"  // Global static buffer pool version
#ifdef PIXELS_ENABLE_SPDK
#include "physical/natives/DirectSpdkRandomAccessFile.h"
#endif
#include "physical/ThreadContext.h"
#include "physical/FilePath.h"
#include "utils/ConfigFactory.h"
#include <filesystem>

namespace fs = std::filesystem;

std::string LocalFS::SchemePrefix = "file://";

LocalFS::LocalFS()
{

};

Storage::Scheme LocalFS::getScheme()
{
    return file;
}

std::string LocalFS::ensureSchemePrefix(const std::string &path) const
{
    if (path.rfind(SchemePrefix, 0) != std::string::npos)
    {
        return path;
    }
    if (path.find("://") != std::string::npos)
    {
        throw std::invalid_argument("Path '" + path +
                                    "' already has a different scheme prefix than '" + SchemePrefix + "'.");
    }
    return SchemePrefix + path;
}

std::shared_ptr <PixelsRandomAccessFile> LocalFS::openRaf(const std::string &path)
{
#ifdef PIXELS_ENABLE_SPDK
    // SPDK user-space NVMe driver path (highest priority).
    // Enabled by: localfs.enable.spdk=true  in pixels-cpp.properties.
    // Requires bind_vfio.sh + gen_lba_map.py to have been run beforehand.
    {
        bool useSpdk = false;
        try {
            useSpdk = ConfigFactory::Instance().boolCheckProperty("localfs.enable.spdk");
        } catch (...) {}
        if (useSpdk) {
            return std::make_shared<DirectSpdkRandomAccessFile>(path);
        }
    }
#endif // PIXELS_ENABLE_SPDK

    // Check if global static buffer pool is enabled
    bool useStaticBufferPool = false;
    try {
        useStaticBufferPool = ConfigFactory::Instance().boolCheckProperty("pixel.enable.globalStaticBytebuffer");
    } catch (...) {
        useStaticBufferPool = false;
    }
    
    if (useStaticBufferPool && pixels::ThreadContext::HasContext())
    {
        // Use global static buffer pool version with pre-registered buffers and ring
        int threadId = pixels::ThreadContext::GetThreadId();
        struct io_uring* ring = pixels::ThreadContext::GetRing();
        return std::make_shared<DirectUringRandomAccessFileStatic>(path, threadId, ring);
    }
    
    // Check if dynamic buffer pool is enabled
    bool useDynamicBuffer = false;
    try {
        useDynamicBuffer = ConfigFactory::Instance().boolCheckProperty("pixels.enable.dynamic.buffer");
    } catch (...) {
        // Default to false if property not found
        useDynamicBuffer = false;
    }
    
    if (useDynamicBuffer)
    {
        // Use dynamic buffer pool version with io_uring sparse registration
        return std::make_shared<DirectUringRandomAccessFileDynamic>(path);
    }
    else
    {
        // Check if we should use fixed buffers
        bool useFixedBuffer = true;
        try {
            useFixedBuffer = ConfigFactory::Instance().boolCheckProperty("localfs.iouring.use.fixed.buffer");
        } catch (...) {
            useFixedBuffer = true; // default to fixed buffer for backward compatibility
        }
        
        if (useFixedBuffer) {
            // Use static buffer pool version with io_uring fixed buffers
            return std::make_shared<DirectUringRandomAccessFile>(path);
        } else {
            // Use static buffer pool version with io_uring non-fixed buffers
            return std::make_shared<DirectUringRandomAccessFileNonFixed>(path);
        }
    }
}

std::vector <std::string> LocalFS::listPaths(const std::string &path)
{
    std::vector <std::string> paths;
    FilePath p(path);
    if (!p.valid)
    {
        throw std::runtime_error("Path " + path + " is not a valid local fs path.");
    }

    fs::path file(p.realPath);
    std::vector <fs::directory_entry> files;
    if (fs::is_directory(file))
    {
        for (const auto &entry: fs::directory_iterator(file))
        {
            files.push_back(entry);
        }
    }
    else
    {
        if (fs::exists(file))
        {
            files.push_back(fs::directory_entry(file));
        }
    }
    if (files.empty())
    {
        throw std::runtime_error("Failed to list files in path: " + p.realPath + ".");
    }
    else
    {
        for (const auto &eachFile: files)
        {
            paths.push_back(ensureSchemePrefix(eachFile.path().string()));
        }
    }
    return paths;
}

std::ifstream LocalFS::open(const std::string &path)
{
    FilePath p(path);
    if (!p.valid)
    {
        throw std::runtime_error("Path '" + path + "' is not a valid local fs path.");
    }
    fs::path file(p.realPath);
    if (fs::is_directory(file))
    {
        throw std::runtime_error("Path '" + p.realPath + "' is a directory, it must be a file.");
    }
    if (!fs::exists(file))
    {
        throw std::runtime_error("File '" + p.realPath + "' doesn't exists.");
    }
    return std::ifstream(file);
}

void LocalFS::close()
{
}

LocalFS::~LocalFS() = default;
