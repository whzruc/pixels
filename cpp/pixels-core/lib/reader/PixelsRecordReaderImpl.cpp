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
#include "physical/BufferPoolMode.h"
#include "physical/BufferPoolStats.h"
#include "physical/DynamicBufferPool.h"
#include "physical/GlobalStaticBufferPool.h"
#include "physical/natives/DirectUringRandomAccessFileDynamic.h"
std::mutex PixelsRecordReaderImpl::mutex_;
PixelsRecordReaderImpl::PixelsRecordReaderImpl(std::shared_ptr <PhysicalReader> reader,
                                               const pixels::fb::PostScript *pixelsPostScript,
                                               const pixels::fb::Footer *pixelsFooter,
                                               PixelsReaderOption &opt,
                                               std::shared_ptr <PixelsFooterCache> pixelsFooterCache)
{
    physicalReader = reader;
    footer = pixelsFooter;
    postScript = pixelsPostScript;
    footerCache = pixelsFooterCache;
    option = std::move(opt);
    // TODO: intialize all kinds of variable
    queryId = option.getQueryId();
    RGStart = option.getRGStart();
    RGLen = option.getRGLen();
    batchSize = option.getBatchSize();
    // batchSize must be larger than STANDARD_VECTOR_SIZE
    // for test purpose, can we comment it temporarily
    // assert(batchSize >= STANDARD_VECTOR_SIZE);
    enabledFilterPushDown = option.isEnabledFilterPushDown();
    if (enabledFilterPushDown) {
        this->filter = option.extractFilter();
    } else {
        this->filter.filters.clear();
    }
    filterMask = nullptr;
    everRead = false;
    everPrepareRead = false;
    targetRGNum = 0;
    curRGIdx = 0;
    curRowInRG = 0;
    curRGRowCount = 0;
    fileName = physicalReader->getName();
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


    // TODO: resultRowBatch.projectionSize


    // update current batch size
    PROFILE_START("PixelsRecordReaderImpl.readBatch.PrepareBatch");
    int curBatchSize = std::min(curRGRowCount - curRowInRG, std::min(batchSize, curRGRowCount));
    if (resultRowBatch == nullptr)
    {
        resultRowBatch = resultSchema->createRowBatch(curBatchSize, resultColumnsEncoded);
    }
    else
    {
        resultRowBatch->reset();
        if (curBatchSize != resultRowBatch->maxSize)
        {
            resultRowBatch->resize(curBatchSize);
        }
    }

    auto columnVectors = resultRowBatch->cols;
    if (filterMask != nullptr) {
        filterMask->set();
    }
    PROFILE_END("PixelsRecordReaderImpl.readBatch.PrepareBatch");

    if(asyncReadRequestNum > 0) {
        PROFILE_START("PixelsRecordReaderImpl.readBatch.AsyncReadComplete");
        asyncReadComplete(asyncReadRequestNum);
        PROFILE_END("PixelsRecordReaderImpl.readBatch.AsyncReadComplete");
    }

    std::vector<int> filterColumnIndex;
    if (!filter.filters.empty())
    {
        PROFILE_START("PixelsRecordReaderImpl.readBatch.ReadFilterColumns");
        for (auto const& [col_idx, filter_ptr] : filter.filters)
        {
            if (filterMask->isNone())
            {
                break;
            }
            int index = curChunkBufferIndex.at(col_idx);
            auto &encoding = curEncoding.at(col_idx);
            auto &chunkIndex = curChunkIndex.at(col_idx);
            readers.at(col_idx)->read(chunkBuffers.at(index), encoding, curRowInRG, curBatchSize,
                                postScript->pixelStride(), resultRowBatch->rowCount,
                                columnVectors.at(col_idx), chunkIndex, filterMask);
            filterColumnIndex.emplace_back(index);

            PROFILE_START("PixelsRecordReaderImpl.readBatch.ApplyFilterExpr");
            PixelsFilter::ApplyFilter(
                columnVectors.at(col_idx),
                *filter_ptr,
                *filterMask,
                resultSchema->getChildren().at(col_idx)
            );
            PROFILE_END("PixelsRecordReaderImpl.readBatch.ApplyFilterExpr");
        }
        PROFILE_END("PixelsRecordReaderImpl.readBatch.ReadFilterColumns");
    }


    // read vectors
    PROFILE_START("PixelsRecordReaderImpl.readBatch.ReadDataColumns");
    for (int i = 0; i < resultColumns.size(); i++)
    {
        // TODO: Refer to Issue #564. Disable data skipping
        //if(filterMask != nullptr) {
        //    if(filterMask->isNone()) {
        //        break;
        //    }
        //}
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
    if (curRowInRG >= curRGRowCount)
    {
        curRGIdx++;
        if (curRGIdx < targetRGNum)
        {
            UpdateRowGroupInfo();
        }
        else
        {
            // if end of file, set result vectorized row batch endOfFile
            // TODO: set checkValid to false!
            endOfFile = true;
        }
        curRowInRG = 0;
    }
    PROFILE_END("PixelsRecordReaderImpl.readBatch.FinalizeBatch");
    PROFILE_END("PixelsRecordReaderImpl.readBatch.Total");
    return resultRowBatch;
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
            requestBatch.add(queryId, (int) footerOffset, (int) footerLength,ringIndex);
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
            if (footerCache != nullptr)
            {
                footerCache->putRGFooter(rgCacheIds[fis[i]], parsed);
            }
        }
    }

    bbs.clear();
    resultColumnsEncoded.clear();
    resultColumnsEncoded.resize(includedColumnNum);

    curEncoding.resize(resultColumns.size());
    curChunkBufferIndex.resize(resultColumns.size());
    curChunkIndex.resize(resultColumns.size());
    UpdateRowGroupInfo();
}

void PixelsRecordReaderImpl::asyncReadComplete(int requestSize)
{
    if (ConfigFactory::Instance().boolCheckProperty("localfs.enable.async.io")
        && asyncReadRequestNum >= requestSize)
    {
        if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "iouring")
        {
            auto localReader = std::static_pointer_cast<PhysicalLocalReader>(physicalReader);
            auto ringIndexCountMap=localReader->getRingIndexCountMap();
            localReader->readAsyncComplete(ringIndexCountMap,localReader->getRingIndexes());
            asyncReadRequestNum -= requestSize;
        }
        else if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "aio")
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

    // TODO: this should remove later
    chunkBuffers.clear();
    chunkBuffers.resize(includedColumns.size());
    std::vector <ChunkId> diskChunks;
    diskChunks.reserve(targetColumns.size());


    // TODO: support cache read

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


    if (!diskChunks.empty())
    {
        PROFILE_START("PixelsRecordReaderImpl.read.PrepareChunks");
        // std::lock_guard<std::mutex> lock(mutex_);
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
        auto columnNames = fileSchema->getFieldNames();
        std::vector <std::shared_ptr<ByteBuffer>> originalByteBuffers;
        std::vector<int> ring_col;
        if (GetBufferPoolMode() == BufferPoolMode::Dynamic)
        {
            ::DirectUringRandomAccessFileDynamic::Initialize();
            auto directIoLib = ::DynamicBufferPool::GetDirectIoLib();
            for (int i = 0; i < colIds.size(); i++)
            {
                auto colId = colIds.at(i);
                auto bufferKey = colId * 2 + static_cast<uint32_t>(::BufferPool::GetBufferId());
                auto chunk = diskChunks.at(i);
                uint64_t bufferSize = chunk.length;
                if (ConfigFactory::Instance().boolCheckProperty("localfs.enable.direct.io"))
                {
                    auto alignedOffset = directIoLib->blockStart(chunk.offset);
                    bufferSize = directIoLib->blockEnd(chunk.offset + chunk.length) - alignedOffset;
                }
                auto buffer = ::DynamicBufferPool::GetBuffer(bufferKey);
                if (buffer == nullptr)
                {
                    buffer = ::DynamicBufferPool::AllocateBuffer(bufferKey, bufferSize);
                }
                else if (buffer->size() < bufferSize)
                {
                    buffer = ::DynamicBufferPool::GrowBuffer(bufferKey, bufferSize);
                }
                else
                {
                    ::BufferPoolStats::Instance().RecordReuse(BufferPoolStatsMode::Dynamic);
                }
                originalByteBuffers.emplace_back(buffer);
                requestBatch.add(queryId, chunk.offset, chunk.length,
                                 ::DynamicBufferPool::GetBufferSlotIndex(bufferKey));
            }
        }
        else if (GetBufferPoolMode() == BufferPoolMode::Static)
        {
            static thread_local int threadId = -1;
            auto &pool = ::GlobalStaticBufferPool::Instance();
            if (threadId < 0)
            {
                threadId = pool.AcquireThreadId();
            }
            for (int i = 0; i < colIds.size(); i++)
            {
                auto chunk = diskChunks.at(i);
                auto columnName = columnNames.at(chunk.columnId);
                int bufferId = ::BufferPool::GetBufferId();
                auto buffer = pool.GetBuffer(columnName, threadId, bufferId);
                uint64_t requiredSize = chunk.length;
                if (ConfigFactory::Instance().boolCheckProperty("localfs.enable.direct.io"))
                {
                    auto directIoLib = std::make_shared<DirectIoLib>(
                        std::stoi(ConfigFactory::Instance().getProperty("localfs.block.size", "4096")));
                    requiredSize = directIoLib->blockEnd(chunk.offset + chunk.length) -
                                   directIoLib->blockStart(chunk.offset);
                }
                if (buffer->size() < requiredSize)
                {
                    throw InvalidArgumentException(
                        "PixelsRecordReaderImpl::read: static buffer is smaller than the column chunk");
                }
                originalByteBuffers.emplace_back(buffer);
                requestBatch.add(queryId, chunk.offset, chunk.length,
                                 pool.GetBufferIndex(columnName, bufferId));
            }
        }
        else
        {
            ::BufferPool::Initialize(colIds, bytes, columnNames);
            ::DirectUringRandomAccessFile::RegisterBufferFromPool(colIds);
            for (int i = 0; i < colIds.size(); i++)
            {
                auto colId = colIds.at(i);
                auto byte = bytes.at(i);
                auto currentBufferEntry = ::BufferPool::GetBuffer(colId, byte, columnNames[colId]);
                originalByteBuffers.emplace_back(currentBufferEntry);
                requestBatch.add(queryId, diskChunks.at(i).offset, diskChunks.at(i).length,
                                 ::BufferPool::GetBufferId());
                requestBatch.getRequest(i).ringIndex = ::BufferPool::getRingIndex(colId);
                if (currentBufferEntry->size() - requestBatch.getRequest(i).length <= 4096)
                {
                    throw InvalidArgumentException(
                        "PixelsRecordReaderImpl::read: insufficient buffer capacity");
                }
                if (requestBatch.getRequest(i).ringIndex != 0)
                {
                    requestBatch.getRequest(i).bufferId = 0;
                    ring_col.emplace_back(i);
                }
            }
        }
        PROFILE_END("PixelsRecordReaderImpl.read.PrepareChunks");

        // ::BufferPool::PrintStats();

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
    includedColumnTypes.clear();
    endOfFile = true;
}
