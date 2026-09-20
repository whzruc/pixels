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
 * @create 2023-02-27
 */
#include "physical/storage/LocalFS.h"
#include "physical/io/PhysicalLocalReader.h"
#include "physical/natives/DirectUringRandomAccessFile.h"
#include "physical/natives/DirectUringRandomAccessFileNonFixed.h"
#include "physical/natives/DirectUringRandomAccessFileDynamic.h"
#include "physical/natives/DirectUringRandomAccessFileStatic.h"
#ifdef PIXELS_ENABLE_SPDK
#include "physical/natives/DirectSpdkRandomAccessFile.h"
#endif
#include "physical/ThreadContext.h"

using namespace  pixels;
#include <utility>
#include "profiler/TimeProfiler.h"

PhysicalLocalReader::PhysicalLocalReader(std::shared_ptr <Storage> storage, std::string path_)
{
    // TODO: should support async
    if (std::dynamic_pointer_cast<LocalFS>(storage).get() != nullptr)
    {
        local = std::dynamic_pointer_cast<LocalFS>(storage);
    }
    else
    {
        throw std::runtime_error("Storage is not LocalFS.");
    }
    if (path_.rfind("file://", 0) != std::string::npos)
    {
        // remove the scheme.
        path_.erase(0, 7);
    }
    path = std::move(path_);
    raf = local->openRaf(path);
    // TODO: get fileid.
    numRequests = 1;
    asyncNumRequests = 0;
}

std::shared_ptr <ByteBuffer> PhysicalLocalReader::readFully(int length)
{
    numRequests++;
    return raf->readFully(length);
}

std::shared_ptr <ByteBuffer> PhysicalLocalReader::readFully(int length, std::shared_ptr <ByteBuffer> bb)
{
    numRequests++;
    return raf->readFully(length, bb);
}

void PhysicalLocalReader::close()
{
    numRequests++;
    raf->close();
}

long PhysicalLocalReader::getFileLength()
{
    numRequests++;
    return raf->length();
}

void PhysicalLocalReader::seek(long desired)
{
    numRequests++;
    raf->seek(desired);
}

long PhysicalLocalReader::readLong()
{
    return raf->readLong();
}

char PhysicalLocalReader::readChar()
{
    return raf->readChar();
}

int PhysicalLocalReader::readInt()
{
    return raf->readInt();
}

std::string PhysicalLocalReader::getName()
{
    if (path.empty())
    {
        return "";
    }
    return path.substr(path.find_last_of('/') + 1);
}

std::string PhysicalLocalReader::getPath()
{
    return path;
}

std::shared_ptr <ByteBuffer> PhysicalLocalReader::readAsync(int length, std::shared_ptr <ByteBuffer> buffer, int index)
{
    numRequests++;
#ifdef PIXELS_ENABLE_SPDK
    if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "spdk")
    {
        auto directRaf = std::static_pointer_cast<DirectSpdkRandomAccessFile>(raf);
        return directRaf->readAsync(length, std::move(buffer), index);
    }
#endif
    if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "iouring")
    {
        // Check if global static buffer pool is enabled
        bool useStaticBufferPool = false;
        try {
            useStaticBufferPool = ConfigFactory::Instance().boolCheckProperty("pixel.enable.globalStaticBytebuffer");
        } catch (...) {
            useStaticBufferPool = false;
        }
        
        if (useStaticBufferPool && ThreadContext::HasContext()) {
            // Global static buffer pool is enabled - note: API is different
            // This should not be called directly in this mode
            // The PixelsRecordReaderImpl should use a different code path
            throw InvalidArgumentException(
                "PhysicalLocalReader::readAsync: GlobalStaticBufferPool mode requires different API. "
                "Use readAsyncWithColumn() instead.");
        }
        
        // Check if dynamic buffer pool is enabled
        bool useDynamicBuffer = false;
        try {
            useDynamicBuffer = ConfigFactory::Instance().boolCheckProperty("pixels.enable.dynamic.buffer");
        } catch (...) {
            useDynamicBuffer = false;
        }
        
        // Check if we should use fixed buffers
        bool useFixedBuffer = true;
        try {
            useFixedBuffer = ConfigFactory::Instance().boolCheckProperty("localfs.iouring.use.fixed.buffer");
        } catch (...) {
            useFixedBuffer = true; // default to fixed buffer for backward compatibility
        }
        
        if (useDynamicBuffer) {
            // Use dynamic buffer pool version
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileDynamic>(raf);
            return directRaf->readAsync(length, std::move(buffer), index);
        } else if (useFixedBuffer) {
            // Use static buffer pool version with fixed buffers
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFile>(raf);
            return directRaf->readAsync(length, std::move(buffer), index);
        } else {
            // Use non-fixed buffer version
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileNonFixed>(raf);
            return directRaf->readAsync(length, std::move(buffer), index);
        }
    }
    else if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "aio")
    {
        throw InvalidArgumentException("PhysicalLocalReader::readAsync: We don't support aio for our async read yet.");
    }
    else
    {
        throw InvalidArgumentException("PhysicalLocalReader::readAsync: the async read method is unknown. ");
    }

}

std::shared_ptr <ByteBuffer> PhysicalLocalReader::readAsync(int length, std::shared_ptr <ByteBuffer> buffer, int bufferIdx, std::string columnName)
{
    numRequests++;
    if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "iouring")
    {
        auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileStatic>(raf);
        return directRaf->readAsync(length, std::move(buffer), columnName, bufferIdx);
    }
    else
    {
        throw InvalidArgumentException("PhysicalLocalReader::readAsync: the async read method is unknown.");
    }
}

void PhysicalLocalReader::readAsyncSubmit(uint32_t size)
{
    numRequests++;
#ifdef PIXELS_ENABLE_SPDK
    if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "spdk")
    {
        auto directRaf = std::static_pointer_cast<DirectSpdkRandomAccessFile>(raf);
        directRaf->readAsyncSubmit(static_cast<int>(size));
        return;
    }
#endif
    if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "iouring")
    {
        // Check if global static buffer pool is enabled
        bool useStaticBufferPool = false;
        try {
            useStaticBufferPool = ConfigFactory::Instance().boolCheckProperty("pixel.enable.globalStaticBytebuffer");
        } catch (...) {
            useStaticBufferPool = false;
        }
        
        if (useStaticBufferPool && ThreadContext::HasContext()) {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileStatic>(raf);
            directRaf->readAsyncSubmit(size);
            return;
        }
        
        // Check if dynamic buffer pool is enabled
        bool useDynamicBuffer = false;
        try {
            useDynamicBuffer = ConfigFactory::Instance().boolCheckProperty("pixels.enable.dynamic.buffer");
        } catch (...) {
            useDynamicBuffer = false;
        }
        
        // Check if we should use fixed buffers
        bool useFixedBuffer = true;
        try {
            useFixedBuffer = ConfigFactory::Instance().boolCheckProperty("localfs.iouring.use.fixed.buffer");
        } catch (...) {
            useFixedBuffer = true; // default to fixed buffer for backward compatibility
        }
        
        if (useDynamicBuffer) {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileDynamic>(raf);
            directRaf->readAsyncSubmit(size);
        } else if (useFixedBuffer) {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFile>(raf);
            directRaf->readAsyncSubmit(size);
        } else {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileNonFixed>(raf);
            directRaf->readAsyncSubmit(size);
        }
    }
    else if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "aio")
    {
        throw InvalidArgumentException("PhysicalLocalReader::readAsync: We don't support aio for our async read yet.");
    }
    else
    {
        throw InvalidArgumentException("PhysicalLocalReader::readAsync: the async read method is unknown. ");
    }
}

void PhysicalLocalReader::readAsyncComplete(uint32_t size)
{
    numRequests++;
#ifdef PIXELS_ENABLE_SPDK
    if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "spdk")
    {
        auto directRaf = std::static_pointer_cast<DirectSpdkRandomAccessFile>(raf);
        directRaf->readAsyncComplete(static_cast<int>(size));
        return;
    }
#endif
    if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "iouring")
    {
        // Check if global static buffer pool is enabled
        bool useStaticBufferPool = false;
        try {
            useStaticBufferPool = ConfigFactory::Instance().boolCheckProperty("pixel.enable.globalStaticBytebuffer");
        } catch (...) {
            useStaticBufferPool = false;
        }
        
        if (useStaticBufferPool && ThreadContext::HasContext()) {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileStatic>(raf);
            directRaf->readAsyncComplete(size);
            return;
        }
        
        // Check if dynamic buffer pool is enabled
        bool useDynamicBuffer = false;
        try {
            useDynamicBuffer = ConfigFactory::Instance().boolCheckProperty("pixels.enable.dynamic.buffer");
        } catch (...) {
            useDynamicBuffer = false;
        }
        
        // Check if we should use fixed buffers
        bool useFixedBuffer = true;
        try {
            useFixedBuffer = ConfigFactory::Instance().boolCheckProperty("localfs.iouring.use.fixed.buffer");
        } catch (...) {
            useFixedBuffer = true; // default to fixed buffer for backward compatibility
        }
        
        if (useDynamicBuffer) {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileDynamic>(raf);
            directRaf->readAsyncComplete(size);
        } else if (useFixedBuffer) {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFile>(raf);
            directRaf->readAsyncComplete(size);
        } else {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileNonFixed>(raf);
            directRaf->readAsyncComplete(size);
        }
    }
    else if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "aio")
    {
        throw InvalidArgumentException("PhysicalLocalReader::readAsync: We don't support aio for our async read yet.");
    }
    else
    {
        throw InvalidArgumentException("PhysicalLocalReader::readAsync: the async read method is unknown. ");
    }
}

void PhysicalLocalReader::readAsyncSubmitAndComplete(uint32_t size)
{
    numRequests++;
#ifdef PIXELS_ENABLE_SPDK
    if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "spdk")
    {
        auto directRaf = std::static_pointer_cast<DirectSpdkRandomAccessFile>(raf);
        // SPDK: submit is a no-op; just poll completions
        directRaf->readAsyncSubmit(static_cast<int>(size));
        ::TimeProfiler::Instance().Start("async wait");
        directRaf->readAsyncComplete(static_cast<int>(size));
        ::TimeProfiler::Instance().End("async wait");
        return;
    }
#endif
    if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "iouring")
    {
        // Check if global static buffer pool is enabled
        bool useStaticBufferPool = false;
        try {
            useStaticBufferPool = ConfigFactory::Instance().boolCheckProperty("pixel.enable.globalStaticBytebuffer");
        } catch (...) {
            useStaticBufferPool = false;
        }
        
        if (useStaticBufferPool && ThreadContext::HasContext()) {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileStatic>(raf);
            directRaf->readAsyncSubmit(size);
            ::TimeProfiler::Instance().Start("async wait");
            directRaf->readAsyncComplete(size);
            ::TimeProfiler::Instance().End("async wait");
            return;
        }
        
        // Check if dynamic buffer pool is enabled
        bool useDynamicBuffer = false;
        try {
            useDynamicBuffer = ConfigFactory::Instance().boolCheckProperty("pixels.enable.dynamic.buffer");
        } catch (...) {
            useDynamicBuffer = false;
        }
        
        // Check if we should use fixed buffers
        bool useFixedBuffer = true;
        try {
            useFixedBuffer = ConfigFactory::Instance().boolCheckProperty("localfs.iouring.use.fixed.buffer");
        } catch (...) {
            useFixedBuffer = true; // default to fixed buffer for backward compatibility
        }
        
        if (useDynamicBuffer) {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileDynamic>(raf);
            directRaf->readAsyncSubmit(size);
            ::TimeProfiler::Instance().Start("async wait");
            directRaf->readAsyncComplete(size);
            ::TimeProfiler::Instance().End("async wait");
        } else if (useFixedBuffer) {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFile>(raf);
            directRaf->readAsyncSubmit(size);
            ::TimeProfiler::Instance().Start("async wait");
            directRaf->readAsyncComplete(size);
            ::TimeProfiler::Instance().End("async wait");
        } else {
            auto directRaf = std::static_pointer_cast<DirectUringRandomAccessFileNonFixed>(raf);
            directRaf->readAsyncSubmit(size);
            ::TimeProfiler::Instance().Start("async wait");
            directRaf->readAsyncComplete(size);
            ::TimeProfiler::Instance().End("async wait");
        }
    }
    else if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "aio")
    {
        throw InvalidArgumentException("PhysicalLocalReader::readAsync: We don't support aio for our async read yet.");
    }
    else
    {
        throw InvalidArgumentException("PhysicalLocalReader::readAsync: the async read method is unknown. ");
    }
}
