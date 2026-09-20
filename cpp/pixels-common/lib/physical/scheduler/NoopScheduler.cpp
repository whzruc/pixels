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
 * @create 2023-03-08
 */
#include "physical/scheduler/NoopScheduler.h"

#include <pixels_generated.h>
#include <physical/ThreadContext.h>

#include "exception/InvalidArgumentException.h"
#include "physical/io/PhysicalLocalReader.h"
#include "profiler/TimeProfiler.h"

namespace
{
    bool BufferIdxDebugEnabled()
    {
        try
        {
            return ConfigFactory::Instance().boolCheckProperty("pixels.debug.bufferidx");
        }
        catch (...)
        {
            return false;
        }
    }

    int BufferIdxDebugThread()
    {
        try
        {
            return std::stoi(ConfigFactory::Instance().getProperty("pixels.debug.bufferidx.thread"));
        }
        catch (...)
        {
            return -1;
        }
    }

    bool ShouldPrintBufferIdxDebug(int threadId)
    {
        if (!BufferIdxDebugEnabled())
        {
            return false;
        }
        int debugThread = BufferIdxDebugThread();
        return debugThread < 0 || debugThread == threadId;
    }
}

Scheduler *NoopScheduler::instance = nullptr;

Scheduler *NoopScheduler::Instance()
{
    if (instance == nullptr)
    {
        instance = new NoopScheduler();
    }
    return instance;
}

std::vector <std::shared_ptr<ByteBuffer>> NoopScheduler::executeBatch(std::shared_ptr <PhysicalReader> reader,
                                                                      RequestBatch batch, long queryId)
{
    return executeBatch(reader, batch, {}, queryId);
}


std::vector <std::shared_ptr<ByteBuffer>>
NoopScheduler::executeBatch(std::shared_ptr <PhysicalReader> reader, RequestBatch batch,
                            std::vector <std::shared_ptr<ByteBuffer>> reuseBuffers, long queryId)
{
    PROFILE_START("NoopScheduler.executeBatch.Total");
    auto requests = batch.getRequests();
    std::vector <std::shared_ptr<ByteBuffer>> results;
    results.resize(batch.getSize());
    if (ConfigFactory::Instance().boolCheckProperty("localfs.enable.async.io") && reuseBuffers.size() > 0)
    {
        // async read
        auto localReader = std::static_pointer_cast<PhysicalLocalReader>(reader);
        for (int i = 0; i < batch.getSize(); i++)
        {
            Request request = requests[i];
            localReader->seek(request.start);
            bool useStaticBufferPool = false;
            try {
                useStaticBufferPool = ConfigFactory::Instance().boolCheckProperty("pixel.enable.globalStaticBytebuffer");
            } catch (...) {
                useStaticBufferPool = false;
            }

            if (useStaticBufferPool && pixels::ThreadContext::HasContext())
            {
                int bufferIdx = ::BufferPool::GetNextBufferIdx();
                if (ShouldPrintBufferIdxDebug(pixels::ThreadContext::GetThreadId()))
                {
                    std::cout << "[BufferIdxDebug] scheduler_submit"
                              << " request_index=" << i
                              << " thread=" << pixels::ThreadContext::GetThreadId()
                              << " column=" << request.columnName
                              << " bufferIdx=" << bufferIdx
                              << " currBufferIdx=" << ::BufferPool::GetCurrentBufferIdx()
                              << " nextBufferIdx=" << ::BufferPool::GetNextBufferIdx()
                              << " start=" << request.start
                              << " length=" << request.length
                              << std::endl;
                }
                results.at(i) = localReader->readAsync(request.length, reuseBuffers.at(i), bufferIdx, request.columnName);
            }
            else
            {
                results.at(i) = localReader->readAsync(request.length, reuseBuffers.at(i), request.bufferId);
            }
            }
        localReader->readAsyncSubmit(batch.getSize());
    }
    else
    {
        // sync read
        for (int i = 0; i < batch.getSize(); i++)
        {
            Request request = requests[i];
            reader->seek(request.start);
            if (reuseBuffers.size() > 0)
            {
                results.at(i) = reader->readFully(request.length, reuseBuffers.at(i));
            }
            else
            {
                results.at(i) = reader->readFully(request.length);
            }

        }
    }
    PROFILE_END("NoopScheduler.executeBatch.Total");
    return results;

}


NoopScheduler::~NoopScheduler()
{
    delete instance;
    instance = nullptr;
}
