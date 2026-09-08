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
 * @create 2023-03-07
 */
#include "reader/PixelsRecordReaderImpl.h"
#include "physical/io/PhysicalLocalReader.h"
#include "profiler/CountProfiler.h"
#include "profiler/TimeProfiler.h"
#include "physical/BufferPool.h"
#include "physical/natives/DirectUringRandomAccessFile.h"
#include "physical/natives/DirectUringRandomAccessFileNonFixed.h"
#include "physical/DynamicBufferPool.h"
#include "physical/natives/DirectUringRandomAccessFileDynamic.h"
#include "physical/GlobalStaticBufferPool.h"
#include "physical/ThreadContext.h"
#ifdef PIXELS_ENABLE_SPDK
#include "physical/SpdkBufferPool.h"
#endif

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

PixelsRecordReaderImpl::PixelsRecordReaderImpl(std::shared_ptr <PhysicalReader> reader,
                                               const pixels::fb::PostScript* pixelsPostScript,
                                               const pixels::fb::Footer* pixelsFooter,
                                               const PixelsReaderOption &opt,
                                               std::shared_ptr <PixelsFooterCache> pixelsFooterCache)
{
    physicalReader = reader;
    footer = pixelsFooter;
    postScript = pixelsPostScript;
    footerCache = pixelsFooterCache;
    option = opt;
    // TODO: intialize all kinds of variables
    queryId = option.getQueryId();
    RGStart = option.getRGStart();
    RGLen = option.getRGLen();
    batchSize = option.getBatchSize();
    // batchSize must be larger than STANDARD_VECTOR_SIZE
    // for test purpose, can we comment it temporarily
    // assert(batchSize >= STANDARD_VECTOR_SIZE);
    enabledFilterPushDown = option.isEnabledFilterPushDown();
    if (enabledFilterPushDown)
    {
        filter = option.getFilter();
    }
    else
    {
        filter = nullptr;
    }
    filterMask = nullptr;
    everRead = false;
    everPrepareRead = false;
    targetRGNum = 0;
    curRGIdx = 0;
    curRowInRG = 0;
    curRGRowCount = 0;
    pixelsBufferIdx = -1;
    // Use full path as cache key to avoid collisions across SSDs.
    fileName = physicalReader->getPath();
    enableEncodedVector = option.isEnableEncodedColumnVector();
    includedColumnNum = 0;
    endOfFile = false;
    resultRowBatch = nullptr;
    checkBeforeRead();
}

void PixelsRecordReaderImpl::checkBeforeRead()
{
    // get file schema
    auto fileColTypesFooterTypes = footer->types();
    auto fileColTypes = std::vector<const pixels::fb::Type*>{};
    for (int i = 0; i < fileColTypesFooterTypes->size(); i++)
    {
        fileColTypes.emplace_back(fileColTypesFooterTypes->Get(i));
    }
    // TODO: if fileCOlTypes == null
    fileSchema = TypeDescription::createSchema(fileColTypes);
    // TODO: getChildren == NULL
    // filter included columns
    includedColumnNum = 0;
    auto optionIncludedCols = option.getIncludedCols();
    // TODO: if size of cols is 0, create an empty row batch
    // TODO: what if false is caused? we must debug this! Currently I didn't understand why we need includedColumns yet. So just leave it alone.
    includedColumns.clear();
    includedColumns.resize(fileColTypes.size());
    std::vector<int> optionColsIndices;
    for (const auto &col: optionIncludedCols)
    {
        for (int j = 0; j < fileColTypes.size(); j++)
        {
            if (icompare(col, fileColTypes.at(j)->name()->str()))
            {
                optionColsIndices.emplace_back(j);
                includedColumns.at(j) = true;
                includedColumnNum++;
                break;
            }
        }

    }
    // TODO: check includedColumns
    // create result columns storing result column ids in user specified order
    resultColumns.clear();
    resultColumns.resize(includedColumnNum);
    for (int i = 0; i < includedColumnNum; i++)
    {
        resultColumns.at(i) = optionColsIndices[i];
    }


    auto optionColsIndicesSet = std::set<int>(
            optionColsIndices.begin(), optionColsIndices.end());
    int targetColumnNum = (int) optionColsIndicesSet.size();
    targetColumns.clear();
    targetColumns.resize(targetColumnNum);
    int targetColIdx = 0;
    for (int i = 0; i < includedColumns.size(); i++)
    {
        if (includedColumns[i])
        {
            targetColumns.at(targetColIdx) = i;
            targetColIdx++;
        }
    }

    // create column readers
    auto columnSchemas = fileSchema->getChildren();
    readers.clear();
    readers.resize(resultColumns.size());
    for (int i = 0; i < resultColumns.size(); i++)
    {
        int index = resultColumns[i];
        readers.at(i) = ColumnReaderBuilder::newColumnReader(columnSchemas.at(index));
    }

    // create result vectorized row batch
    for (int resultColumn: resultColumns)
    {
        includedColumnTypes.emplace_back(fileColTypes.at(resultColumn));
    }
    resultSchema = TypeDescription::createSchema(includedColumnTypes);

}


void PixelsRecordReaderImpl::UpdateRowGroupInfo()
{
    // if not end of file, update row count
    curRGRowCount = (int) footer->rowGroupInfos()->Get(targetRGs.at(curRGIdx))->numberOfRows();

    if (enabledFilterPushDown)
    {
        int length = std::min(batchSize, curRGRowCount);
        filterMask = std::make_shared<PixelsBitMask>(length);
    }

    curRGFooter = rowGroupFooters.at(curRGIdx);
    // refresh resultColumnsEncoded for reading the column vectors in the next row group.
    const pixels::fb::RowGroupEncoding* rgEncoding = rowGroupFooters.at(curRGIdx)->rowGroupEncoding();
    for (int i = 0; i < includedColumnNum; i++)
    {
        resultColumnsEncoded.at(i) =
                rgEncoding->columnChunkEncodings()->Get(resultColumns.at(i))
                        ->kind() != pixels::fb::EncodingKind_NONE
                && enableEncodedVector;
    }
    for (int i = 0; i < resultColumns.size(); i++)
    {
        curEncoding.at(i) = rgEncoding->columnChunkEncodings()->Get(resultColumns.at(i));
        curChunkBufferIndex.at(i) = resultColumns.at(i);
        curChunkIndex.at(i) = curRGFooter->rowGroupIndexEntry()
                                     ->columnChunkIndexEntries()->Get(resultColumns.at(i));
    }
    // This flag makes sure that each row group invokes read()
    everRead = false;
}


// If cross multiple row group, we only process one row group
// In the current configuration, one batch is 10000 rows. This function creates
// VectorizedRowBatch with some cols. Each column has 10000 elements. The columns
// read value from chunkBuffer.
std::shared_ptr <VectorizedRowBatch> PixelsRecordReaderImpl::readBatch(bool reuse)
{
    PROFILE_START("PixelsRecordReaderImpl.readBatch.Total");
    
    if (endOfFile)
    {
        endOfFile = true;
        PROFILE_END("PixelsRecordReaderImpl.readBatch.Total");
        return createEmptyEOFRowBatch(0);
    }
    
    if (!everRead)
    {
        PROFILE_START("PixelsRecordReaderImpl.readBatch.FirstRead");
        if (!read())
        {
            throw std::runtime_error("failed to read file");
        }
        PROFILE_END("PixelsRecordReaderImpl.readBatch.FirstRead");
    }

    PROFILE_START("PixelsRecordReaderImpl.readBatch.ComputeBatchSize");
    int curBatchSize = std::min(curRGRowCount - curRowInRG, std::min(batchSize, curRGRowCount));
    PROFILE_END("PixelsRecordReaderImpl.readBatch.ComputeBatchSize");

    // std::cout<<"ReadBatch: curRGRowCount: "<<curRGRowCount<<" curRowInRG: "<<curRowInRG<<" bathcSize: "<<batchSize<<std::endl;
    if (resultRowBatch == nullptr)
    {
        PROFILE_START("PixelsRecordReaderImpl.readBatch.CreateRowBatch");
        // std::cout<<"create new rowbatch"<<std::endl;
        resultRowBatch = resultSchema->createRowBatch(curBatchSize, resultColumnsEncoded);
        PROFILE_END("PixelsRecordReaderImpl.readBatch.CreateRowBatch");
    }
    else
    {
        PROFILE_START("PixelsRecordReaderImpl.readBatch.ResetRowBatch");
        resultRowBatch->reset();
        PROFILE_END("PixelsRecordReaderImpl.readBatch.ResetRowBatch");
        if (curBatchSize != resultRowBatch->maxSize)
        {
            PROFILE_START("PixelsRecordReaderImpl.readBatch.ResizeRowBatch");
            resultRowBatch->resize(curBatchSize);
            PROFILE_END("PixelsRecordReaderImpl.readBatch.ResizeRowBatch");
        }
    }

    auto columnVectors = resultRowBatch->cols;
    if (filterMask != nullptr)
    {
        PROFILE_START("PixelsRecordReaderImpl.readBatch.ResetFilterMask");
        filterMask->set();
        PROFILE_END("PixelsRecordReaderImpl.readBatch.ResetFilterMask");
    }

    // if(asyncReadRequestNum > 0)
    // {
    //     // PROFILE_START("PixelsRecordReaderImpl.readBatch.AsyncReadComplete");
    //     asyncReadComplete(asyncReadRequestNum);
    //     // PROFILE_END("PixelsRecordReaderImpl.readBatch.AsyncReadComplete");
    // }

    std::vector<int> filterColumnIndex;
    if (filter != nullptr)
    {
        for (auto &filterCol: filter->filters)
        {
            if (filterMask->isNone())
            {
                break;
            }
            int i = filterCol.first;
            int index = curChunkBufferIndex.at(i);
            auto &encoding = curEncoding.at(i);
            auto &chunkIndex = curChunkIndex.at(i);
            PROFILE_START("PixelsRecordReaderImpl.readBatch.ReadFilterColumns");
            readers.at(i)->read(chunkBuffers.at(index), encoding, curRowInRG, curBatchSize,
                                postScript->pixelStride(), resultRowBatch->rowCount,
                                columnVectors.at(i), chunkIndex, filterMask);
            PROFILE_END("PixelsRecordReaderImpl.readBatch.ReadFilterColumns");
            filterColumnIndex.emplace_back(index);
            PROFILE_START("PixelsRecordReaderImpl.readBatch.ApplyFilterExpr");
            PixelsFilter::ApplyFilter(columnVectors.at(i), *filterCol.second, *filterMask,
                                      resultSchema->getChildren().at(i));
            PROFILE_END("PixelsRecordReaderImpl.readBatch.ApplyFilterExpr");
        }
    }

    // Completion is performed before readBatch(), so this path does not wait for
    // I/O.  Keep profiling outside the per-column loop: this loop runs tens of
    // millions of times for ClickBench and steady_clock falls back to HPET on the
    // benchmark host, making fine-grained profiling materially change the query.
    PROFILE_START("PixelsRecordReaderImpl.readBatch.ReadDataColumns");
    for (int i = 0; i < resultColumns.size(); i++)
    {
        // Skip the columns that calculate the filter mask, since they are already processed
        int index = curChunkBufferIndex.at(i);
        if (std::find(filterColumnIndex.begin(), filterColumnIndex.end(), index) != filterColumnIndex.end())
        {
            continue;
        }
        auto &encoding = curEncoding.at(i);
        auto &chunkIndex = curChunkIndex.at(i);
        readers.at(i)->read(chunkBuffers.at(index), encoding, curRowInRG, curBatchSize,
                            postScript->pixelStride(), resultRowBatch->rowCount,
                            columnVectors.at(i), chunkIndex, filterMask);
    }
    PROFILE_END("PixelsRecordReaderImpl.readBatch.ReadDataColumns");

    // update current row index in the row group
    PROFILE_START("PixelsRecordReaderImpl.readBatch.FinalizeBatch");
    curRowInRG += curBatchSize;
    resultRowBatch->rowCount += curBatchSize;
    // update row group index if current row index exceeds max row count in the row group
    // std::cout<<"PixelsRecordReaderImpl:: curRGIdx: "<<curRGIdx<<
    // " curRowInRG: "<<curRowInRG<<" curRGRowCount: "<<curRGRowCount<<std::endl;
    // if (curRowInRG >= curRGRowCount)
    // {
    //     curRGIdx++;
    //
    //     // if (curRGIdx < targetRGNum)
    //     // {
    //     //     std::cout<<"curRGIdx: "<<curRGIdx<<" targetRGNum: "<<targetRGNum<<std::endl;
    //     //     UpdateRowGroupInfo();
    //     //     std::cout<<"curRowCount: "<<curRGRowCount<<std::endl;
    //     // }
    //     // else
    //     // {
    //     //     // if end of file, set result vectorized row batch endOfFile
    //     //     endOfFile = true;
    //     // }
    //     curRowInRG = 0;
    // }
    PROFILE_END("PixelsRecordReaderImpl.readBatch.FinalizeBatch");
    
    PROFILE_END("PixelsRecordReaderImpl.readBatch.Total");
    return resultRowBatch;
}


void PixelsRecordReaderImpl::nextRowGroup()
{
    if (curRowInRG >= curRGRowCount)
    {
        curRGIdx++;
        if (curRGIdx < targetRGNum)
        {
            std::cout<<"curRGIdx: "<<curRGIdx<<" targetRGNum: "<<targetRGNum<<std::endl;
            UpdateRowGroupInfo();
            std::cout<<"curRowCount: "<<curRGRowCount<<std::endl;
        }
        else
        {
            // if end of file, set result vectorized row batch endOfFile
            endOfFile = true;
        }
        curRowInRG = 0;
    }
}


void PixelsRecordReaderImpl::prepareRead()
{
    everPrepareRead = true;
    std::vector<bool> includedRGs;
    includedRGs.resize(RGLen);

    uint64_t includedRowNum = 0;
    // read row group statistics and find target row groups
    for (int i = 0; i < RGLen; i++)
    {
        includedRGs.at(i) = true;
        includedRowNum += footer->rowGroupInfos()->Get(RGStart + i)->numberOfRows();
    }
    targetRGs.clear();
    targetRGs.resize(RGLen);
    int targetRGIdx = 0;
    for (int i = 0; i < RGLen; i++)
    {
        if (includedRGs[i])
        {
            targetRGs.at(targetRGIdx) = i + RGStart;
            targetRGIdx++;
        }
    }
    targetRGNum = targetRGIdx;

    // TODO: if taregetRGNum == 0

    // read row group footers
    rowGroupFooters.clear();
    rowGroupFooters.resize(targetRGNum);
    rowGroupFooterBuffers.clear();
    rowGroupFooterBuffers.resize(targetRGNum);
    std::vector<bool> rowGroupFooterCacheHit;
    rowGroupFooterCacheHit.resize(targetRGNum);

    /**
     * Issue #114:
     * Use request batch and read scheduler to execute the read requests.
     *
     * Here, we create an empty batch as footer cache is very likely to be hit in
     * the subsequent queries on the same table.
     */
    RequestBatch requestBatch;
    std::vector<int> fis;
    std::vector <std::string> rgCacheIds;
    for (int i = 0; i < targetRGNum; i++)
    {
        int rgId = targetRGs[i];
        std::string rgCacheId = fileName + "-" + std::to_string(rgId);
        rgCacheIds.emplace_back(rgCacheId);
        if (footerCache != nullptr && footerCache->containsRGFooter(rgCacheId))
        {
            // cache hit
            rowGroupFooters.at(i) = footerCache->getRGFooter(rgCacheId);
            rowGroupFooterCacheHit.at(i) = true;
        }
        else
        {
            // cache miss, read from disk and put it into cache
            const pixels::fb::RowGroupInformation* rowGroupInformation = footer->rowGroupInfos()->Get(rgId);
            uint64_t footerOffset = rowGroupInformation->footerOffset();
            uint64_t footerLength = rowGroupInformation->footerLength();
            fis.push_back(i);
            requestBatch.add(queryId, (int) footerOffset, (int) footerLength);
            rowGroupFooterCacheHit.at(i) = false;
        }
    }
    Scheduler *scheduler = SchedulerFactory::Instance()->getScheduler();
    PROFILE_START("Pixels.Metadata.RowGroupFooterRead");
    auto bbs = scheduler->executeBatch(physicalReader, requestBatch, queryId);
    PROFILE_END("Pixels.Metadata.RowGroupFooterRead");
    // TODO: the return value should be unique_ptr?

    for (int i = 0; i < bbs.size(); i++)
    {
        if (!rowGroupFooterCacheHit.at(i))
        {
            const pixels::fb::RowGroupFooter* parsed =
                flatbuffers::GetRoot<pixels::fb::RowGroupFooter>((bbs[i]->getPointer()));
            rowGroupFooters.at(fis[i]) = parsed;
            // Keep the ByteBuffer alive so the FlatBuffer pointer stays valid.
            rowGroupFooterBuffers.at(fis[i]) = bbs[i];
            if (footerCache != nullptr)
            {
                // Pass the buffer to the cache so the pointer stays valid after
                // this reader is destroyed.
                footerCache->putRGFooter(rgCacheIds[fis[i]], bbs[i], parsed);
            }
        }
    }

    // Do NOT clear bbs here — rowGroupFooterBuffers now owns the references
    // we need. bbs itself will be destroyed at end of scope.
    resultColumnsEncoded.clear();
    resultColumnsEncoded.resize(includedColumnNum);

    curEncoding.resize(resultColumns.size());
    curChunkBufferIndex.resize(resultColumns.size());
    curChunkIndex.resize(resultColumns.size());
    // std::cout<<"PreparedRead:"<<std::endl;
    UpdateRowGroupInfo();
}

void PixelsRecordReaderImpl::asyncReadComplete(int requestSize)
{
    if (ConfigFactory::Instance().boolCheckProperty("localfs.enable.async.io")
        && asyncReadRequestNum > 0)
    {
        auto asyncLib = ConfigFactory::Instance().getProperty("localfs.async.lib");
        if (asyncLib == "spdk")
        {
            // SPDK submits immediately and its file object owns the exact set
            // of pending operations.  Complete all reads actually submitted
            // by this reader; DuckDB's projected-column count can differ from
            // the de-duplicated disk chunk count.
            auto localReader = std::static_pointer_cast<PhysicalLocalReader>(physicalReader);
            localReader->readAsyncComplete(asyncReadRequestNum);
            asyncReadRequestNum = 0;
        }
        else if (asyncLib == "iouring" && asyncReadRequestNum >= requestSize)
        {
            auto localReader = std::static_pointer_cast<PhysicalLocalReader>(physicalReader);
            localReader->readAsyncComplete(requestSize);
            asyncReadRequestNum -= requestSize;
        }
        else if (asyncLib == "aio")
        {
            throw InvalidArgumentException(
                    "PhysicalLocalReader::readAsync: We don't support aio for our async read yet.");
        }
    }

}


std::shared_ptr <PixelsBitMask> PixelsRecordReaderImpl::getFilterMask()
{
    return filterMask;
}

bool PixelsRecordReaderImpl::read()
{
    PROFILE_START("PixelsRecordReaderImpl.read.Total");
    
    if (!everPrepareRead)
    {
        PROFILE_START("PixelsRecordReaderImpl.read.PrepareRead");
        prepareRead();
        PROFILE_END("PixelsRecordReaderImpl.read.PrepareRead");
    }

    everRead = true;

    // read chunk offset and length of each target column chunks
    PROFILE_START("PixelsRecordReaderImpl.read.PrepareChunks");
    chunkBuffers.clear();
    chunkBuffers.resize(includedColumns.size());
    std::vector <ChunkId> diskChunks;
    diskChunks.reserve(targetColumns.size());

    const pixels::fb::RowGroupIndex* rowGroupIndex =
            rowGroupFooters[curRGIdx]->rowGroupIndexEntry();
    for (int colId: targetColumns)
    {
        const pixels::fb::ColumnChunkIndex* chunkIndex =
                rowGroupIndex->columnChunkIndexEntries()->Get(colId);
        if (!chunkIndex->littleEndian())
        {
            throw InvalidArgumentException("Pixels C++ reader only supports little endianness. ");
        }
        ChunkId chunk(curRGIdx, colId, chunkIndex->chunkOffset(), chunkIndex->chunkLength());
        diskChunks.emplace_back(chunk);
    }
    PROFILE_END("PixelsRecordReaderImpl.read.PrepareChunks");

    if (!diskChunks.empty())
    {
        RequestBatch requestBatch((int) diskChunks.size());
        Scheduler *scheduler = SchedulerFactory::Instance()->getScheduler();
        std::vector <uint32_t> colIds;
        std::vector <uint64_t> bytes;
        for (int i = 0; i < diskChunks.size(); i++)
        {
            ChunkId chunk = diskChunks.at(i);
            colIds.emplace_back(chunk.columnId);
            bytes.emplace_back(chunk.length);
        }
        
        // Check buffer pool mode
        bool useStaticBufferPool = false;
        bool useDynamicBuffer    = false;
        std::string asyncLib;
        try { asyncLib = ConfigFactory::Instance().getProperty("localfs.async.lib"); } catch (...) {}

        try {
            useStaticBufferPool = ConfigFactory::Instance().boolCheckProperty("pixel.enable.globalStaticBytebuffer");
        } catch (...) {
            useStaticBufferPool = false;
        }

        try {
            useDynamicBuffer = ConfigFactory::Instance().boolCheckProperty("pixels.enable.dynamic.buffer");
        } catch (...) {
            useDynamicBuffer = false;
        }

        std::vector <std::shared_ptr<ByteBuffer>> originalByteBuffers;

        PROFILE_START("PixelsRecordReaderImpl.read.AllocateBuffers");

#ifdef PIXELS_ENABLE_SPDK
        if (asyncLib == "spdk") {
            // ========== SPDK DMA Buffer Pool ==========
            // SpdkBufferPool holds spdk_dma_malloc'd buffers; readAsync uses
            // them directly as the NVMe DMA target (zero-copy, no memcpy).
            SpdkBufferPool::Initialize(colIds, bytes, fileSchema->getFieldNames());
            if (pixelsBufferIdx == -1)
                pixelsBufferIdx = SpdkBufferPool::GetCurrentBufferIdx();

            for (int i = 0; i < (int)colIds.size(); i++) {
                auto colId = colIds.at(i);
                originalByteBuffers.emplace_back(
                    SpdkBufferPool::GetBufferAt(colId, pixelsBufferIdx));
                requestBatch.add(queryId,
                                 diskChunks.at(i).offset,
                                 (int)diskChunks.at(i).length,
                                 static_cast<int64_t>(i));
            }
            // ==========================================
        } else
#endif // PIXELS_ENABLE_SPDK
        if (useStaticBufferPool && pixels::ThreadContext::HasContext()) {
            // ========== Global Static BufferPool Implementation ==========
            // Buffers and ring are already pre-allocated and pre-registered
            // Just get buffer from GlobalStaticBufferPool
            
            int threadId = pixels::ThreadContext::GetThreadId();
            auto& pool = GlobalStaticBufferPool::Instance();
            
            for (int i = 0; i < colIds.size(); i++)
            {
                auto colId = colIds.at(i);
                auto offset = diskChunks.at(i).offset;
                auto length = diskChunks.at(i).length;
                
                // Get column name from field names
                std::string columnName = fileSchema->getFieldNames()[colId];
                
                // For double buffering, we use bufferIdx 0 or 1 alternately
                // TODO: Implement proper double buffering logic if needed
                int bufferIdx = ::BufferPool::GetNextBufferIdx(); // Simple implementation: always use buffer 0
                if (ShouldPrintBufferIdxDebug(threadId))
                {
                    std::cout << "[BufferIdxDebug] reader_read"
                              << " file=" << fileName
                              << " thread=" << threadId
                              << " column=" << columnName
                              << " bufferIdx=" << bufferIdx
                              << " currBufferIdx=" << ::BufferPool::GetCurrentBufferIdx()
                              << " nextBufferIdx=" << ::BufferPool::GetNextBufferIdx()
                              << " offset=" << offset
                              << " length=" << length
                              << std::endl;
                }
                
                // Get pre-allocated buffer from global pool
                auto buffer = pool.GetBuffer(columnName, threadId, bufferIdx);
                originalByteBuffers.emplace_back(buffer);
                
                // Get buffer index in the registered array (for io_uring_prep_read_fixed)
                int registeredBufferIndex = pool.GetBufferIndex(columnName, threadId, bufferIdx);
                
                requestBatch.add(queryId, offset, (int) length,columnName, registeredBufferIndex);
            }
            // ==========================================================
        } else if (useDynamicBuffer) {
            // ========== Dynamic BufferPool Implementation ==========
            // Initialize DirectUringRandomAccessFileDynamic with io_uring and buffer pool
            DirectUringRandomAccessFileDynamic::Initialize(4096, 1024);
            
            // Get DirectIoLib for alignment calculation
            auto directIoLib = DynamicBufferPool::GetDirectIoLib();
            
            // Allocate buffers for each column dynamically
            for (int i = 0; i < colIds.size(); i++)
            {
                auto colId = colIds.at(i);
                auto offset = diskChunks.at(i).offset;
                auto length = diskChunks.at(i).length;
                
                // Calculate aligned buffer size for Direct I/O
                uint64_t bufferSize;
                if (ConfigFactory::Instance().boolCheckProperty("localfs.enable.direct.io") && directIoLib != nullptr) {
                    // For Direct I/O, we need to align to block boundaries
                    uint64_t fileOffsetAligned = directIoLib->blockStart(offset);
                    bufferSize = directIoLib->blockEnd(offset + length) - fileOffsetAligned;
                } else {
                    bufferSize = length;
                }

                // if we specify a fixed-size buffer
                if (ConfigFactory::Instance().boolCheckProperty("pixel.bufferpool.fixedSize"))
                {
                    bufferSize = std::stoi(ConfigFactory::Instance().getProperty("pixel.bufferpool.bufferpoolSize"));
                }
                // Allocate buffer for this column if not already allocated

                auto buffer = DynamicBufferPool::GetBuffer(colId);
                if (buffer == nullptr) {
                    buffer = DynamicBufferPool::AllocateBuffer(colId, bufferSize);
                } else if (buffer->size() < bufferSize) {
                    // Grow buffer if needed
                    buffer = DynamicBufferPool::GrowBuffer(colId, bufferSize);
                    if (ConfigFactory::Instance().boolCheckProperty("pixel.bufferpool.fixedSize"))
                    {
                        throw InvalidArgumentException("We can not grow buffer when the bufferSize is fixed.");
                    }
                }

                
                originalByteBuffers.emplace_back(buffer);
                
                // Get buffer slot index for io_uring fixed buffer read
                int slotIndex = DynamicBufferPool::GetBufferSlotIndex(colId);
                requestBatch.add(queryId, offset, (int) length, slotIndex);
            }
            // ==========================================================
        } else {
            // ========== Static BufferPool Implementation ==========
            // Check if we should use fixed buffers
            bool useFixedBuffer = true;
            try {
                useFixedBuffer = ConfigFactory::Instance().boolCheckProperty("localfs.iouring.use.fixed.buffer");
            } catch (...) {
                useFixedBuffer = true; // default to fixed buffer for backward compatibility
            }

            // Pin the buffer set index for this file's lifetime.  BufferPool::Switch() may be
            // called between row groups (double-buffer file-level prefetch), but all row groups
            // of the same file must read into / from the same buffer set.
            if (pixelsBufferIdx == -1)
            {
                pixelsBufferIdx = ::BufferPool::GetCurrentBufferIdx();
            }

            if (useFixedBuffer) {
                // Use fixed buffer version - requires buffer registration
                ::DirectUringRandomAccessFile::Initialize();
                ::BufferPool::Initialize(colIds, bytes, fileSchema->getFieldNames());
                if (!ConfigFactory::Instance().boolCheckProperty("pixel.enable.globalStaticBytebuffer"))
                {
                    ::DirectUringRandomAccessFile::RegisterBufferFromPool(colIds);
                }

                for (int i = 0; i < colIds.size(); i++)
                {
                    auto colId = colIds.at(i);
                    originalByteBuffers.emplace_back(::BufferPool::GetBufferAt(colId, pixelsBufferIdx));
                    requestBatch.add(queryId, diskChunks.at(i).offset, (int) diskChunks.at(i).length,
                                     ::BufferPool::GetBufferIdAt(i, pixelsBufferIdx));
                }
            } else {
                // Use non-fixed buffer version - no buffer registration needed
                ::DirectUringRandomAccessFileNonFixed::Initialize();
                ::BufferPool::Initialize(colIds, bytes, fileSchema->getFieldNames());
                // No need to register buffers for non-fixed version

                for (int i = 0; i < colIds.size(); i++)
                {
                    auto colId = colIds.at(i);
                    originalByteBuffers.emplace_back(::BufferPool::GetBufferAt(colId, pixelsBufferIdx));
                    // For non-fixed buffers, the index parameter is ignored but we still pass it for API compatibility
                    requestBatch.add(queryId, diskChunks.at(i).offset, (int) diskChunks.at(i).length,
                                     ::BufferPool::GetBufferIdAt(i, pixelsBufferIdx));
                }
            }
            // ==========================================================
        }
        PROFILE_END("PixelsRecordReaderImpl.read.AllocateBuffers");

        PROFILE_START("PixelsRecordReaderImpl.read.ExecuteIO");
        auto byteBuffers = scheduler->executeBatch(
            physicalReader, requestBatch, originalByteBuffers, queryId);
        PROFILE_END("PixelsRecordReaderImpl.read.ExecuteIO");

        if(ConfigFactory::Instance().boolCheckProperty("localfs.enable.async.io")
            && originalByteBuffers.size() > 0)
        {
            asyncReadRequestNum += diskChunks.size();
        }

        PROFILE_START("PixelsRecordReaderImpl.read.AssignBuffers");
        for (int index = 0; index < diskChunks.size(); index++)
        {
            ChunkId chunk = diskChunks.at(index);
            std::shared_ptr <ByteBuffer> bb = byteBuffers.at(index);
            uint32_t colId = chunk.columnId;
            if (bb != nullptr)
            {
                chunkBuffers.at(colId) = bb;
            }
        }
        PROFILE_END("PixelsRecordReaderImpl.read.AssignBuffers");
    }
    
    PROFILE_END("PixelsRecordReaderImpl.read.Total");
    return true;

}

PixelsRecordReaderImpl::~PixelsRecordReaderImpl()
{
    // TODO: chunkBuffers, physicalReader should be deleted?
}

std::shared_ptr <TypeDescription> PixelsRecordReaderImpl::getResultSchema()
{
    return resultSchema;
}

/**
     * Create a row batch without any data, only sets the number of rows (size) and OEF.
     * Such a row batch is used for queries such as select count(*).
     * @param size the number of rows in the row batch.
     * @return the empty row batch.
 */
std::shared_ptr <VectorizedRowBatch> PixelsRecordReaderImpl::createEmptyEOFRowBatch(int size)
{
    auto emptySchema = TypeDescription::createSchema(
            std::span<const pixels::fb::Type*>());
    auto emptyRowBatch = emptySchema->createRowBatch(0);
    emptyRowBatch->rowCount = 0;
    return emptyRowBatch;
}

bool PixelsRecordReaderImpl::isEndOfFile()
{
    return endOfFile;
}

void PixelsRecordReaderImpl::close()
{
    // release chunk buffers
    chunkBuffers.clear();
    for (const auto &reader: readers)
    {
        reader->close();
    }
    if (resultRowBatch != nullptr)
    {
        resultRowBatch->close();
    }
    readers.clear();
    rowGroupFooters.clear();
    rowGroupFooterBuffers.clear();
    includedColumnTypes.clear();
    endOfFile = true;
}
