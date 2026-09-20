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
 * @create 2023-03-02
 */
#include "physical/natives/DirectRandomAccessFile.h"

#include "physical/natives/DirectIoLib.h"
#include "utils/ConfigFactory.h"

#include <cstdio>
#include <malloc.h>
#include "profiler/CountProfiler.h"
#include "profiler/TimeProfiler.h"
#include "physical/allocator/OrdinaryAllocator.h"
#include "physical/allocator/BufferPoolAllocator.h"

DirectRandomAccessFile::DirectRandomAccessFile(const std::string &file)
{
    FILE *fp = fopen(file.c_str(), "r");
    // checking if the file exist or not
    if (fp == nullptr)
    {
        throw std::runtime_error("DirectRandomAccessFile: File not found or fd exceeds the limitation. ");
    }
    fseek(fp, 0L, SEEK_END);
    // calculating the size of the file
    len = ftell(fp);
    // closing the file
    fclose(fp);
    fsBlockSize = std::stoi(ConfigFactory::Instance().getProperty("localfs.block.size"));
    enableDirect = ConfigFactory::Instance().boolCheckProperty("localfs.enable.direct.io");
    if (enableDirect)
    {
        fd = open(file.c_str(), O_RDONLY | O_DIRECT);
    }
    else
    {
        fd = open(file.c_str(), O_RDONLY);
        smallBuffer = std::make_shared<ByteBuffer>(fsBlockSize);
    }
    offset = 0;

    bufferValid = false;
    directIoLib = std::make_shared<DirectIoLib>(fsBlockSize);
    try
    {
        smallDirectBuffer = directIoLib->allocateDirectBuffer(fsBlockSize);
    }
    catch (...)
    {
        throw std::runtime_error("failed to allocate buffer");
    }
    allocator = std::make_shared<BufferPoolAllocator>();

}

void DirectRandomAccessFile::close()
{
    largeBuffers.clear();

    if (fd != -1 && ::close(fd) != 0)
    {
        throw std::runtime_error("File is not closed properly");
    }
    fd = -1;
    offset = 0;
    len = 0;
}

std::shared_ptr <ByteBuffer> DirectRandomAccessFile::readFully(int len)
{
    PROFILE_START("Pixels.Pread.ReadFully.Total");
    if (enableDirect)
    {
        PROFILE_START("Pixels.Pread.ReadFully.Allocate");
        auto directBuffer = directIoLib->allocateDirectBuffer(len);
        PROFILE_END("Pixels.Pread.ReadFully.Allocate");
        PROFILE_START("Pixels.Pread.ReadFully.DirectIO");
        auto buffer = directIoLib->read(fd, offset, directBuffer, len);
        PROFILE_END("Pixels.Pread.ReadFully.DirectIO");
        seek(offset + len);
        largeBuffers.emplace_back(directBuffer);
        PROFILE_END("Pixels.Pread.ReadFully.Total");
        return buffer;
    }
    else
    {
        PROFILE_START("Pixels.Pread.ReadFully.Allocate");
        auto buffer = allocator->allocate(len);
        PROFILE_END("Pixels.Pread.ReadFully.Allocate");
        PROFILE_START("Pixels.Pread.ReadFully.Syscall");
        if (pread(fd, buffer->getPointer(), len, offset) == -1)
        {
            throw std::runtime_error("pread fail");
        }
        PROFILE_END("Pixels.Pread.ReadFully.Syscall");
        seek(offset + len);
        largeBuffers.emplace_back(buffer);
        PROFILE_END("Pixels.Pread.ReadFully.Total");
        return buffer;
    }

}

std::shared_ptr <ByteBuffer> DirectRandomAccessFile::readFully(int len, std::shared_ptr <ByteBuffer> bb)
{
    PROFILE_START("Pixels.Pread.ReadFullyReuse.Total");
    if (enableDirect)
    {
        PROFILE_START("Pixels.Pread.ReadFullyReuse.DirectIO");
        auto buffer = directIoLib->read(fd, offset, bb, len);
        PROFILE_END("Pixels.Pread.ReadFullyReuse.DirectIO");
        seek(offset + len);
        PROFILE_END("Pixels.Pread.ReadFullyReuse.Total");
        return buffer;
    }
    else
    {
        PROFILE_START("Pixels.Pread.ReadFullyReuse.Syscall");
        if (pread(fd, bb->getPointer(), len, offset) == -1)
        {
            throw std::runtime_error("pread fail");
        }
        PROFILE_END("Pixels.Pread.ReadFullyReuse.Syscall");
        seek(offset + len);
        PROFILE_START("Pixels.Pread.ReadFullyReuse.WrapBuffer");
        auto buffer = std::make_shared<ByteBuffer>(*bb, 0, len);
        PROFILE_END("Pixels.Pread.ReadFullyReuse.WrapBuffer");
        PROFILE_END("Pixels.Pread.ReadFullyReuse.Total");
        return buffer;
    }
}


long DirectRandomAccessFile::length()
{
    return len;
}

void DirectRandomAccessFile::seek(long off)
{
    if (bufferValid && off > offset - smallBuffer->getReadPos() &&
        off < offset + smallBuffer->bytesRemaining())
    {
        smallBuffer->setReadPos(off - offset + smallBuffer->getReadPos());
    }
    else
    {
        bufferValid = false;
    }
    offset = off;
}

long DirectRandomAccessFile::readLong()
{
    if (!bufferValid || smallBuffer->bytesRemaining() < sizeof(long))
    {
        populatedBuffer();
    }
    offset += sizeof(long);
    return smallBuffer->getLong();
}

int DirectRandomAccessFile::readInt()
{
    if (!bufferValid || smallBuffer->bytesRemaining() < sizeof(int))
    {
        populatedBuffer();
    }
    offset += sizeof(int);
    return smallBuffer->getInt();
}

char DirectRandomAccessFile::readChar()
{
    if (!bufferValid || smallBuffer->bytesRemaining() < sizeof(char))
    {
        populatedBuffer();
    }
    offset += sizeof(char);
    return smallBuffer->getChar();
}

void DirectRandomAccessFile::populatedBuffer()
{
    PROFILE_START("Pixels.Pread.Metadata.Total");
    if (enableDirect)
    {
        PROFILE_START("Pixels.Pread.Metadata.DirectIO");
        smallBuffer = directIoLib->read(fd, offset, smallDirectBuffer, fsBlockSize);
        PROFILE_END("Pixels.Pread.Metadata.DirectIO");
        bufferValid = true;
    }
    else
    {
        PROFILE_START("Pixels.Pread.Metadata.Syscall");
        if (pread(fd, smallBuffer->getPointer(), fsBlockSize, offset) == -1)
        {
            throw std::runtime_error("pread fail");
        }
        PROFILE_END("Pixels.Pread.Metadata.Syscall");
        smallBuffer->resetPosition();
        bufferValid = true;
    }
    PROFILE_END("Pixels.Pread.Metadata.Total");

}





