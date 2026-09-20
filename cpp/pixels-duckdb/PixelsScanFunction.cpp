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
 * @create 2023-03-26
 */
#include "PixelsScanFunction.hpp"

#include <physical/DynamicBufferPool.h>
#ifdef PIXELS_ENABLE_SPDK
#include <physical/SpdkBufferPool.h>
#include <physical/natives/DirectSpdkRandomAccessFile.h>
#endif
#include <physical/natives/DirectUringRandomAccessFile.h>
#include <physical/natives/DirectUringRandomAccessFileNonFixed.h>
#include <physical/natives/DirectUringRandomAccessFileDynamic.h>
#include <physical/ThreadContext.h>

#include "physical/StorageArrayScheduler.h"
#include "profiler/CountProfiler.h"
#include "profiler/TimeProfiler.h"
#include "CPUAffinity.h"
#include <iostream>

namespace duckdb
{
    static bool BufferIdxDebugEnabled()
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

    static int BufferIdxDebugThread()
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

    static bool ShouldPrintBufferIdxDebug(int threadId)
    {
        if (!BufferIdxDebugEnabled())
        {
            return false;
        }
        int debugThread = BufferIdxDebugThread();
        return debugThread < 0 || debugThread == threadId;
    }

    bool PixelsScanFunction::enable_filter_pushdown = true;

    static idx_t PixelsScanGetBatchIndex(ClientContext& context, const FunctionData* bind_data_p,
                                         LocalTableFunctionState* local_state,
                                         GlobalTableFunctionState* global_state)
    {
        auto& data = (PixelsReadLocalState&)*local_state;
        return data.curr_batch_index;
    }

    static double PixelsProgress(ClientContext& context, const FunctionData* bind_data_p,
                                 const GlobalTableFunctionState* global_state)
    {
        auto& bind_data = (PixelsReadBindData&)*bind_data_p;
        if (bind_data.files.empty())
        {
            return 100.0;
        }
        auto percentage = bind_data.curFileId * 100.0 / bind_data.files.size();
        return percentage;
    }

    static unique_ptr<NodeStatistics> PixelsCardinality(ClientContext& context, const FunctionData* bind_data)
    {
        auto& data = (PixelsReadBindData&)*bind_data;

        return make_uniq<NodeStatistics>(data.initialPixelsReader->getNumberOfRows() * data.files.size());
    }

    TableFunctionSet PixelsScanFunction::GetFunctionSet()
    {
        TableFunction table_function("pixels_scan", {LogicalType::VARCHAR}, PixelsScanImplementation, PixelsScanBind,
                                     PixelsScanInitGlobal, PixelsScanInitLocal);
        table_function.projection_pushdown = true;
        table_function.filter_pushdown = true;
        // table_function.filter_prune = true;
        enable_filter_pushdown = table_function.filter_pushdown;
        MultiFileReader::AddParameters(table_function);
        table_function.cardinality = PixelsCardinality;
        table_function.table_scan_progress = PixelsProgress;
        // TODO: maybe we need other code here later. Refer parquet-extension.cpp
        return MultiFileReader::CreateFunctionSet(table_function);
    }

    void PixelsScanFunction::PixelsScanImplementation(ClientContext& context,
                                                      TableFunctionInput& data_p,
                                                      DataChunk& output)
    {
        if (!data_p.local_state)
        {
            return;
        }

        auto& data = (PixelsReadLocalState&)*data_p.local_state;
        auto& gstate = (PixelsReadGlobalState&)*data_p.global_state;
        auto& bind_data = (PixelsReadBindData&)*data_p.bind_data;
        PROFILE_START("PixelsScanFunction.PixelsScanImplementation.Total");

        do
        {
            PROFILE_START("PixelsScanFunction.Stage.HandleFileBoundary");
            if (data.currPixelsRecordReader == nullptr ||
                (data.currPixelsRecordReader->isEndOfFile() && data.vectorizedRowBatch->isEndOfFile()))
            {
                if (data.currPixelsRecordReader != nullptr)
                {
                    data.currPixelsRecordReader.reset();
                }
                if (!PixelsParallelStateNext(context, bind_data, data, gstate))
                {
                    PROFILE_END("PixelsScanFunction.Stage.HandleFileBoundary");
                    PROFILE_END("PixelsScanFunction.PixelsScanImplementation.Total");
                    ::TimeProfiler::Instance().Collect();
                    if (data.shouldPrintProfileSummary)
                    {
#ifdef PIXELS_ENABLE_SPDK
                        if (data.cfgSpdk)
                        {
                            SpdkGlobal::PrintIoStats();
                        }
#endif
                        ::TimeProfiler::Instance().PrintSummary(
                            "PixelsScanFunction.PixelsScanImplementation.Total",
                            {
                                "PixelsScanFunction.PixelsScanImplementation.Total",
                                "PixelsScanFunction.Stage.HandleFileBoundary",
                                "PixelsScanFunction.Stage.AcquireBatch",
                                "PixelsScanFunction.Stage.TransformOutput",
                                "PixelsScanFunction.Stage.ApplyFilter",
                                "PixelsScanFunction.Stage.AdvanceState"
                            },
                            "Closed Pipeline Profile Summary");
                        ::TimeProfiler::Instance().PrintSummary(
                            "PixelsRecordReaderImpl.readBatch.Total",
                            {
                                "PixelsRecordReaderImpl.readBatch.Total",
                                "PixelsRecordReaderImpl.readBatch.FirstRead",
                                "PixelsRecordReaderImpl.readBatch.ComputeBatchSize",
                                "PixelsRecordReaderImpl.readBatch.CreateRowBatch",
                                "PixelsRecordReaderImpl.readBatch.ResetRowBatch",
                                "PixelsRecordReaderImpl.readBatch.ResizeRowBatch",
                                "PixelsRecordReaderImpl.readBatch.ResetFilterMask",
                                "PixelsRecordReaderImpl.readBatch.ReadFilterColumns",
                                "PixelsRecordReaderImpl.readBatch.ApplyFilterExpr",
                                "PixelsRecordReaderImpl.readBatch.ReadDataColumns",
                                "PixelsRecordReaderImpl.readBatch.ReadDataColumns.WaitBuffer",
                                "PixelsRecordReaderImpl.readBatch.ReadDataColumns.Decode",
                                "PixelsRecordReaderImpl.readBatch.ReadDataColumns.Copy",
                                "PixelsRecordReaderImpl.readBatch.FinalizeBatch"
                            },
                            "ReadBatch Profile Summary");
                        ::TimeProfiler::Instance().PrintSummary(
                            "PixelsRecordReaderImpl.read.Total",
                            {
                                "PixelsRecordReaderImpl.read.Total",
                                "PixelsRecordReaderImpl.read.PrepareRead",
                                "PixelsRecordReaderImpl.read.PrepareChunks",
                                "PixelsRecordReaderImpl.read.AllocateBuffers",
                                "PixelsRecordReaderImpl.read.ExecuteIO",
                                "PixelsRecordReaderImpl.read.AssignBuffers"
                            },
                            "Read Submit Profile Summary");
                        ::TimeProfiler::Instance().PrintSummary(
                            "PixelsScanFunction.PixelsParallelStateNext.Total",
                            {
                                "PixelsScanFunction.PixelsParallelStateNext.Total",
                                "PixelsScanFunction.StateNext.LockWait",
                                "PixelsScanFunction.StateNext.CloseReader",
                                "PixelsScanFunction.StateNext.SwitchBuffer",
                                "PixelsScanFunction.StateNext.BuildPixelsReader",
                                "PixelsScanFunction.StateNext.CreateRecordReader",
                                "PixelsScanFunction.curr_reader.read",
                                "PixelsScanFunction.curr_reader.complete",
                                "PixelsScanFunction.next_reader.read"
                            },
                            "State Transition Profile Summary");
                        // Metadata is read synchronously for every local-file backend.
                        // Keep this summary backend-neutral so io_uring, pread, and
                        // SPDK runs use the same attribution labels and time base.
                        ::TimeProfiler::Instance().PrintSummary(
                            "PixelsScanFunction.PixelsParallelStateNext.Total",
                            {
                                "Pixels.Metadata.FileTailOffsetRead",
                                "Pixels.Metadata.FileTailRead",
                                "Pixels.Metadata.RowGroupFooterRead"
                            },
                            "Metadata I/O Profile Summary");
                        if (data.cfgSpdk)
                        {
                            ::TimeProfiler::Instance().PrintSummary(
                                "PixelsRecordReaderImpl.read.Total",
                                {
                                    "Spdk.Initialize.Total",
                                    "Spdk.Initialize.Environment",
                                    "Spdk.Initialize.Probe",
                                    "Spdk.Initialize.LoadLbaMap",
                                    "Spdk.BufferPool.Initialize.Total",
                                    "Spdk.BufferPool.Initialize.Allocate",
                                    "Spdk.BufferPool.Initialize.Grow",
                                    "Spdk.SyncRead.Total",
                                    "Spdk.SyncRead.DmaAllocate",
                                    "Spdk.SyncRead.Submit",
                                    "Spdk.SyncRead.Poll",
                                    "Spdk.SyncRead.Copy",
                                    "Spdk.SyncRead.DmaFree",
                                    "Spdk.AsyncRead.Submit",
                                    "Spdk.AsyncComplete.Total",
                                    "Spdk.AsyncComplete.Poll",
                                    "Spdk.AsyncComplete.ReleaseOps"
                                },
                                "SPDK I/O Profile Summary");
                        }
                        if (ConfigFactory::Instance().boolCheckProperty("localfs.enable.async.io") &&
                            ConfigFactory::Instance().getProperty("localfs.async.lib") == "iouring")
                        {
                            ::TimeProfiler::Instance().PrintSummary(
                                "PixelsRecordReaderImpl.read.Total",
                                {
                                    "Uring.AsyncSubmit.Total",
                                    "Uring.AsyncComplete.Total",
                                    "Uring.AsyncComplete.WaitCQE",
                                    "Uring.AsyncComplete.ProcessCQE"
                                },
                                "io_uring I/O Profile Summary");
                        }
                        if (!ConfigFactory::Instance().boolCheckProperty("localfs.enable.async.io"))
                        {
                            ::TimeProfiler::Instance().PrintSummary(
                                "PixelsRecordReaderImpl.read.Total",
                                {
                                    "Pixels.Pread.ReadFully.Total",
                                    "Pixels.Pread.ReadFully.Allocate",
                                    "Pixels.Pread.ReadFully.DirectIO",
                                    "Pixels.Pread.ReadFully.Syscall",
                                    "Pixels.Pread.ReadFullyReuse.Total",
                                    "Pixels.Pread.ReadFullyReuse.DirectIO",
                                    "Pixels.Pread.ReadFullyReuse.Syscall",
                                    "Pixels.Pread.ReadFullyReuse.WrapBuffer",
                                    "Pixels.Pread.Metadata.Total",
                                    "Pixels.Pread.Metadata.DirectIO",
                                    "Pixels.Pread.Metadata.Syscall"
                                },
                                "Pixels Pread I/O Profile Summary");
                        }
                    }
                    return;
                }
            }
            PROFILE_END("PixelsScanFunction.Stage.HandleFileBoundary");
            auto currPixelsRecordReader = std::static_pointer_cast<PixelsRecordReaderImpl>(data.currPixelsRecordReader);

            PROFILE_START("PixelsScanFunction.Stage.AcquireBatch");
            if (data.vectorizedRowBatch != nullptr && data.vectorizedRowBatch->isEndOfFile())
            {
                data.vectorizedRowBatch = nullptr;
            }
            if (data.vectorizedRowBatch == nullptr)
            {
                data.vectorizedRowBatch = currPixelsRecordReader->readBatch(false);
            }
            uint64_t currentLoc = data.vectorizedRowBatch->position();

            std::shared_ptr<TypeDescription> resultSchema = data.currPixelsRecordReader->getResultSchema();
            uint64_t remaining = data.vectorizedRowBatch->remaining();
            // std::cout<<"PixelsScan: curpixels:"<<currPixelsRecordReader->getFileName()<<std::endl;
            // std::cout<<"PixelsScanImplemenetation: currentLoc: "<<currentLoc<<" remaining: "<<remaining<<std::endl;
            assert(remaining > 0);
            auto thisOutputChunkRows = MinValue<idx_t>(STANDARD_VECTOR_SIZE, remaining);
            output.SetCardinality(thisOutputChunkRows);
            std::shared_ptr<PixelsBitMask> filterMask =
                std::static_pointer_cast<PixelsRecordReaderImpl>(data.currPixelsRecordReader)->getFilterMask();
            PROFILE_END("PixelsScanFunction.Stage.AcquireBatch");

            PROFILE_START("PixelsScanFunction.Stage.TransformOutput");
            TransformDuckdbChunk(data, output, resultSchema, thisOutputChunkRows);
            PROFILE_END("PixelsScanFunction.Stage.TransformOutput");

            // apply the filter operation
            PROFILE_START("PixelsScanFunction.Stage.ApplyFilter");
            if (enable_filter_pushdown)
            {
                idx_t sel_size = 0;
                SelectionVector sel;
                sel.Initialize(thisOutputChunkRows);
                for (idx_t i = 0; i < thisOutputChunkRows; i++)
                {
                    if (filterMask->get(i + currentLoc))
                    {
                        sel.set_index(sel_size++, i);
                    }
                }
                output.Slice(sel, sel_size);
            }
            PROFILE_END("PixelsScanFunction.Stage.ApplyFilter");
            //if remaining< STANDARD_VECTOR_SIZE updtaepdate rowGroup
            PROFILE_START("PixelsScanFunction.Stage.AdvanceState");
            if (remaining<STANDARD_VECTOR_SIZE)
            {
                currPixelsRecordReader->nextRowGroup();
            }
            if (output.size() > 0)
            {
                PROFILE_END("PixelsScanFunction.Stage.AdvanceState");
                PROFILE_END("PixelsScanFunction.PixelsScanImplementation.Total");
                return;
            }
            else
            {
                output.Reset();
            }
            PROFILE_END("PixelsScanFunction.Stage.AdvanceState");
        }
        while (true);
    }

    struct compare_file_name
    {
        inline bool operator()(const string& path1, const string& path2)
        {
            int num1 = filename2num(path1);
            int num2 = filename2num(path2);
            return num1 < num2;
        }

        // the pixels file name format is xxxxx_${number}.pxl. We transfer this name to ${number}
        static int filename2num(const string& filename)
        {
            string filename_without_suffix = filename.substr(0, filename.rfind('.'));
            int number = std::stoi(filename_without_suffix.substr(filename_without_suffix.rfind('_') + 1));
            return number;
        }
    };

    unique_ptr<FunctionData> PixelsScanFunction::PixelsScanBind(
        ClientContext& context, TableFunctionBindInput& input,
        vector<LogicalType>& return_types, vector<string>& names)
    {
        if (input.inputs[0].IsNull())
        {
            throw ParserException("Pixels reader cannot take NULL list as parameter");
        }
        auto multi_file_reader = MultiFileReader::CreateDefault("PixelsScan");

        auto file_list = multi_file_reader->CreateFileList(context, input.inputs[0],
                                                           duckdb::FileGlobOptions::ALLOW_EMPTY);

        auto files = file_list->GetAllFiles();
        vector<string> filePaths;
        if (files.empty())
        {
            // SPDK takes over disks and unmounts file systems; the kernel VFS can no longer
            // see the files even when explicit paths are given. Fall back to extracting
            // paths directly from the input Value so the bind can proceed and SPDK reads
            // the footer via the LBA map.
            bool spdk_enabled = false;
            try { spdk_enabled = ConfigFactory::Instance().boolCheckProperty("localfs.enable.spdk"); }
            catch (...) {}

            if (spdk_enabled)
            {
                if (input.inputs[0].type().id() == LogicalTypeId::LIST)
                {
                    for (auto& v : ListValue::GetChildren(input.inputs[0]))
                    {
                        if (!v.IsNull()) filePaths.push_back(v.GetValue<string>());
                    }
                }
                else
                {
                    filePaths.push_back(input.inputs[0].GetValue<string>());
                }
            }
            if (filePaths.empty())
            {
                throw InvalidArgumentException("The number of pxl file should be positive. ");
            }
        }
        else
        {
            for (auto file : files)
            {
                filePaths.push_back(file.path);
            }
        }
        // sort the pxl file by file name, so that all SSD arrays can be fully utilized
        sort(filePaths.begin(), filePaths.end(), compare_file_name());

        auto footerCache = std::make_shared<PixelsFooterCache>();
        auto builder = std::make_shared<PixelsReaderBuilder>();

        std::shared_ptr<::Storage> storage = StorageFactory::getInstance()->getStorage(::Storage::file);
        std::shared_ptr<PixelsReader> pixelsReader = builder
                                                     ->setPath(filePaths.at(0))
                                                     ->setStorage(storage)
                                                     ->setPixelsFooterCache(footerCache)
                                                     ->build();
        std::shared_ptr<TypeDescription> fileSchema = pixelsReader->getFileSchema();
        TransformDuckdbType(fileSchema, return_types);
        names = fileSchema->getFieldNames();

        auto result = make_uniq<PixelsReadBindData>();
        result->initialPixelsReader = pixelsReader;
        result->fileSchema = fileSchema;
        result->files = filePaths;

        return std::move(result);
    }

    unique_ptr<GlobalTableFunctionState> PixelsScanFunction::PixelsScanInitGlobal(
        ClientContext& context, TableFunctionInitInput& input)
    {
        auto& bind_data = (PixelsReadBindData&)*input.bind_data;
        ::TimeProfiler::Instance().Reset();
#ifdef PIXELS_ENABLE_SPDK
        SpdkGlobal::ResetIoStats();
#endif

        auto result = make_uniq<PixelsReadGlobalState>();

        result->initialPixelsReader = bind_data.initialPixelsReader;

        int max_threads = std::stoi(ConfigFactory::Instance().getProperty("pixel.threads"));
        if (max_threads <= 0)
        {
            max_threads = (int)bind_data.files.size();
        }

        result->storageArrayScheduler = std::make_shared<StorageArrayScheduler>(bind_data.files, max_threads);

        result->file_index.resize(result->storageArrayScheduler->getDeviceSum());

        result->max_threads = max_threads;

        result->batch_index = 0;

        result->filters = input.filters.get();

        // Get actual DuckDB thread count from TaskScheduler
        auto &scheduler = TaskScheduler::GetScheduler(context);
        result->active_threads = NumericCast<idx_t>(scheduler.NumberOfThreads());

        result->all_done=false;

        return std::move(result);
    }

    unique_ptr<LocalTableFunctionState> PixelsScanFunction::PixelsScanInitLocal(
        ExecutionContext& context, TableFunctionInitInput& input,
        GlobalTableFunctionState* gstate_p)
    {
        auto& bind_data = (PixelsReadBindData&)*input.bind_data;

        auto& gstate = (PixelsReadGlobalState&)*gstate_p;

        auto result = make_uniq<PixelsReadLocalState>();

        result->deviceID = gstate.storageArrayScheduler->acquireDeviceId();

        result->column_ids = input.column_ids;

        auto fieldNames = bind_data.fileSchema->getFieldNames();

        for (column_t column_id : input.column_ids)
        {
            if (!IsRowIdColumnId(column_id))
            {
                result->column_names.emplace_back(fieldNames.at(column_id));
            }
        }

        // Initialize the appropriate io_uring implementation based on configuration
        bool useDynamicBuffer = false;
        bool useStaticBufferPool = false;
        
        try {
            useDynamicBuffer = ConfigFactory::Instance().boolCheckProperty("pixels.enable.dynamic.buffer");
        } catch (...) {
            useDynamicBuffer = false;
        }
        
        try {
            useStaticBufferPool = ConfigFactory::Instance().boolCheckProperty("pixel.enable.globalStaticBytebuffer");
        } catch (...) {
            useStaticBufferPool = false;
        }
        
        if (useStaticBufferPool) {
            // Global static buffer pool - acquire thread context
            if (!GlobalStaticBufferPool::Instance().IsInitialized()) {
                throw InvalidArgumentException("PixelsScanInitLocal: GlobalStaticBufferPool not initialized");
            }
            
            result->threadId = GlobalStaticBufferPool::Instance().AcquireThreadId();
            result->ring = GlobalStaticBufferPool::Instance().GetRing(result->threadId, 0);
            result->prefetchRing = GlobalStaticBufferPool::Instance().GetRing(result->threadId, 1);
            
            // Set thread-local context so LocalFS can access it
            pixels::ThreadContext::SetThreadId(result->threadId);
            pixels::ThreadContext::SetRings(result->ring, result->prefetchRing);
            
            // std::cout << "PixelsScanInitLocal: Thread acquired threadId=" << result->threadId << std::endl;
        } else if (useDynamicBuffer) {
            // Dynamic buffer pool - uses DirectUringRandomAccessFileDynamic
            ::DirectUringRandomAccessFileDynamic::Initialize(4096, 1024);
        } else {
            // Static buffer pool - check if using fixed or non-fixed buffers
            bool useFixedBuffer = true;
            try {
                useFixedBuffer = ConfigFactory::Instance().boolCheckProperty("localfs.iouring.use.fixed.buffer");
            } catch (...) {
                useFixedBuffer = true; // default to fixed buffer for backward compatibility
            }
            
            if (useFixedBuffer) {
                ::DirectUringRandomAccessFile::Initialize();
            } else {
                ::DirectUringRandomAccessFileNonFixed::Initialize();
            }
        }
        
        // CPU Affinity Binding (线程绑核)
        bool enableCPUAffinity = false;
        try {
            enableCPUAffinity = ConfigFactory::Instance().boolCheckProperty("pixels.enable.cpu.affinity");
        } catch (...) {
            enableCPUAffinity = false;
        }
        
        if (enableCPUAffinity&&useStaticBufferPool) {
            // Get CPU affinity strategy from configuration
            std::string affinityStrategy = "round-robin"; // default strategy
            try {
                affinityStrategy = ConfigFactory::Instance().getProperty("pixels.cpu.affinity.strategy");
            } catch (...) {
                // Use default
            }
            
            int numCores = pixels::CPUAffinity::GetNumCores();
            int coreId = -1;
            
            if (affinityStrategy == "round-robin") {
                // Round-robin: distribute threads across all available cores
                // Use threadId if available (from GlobalStaticBufferPool), otherwise use deviceID
                int threadIndex = useStaticBufferPool ? result->threadId : result->deviceID;
                coreId = threadIndex % numCores;
            } else if (affinityStrategy == "device-based") {
                // Device-based: bind threads to cores based on deviceID
                coreId = result->deviceID % numCores;
            } else if (affinityStrategy == "custom") {
                // Custom: read core mapping from configuration
                try {
                    std::string coreMapping = ConfigFactory::Instance().getProperty("pixels.cpu.affinity.core.mapping");
                    // Parse comma-separated core IDs, e.g., "0,1,2,3"
                    std::vector<int> coreList;
                    size_t pos = 0;
                    while (pos < coreMapping.length()) {
                        size_t nextComma = coreMapping.find(',', pos);
                        if (nextComma == std::string::npos) {
                            nextComma = coreMapping.length();
                        }
                        int core = std::stoi(coreMapping.substr(pos, nextComma - pos));
                        coreList.push_back(core);
                        pos = nextComma + 1;
                    }
                    if (!coreList.empty()) {
                        int threadIndex = useStaticBufferPool ? result->threadId : result->deviceID;
                        coreId = coreList[threadIndex % coreList.size()];
                    }
                } catch (...) {
                    std::cerr << "Failed to parse custom CPU affinity mapping, falling back to round-robin" << std::endl;
                    int threadIndex = useStaticBufferPool ? result->threadId : result->deviceID;
                    coreId = threadIndex % numCores;
                }
            }
            
            if (coreId >= 0 && coreId < numCores) {
                bool success = pixels::CPUAffinity::BindToCore(coreId);
                if (success) {
                    std::cout << "Thread " << (useStaticBufferPool ? result->threadId : result->deviceID) 
                              << " bound to CPU core " << coreId << std::endl;
                } else {
                    std::cerr << "Warning: Failed to bind thread to CPU core " << coreId << std::endl;
                }
            }
        }
        
        result->cfgDoubleBuffer = ConfigFactory::Instance().getProperty("pixels.doublebuffer") == "true";
        result->cfgDynamicBuffer = useDynamicBuffer;
        try {
            result->cfgSpdk = ConfigFactory::Instance().getProperty("localfs.async.lib") == "spdk";
        } catch (...) { result->cfgSpdk = false; }
        if (!PixelsParallelStateNext(context.client, bind_data, *result, gstate, true))
        {
            return nullptr;
        }
        return std::move(result);
    }

    void PixelsScanFunction::TransformDuckdbType(const std::shared_ptr<TypeDescription>& type,
                                                 vector<LogicalType>& return_types)
    {
        auto columnSchemas = type->getChildren();
        for (auto columnType : columnSchemas)
        {
            switch (columnType->getCategory())
            {
            //        case TypeDescription::BOOLEAN:
            //            break;
            //        case TypeDescription::BYTE:
            //            break;
            case TypeDescription::SHORT:
            case TypeDescription::INT: return_types.emplace_back(LogicalType::INTEGER);
                break;
            case TypeDescription::LONG: return_types.emplace_back(LogicalType::BIGINT);
                break;
            //        case TypeDescription::FLOAT:
            //            break;
            //        case TypeDescription::DOUBLE:
            //            break;
            case TypeDescription::DECIMAL:
                return_types.emplace_back(LogicalType::DECIMAL(columnType->getPrecision(),
                                                               columnType->getScale()));
                break;
            case TypeDescription::STRING: return_types.emplace_back(LogicalType::VARCHAR);
                break;
            case TypeDescription::DATE: return_types.emplace_back(LogicalType::DATE);
                break;
            //        case TypeDescription::TIME:
            //            break;
            case TypeDescription::TIMESTAMP: return_types.emplace_back(LogicalType::TIMESTAMP);
                break;
            //        case TypeDescription::VARBINARY:
            //            break;
            //        case TypeDescription::BINARY:
            //            break;
            case TypeDescription::VARCHAR: return_types.emplace_back(LogicalType::VARCHAR);
                break;
            case TypeDescription::CHAR: return_types.emplace_back(LogicalType::VARCHAR);
                break;
            //        case TypeDescription::STRUCT:
            //            break;
            default:
                throw InvalidArgumentException(
                    "bad column type in TransformDuckdbType: " + std::to_string(type->getCategory()));
            }
        }
    }

    void PixelsScanFunction::TransformDuckdbChunk(PixelsReadLocalState& data,
                                                  DataChunk& output,
                                                  const std::shared_ptr<TypeDescription>& schema,
                                                  uint64_t thisOutputChunkRows)
    {
        int row_batch_id = 0;
        auto column_ids = data.column_ids;
        auto vectorizedRowBatch = data.vectorizedRowBatch;
        for (uint64_t col_id = 0; col_id < column_ids.size(); col_id++)
        {
            if (IsRowIdColumnId(column_ids.at(col_id)))
            {
                Value constant_42 = Value::BIGINT(42);
                output.data.at(col_id).Reference(constant_42);
                continue;
            }
            auto col = vectorizedRowBatch->cols.at(row_batch_id);
            auto colSchema = schema->getChildren().at(row_batch_id);
            switch (colSchema->getCategory())
            {
            //        case TypeDescription::BOOLEAN:
            //            break;
            //        case TypeDescription::BYTE:
            //            break;
            case TypeDescription::SHORT:
            case TypeDescription::INT:
            {
                auto intCol = std::static_pointer_cast<IntColumnVector>(col);
                Vector vector(LogicalType::INTEGER,
                              (data_ptr_t)(intCol->current()), col->currentValid(), col->getCapacity());
                output.data.at(col_id).Reference(vector);
                //			    auto result_ptr = FlatVector::GetData<int>(output.data.at(col_id));
                //			    memcpy(result_ptr, intCol->intVector + row_offset, thisOutputChunkRows * sizeof(int));
                //			    for(long i = 0; i < thisOutputChunkRows; i++) {
                //				    result_ptr[i] = intCol->intVector[i + row_offset];
                //			    }

                break;
            }
            case TypeDescription::LONG:
            {
                auto longCol = std::static_pointer_cast<LongColumnVector>(col);
                Vector vector(LogicalType::BIGINT,
                              (data_ptr_t)(longCol->current()), col->currentValid(), col->getCapacity());
                output.data.at(col_id).Reference(vector);
                //			    auto result_ptr = FlatVector::GetData<long>(output.data.at(col_id));
                //			    memcpy(result_ptr, longCol->longVector + row_offset, thisOutputChunkRows * sizeof(long));
                //			    for(long i = 0; i < thisOutputChunkRows; i++) {
                //				    result_ptr[i] = longCol->longVector[i + row_offset];
                //			    }
                break;
            }
            //        case TypeDescription::FLOAT:
            //            break;
            //        case TypeDescription::DOUBLE:
            //            break;
            case TypeDescription::DECIMAL:
            {
                auto decimalCol = std::static_pointer_cast<DecimalColumnVector>(col);
                Vector vector(LogicalType::DECIMAL(colSchema->getPrecision(), colSchema->getScale()),
                              (data_ptr_t)(decimalCol->current()), col->currentValid(), col->getCapacity());
                output.data.at(col_id).Reference(vector);
                //			    auto result_ptr = FlatVector::GetData<long>(output.data.at(col_id));
                //			    memcpy(result_ptr, decimalCol->vector + row_offset, thisOutputChunkRows * sizeof(long));
                //			    for(long i = 0; i < thisOutputChunkRows; i++) {
                //				    result_ptr[i] = decimalCol->vector[i + row_offset];
                //			    }
                break;
            }

            //        case TypeDescription::STRING:
            //            break;
            case TypeDescription::DATE:
            {
                auto dateCol = std::static_pointer_cast<DateColumnVector>(col);
                Vector vector(LogicalType::DATE,
                              (data_ptr_t)(dateCol->current()), col->currentValid(), col->getCapacity());
                output.data.at(col_id).Reference(vector);
                //			    auto result_ptr = FlatVector::GetData<int>(output.data.at(col_id));
                //			    memcpy(result_ptr, dateCol->dates + row_offset, thisOutputChunkRows * sizeof(int));
                //			    for(long i = 0; i < thisOutputChunkRows; i++) {
                //				    result_ptr[i] = dateCol->dates[i + row_offset];
                //			    }
                break;
            }

            //        case TypeDescription::TIME:
            //            break;
            case TypeDescription::TIMESTAMP:
            {
                auto tsCol = std::static_pointer_cast<TimestampColumnVector>(col);
                Vector vector(LogicalType::TIMESTAMP,
                              (data_ptr_t)(tsCol->current()), col->currentValid(), col->getCapacity());
                output.data.at(col_id).Reference(vector);
                break;
            }

            //        case TypeDescription::VARBINARY:
            //            break;
            //        case TypeDescription::BINARY:
            //            break;
            case TypeDescription::VARCHAR:
            case TypeDescription::CHAR:
            case TypeDescription::STRING:
            {
                auto binaryCol = std::static_pointer_cast<BinaryColumnVector>(col);
                Vector vector(LogicalType::VARCHAR,
                              (data_ptr_t)(binaryCol->current()), col->currentValid(), col->getCapacity());
                output.data.at(col_id).Reference(vector);
                //			    auto result_ptr = FlatVector::GetData<duckdb::string_t>(output.data.at(col_id));
                //                memcpy(result_ptr, binaryCol->vector + row_offset, thisOutputChunkRows * sizeof(string_t));
                break;
            }
                //        case TypeDescription::STRUCT:
                //            break;
                //			default:
                //				throw InvalidArgumentException("bad column type " + std::to_string(colSchema->getCategory()));
            }
            row_batch_id++;
        }
        vectorizedRowBatch->increment(thisOutputChunkRows);
    }

    bool PixelsScanFunction::PixelsParallelStateNext(ClientContext& context, PixelsReadBindData& bind_data,
                                                     PixelsReadLocalState& scan_data,
                                                     PixelsReadGlobalState& parallel_state,
                                                     bool is_init_state)
    {
        PROFILE_START("PixelsScanFunction.PixelsParallelStateNext.Total");
        PROFILE_START("PixelsScanFunction.StateNext.LockWait");
        unique_lock<mutex> parallel_lock(parallel_state.lock);
        PROFILE_END("PixelsScanFunction.StateNext.LockWait");
        if (parallel_state.error_opening_file)
        {
            PROFILE_END("PixelsScanFunction.PixelsParallelStateNext.Total");
            throw InvalidArgumentException("PixelsScanInitLocal: file open error.");
        }

        auto& StorageInstance = parallel_state.storageArrayScheduler;
        // In the following two cases, the state ends:
        // 1. When PixelsScanInitLocal invokes this function, if all files are
        // fetched by other threads, this means this thread doesn't need do anything, so just return false;
        // 2. When PixelsScanImplementation invokes this function (scan_data.next_file_index > -1), if
        // scan_data.next_file_index >= (int) StorageInstance.getFileSum(scan_data.deviceID), it means the current file is already
        // done, so the function return false.
        if ((is_init_state &&
                parallel_state.file_index.at(scan_data.deviceID) >= StorageInstance->getFileSum(scan_data.deviceID)) ||
            scan_data.next_file_index >= StorageInstance->getFileSum(scan_data.deviceID))
        {

                            // ::BufferPool::Reset();
            // if async io is enabled, we need to unregister uring buffer
            if (ConfigFactory::Instance().boolCheckProperty("localfs.enable.async.io"))
            {
                if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "iouring")
                {
                    int remaining_threads = --parallel_state.active_threads;
                    if (remaining_threads==0&&!parallel_state.all_done)
                    {
                        parallel_state.all_done=true;
                        scan_data.shouldPrintProfileSummary = true;
                        // ::DirectUringRandomAccessFile::Reset();
                        // Print memory statistics if enabled
                        if (GlobalByteBufferPool::Instance().IsInitialized() &&
                            GlobalByteBufferPool::Instance().IsStatsEnabled())
                        {
                            auto stats = GlobalByteBufferPool::Instance().GetMemoryStats();
                            std::cout << "\n=== Query Finished - Memory Statistics ===" << std::endl;
                            std::cout << "Total Allocated: " << (stats.totalAllocatedBytes / 1024.0 / 1024.0) << " MB" << std::endl;
                            std::cout << "Currently Used: " << (stats.totalUsedBytes / 1024.0 / 1024.0) << " MB" << std::endl;
                            std::cout << "Actually Requested: " << (stats.totalRequestedBytes / 1024.0 / 1024.0) << " MB" << std::endl;
                            std::cout << "External Fragmentation: " << (stats.fragmentationRatio * 100.0) << " %" << std::endl;
                            std::cout << "Internal Fragmentation: " << (stats.internalFragmentationRatio * 100.0) << " %" << std::endl;
                            std::cout << "\nBuffer Growth Analysis by Size Class:" << std::endl;
                            for (const auto& pair : stats.bufferCountPerClass)
                            {
                                size_t sizeClass = pair.first;
                                size_t totalCount = pair.second;
                                size_t inUseCount = 0;
                                size_t preAllocCount = 0;
                                size_t dynamicAllocCount = 0;
                                
                                auto it = stats.inUseCountPerClass.find(sizeClass);
                                if (it != stats.inUseCountPerClass.end())
                                {
                                    inUseCount = it->second;
                                }
                                
                                auto preIt = stats.preAllocatedCountPerClass.find(sizeClass);
                                if (preIt != stats.preAllocatedCountPerClass.end())
                                {
                                    preAllocCount = preIt->second;
                                }
                                
                                auto dynIt = stats.dynamicAllocatedCountPerClass.find(sizeClass);
                                if (dynIt != stats.dynamicAllocatedCountPerClass.end())
                                {
                                    dynamicAllocCount = dynIt->second;
                                }
                                
                                std::cout << "  " << (sizeClass / 1024 / 1024) << " MB buffers:" << std::endl;
                                std::cout << "    Pre-allocated: " << preAllocCount << std::endl;
                                std::cout << "    Dynamic Growth: +" << dynamicAllocCount;
                                if (dynamicAllocCount > 0) {
                                    double growthRate = (preAllocCount > 0) ? 
                                        (dynamicAllocCount * 100.0 / preAllocCount) : 100.0;
                                    std::cout << " (+" << growthRate << "%)";
                                }
                                std::cout << std::endl;
                                std::cout << "    Total: " << totalCount << " (" << inUseCount << " in use, " 
                                          << (totalCount - inUseCount) << " free)" << std::endl;
                            }
                            std::cout << "========================================\n" << std::endl;
                        }
                    }
                }
                else if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "spdk")
                {
                    int remaining_threads = --parallel_state.active_threads;
                    if (remaining_threads == 0 && !parallel_state.all_done)
                    {
                        parallel_state.all_done = true;
                        scan_data.shouldPrintProfileSummary = true;
                    }
                }
                else if (ConfigFactory::Instance().getProperty("localfs.async.lib") == "aio")
                {
                    throw InvalidArgumentException(
                        "PhysicalLocalReader::readAsync: We don't support aio for our async read yet.");
                }
            }
            else
            {
                int remaining_threads = --parallel_state.active_threads;
                if (remaining_threads == 0 && !parallel_state.all_done)
                {
                    parallel_state.all_done = true;
                    scan_data.shouldPrintProfileSummary = true;
                }
            }

            parallel_lock.unlock();
            PROFILE_END("PixelsScanFunction.PixelsParallelStateNext.Total");
            ::TimeProfiler::Instance().Collect();
            return false;
        }
        bind_data.curFileId++;
        scan_data.curr_file_index = scan_data.next_file_index;
        scan_data.curr_batch_index = scan_data.next_batch_index;
        scan_data.next_file_index = parallel_state.file_index.at(scan_data.deviceID);
        scan_data.next_batch_index = StorageInstance->getBatchID(scan_data.deviceID, scan_data.next_file_index);
        scan_data.curr_file_name = scan_data.next_file_name;
        parallel_state.file_index.at(scan_data.deviceID)++;
        parallel_lock.unlock();
        // The below code uses global state but no race happens, so we don't need the lock anymore


        if (scan_data.currReader != nullptr)
        {
            PROFILE_START("PixelsScanFunction.StateNext.CloseReader");
            scan_data.currReader->close();
            PROFILE_END("PixelsScanFunction.StateNext.CloseReader");
        }

        if (scan_data.cfgDoubleBuffer)
        {
            PROFILE_START("PixelsScanFunction.StateNext.SwitchBuffer");
            if (scan_data.cfgDynamicBuffer)
            {
                ::DynamicBufferPool::Switch();
            }
#ifdef PIXELS_ENABLE_SPDK
            else if (scan_data.cfgSpdk)
            {
                ::SpdkBufferPool::Switch();
            }
#endif
            else
            {
                ::BufferPool::Switch();
            }
            if (ShouldPrintBufferIdxDebug(scan_data.threadId))
            {
                std::cout << "[BufferIdxDebug] switch"
                          << " thread=" << scan_data.threadId
                          << " curr_file=" << scan_data.curr_file_name
                          << " next_file=" << scan_data.next_file_name
                          << " currBufferIdx=" << ::BufferPool::GetCurrentBufferIdx()
                          << " nextBufferIdx=" << ::BufferPool::GetNextBufferIdx()
                          << std::endl;
            }
            if (!is_init_state && scan_data.prefetchRing != nullptr)
            {
                pixels::ThreadContext::SwapRings();
                scan_data.ring = pixels::ThreadContext::GetCurrentRing();
                scan_data.prefetchRing = pixels::ThreadContext::GetPrefetchRing();
            }
            PROFILE_END("PixelsScanFunction.StateNext.SwitchBuffer");
        }
        // double/single buffer

        scan_data.currReader = scan_data.nextReader;
        scan_data.currPixelsRecordReader = scan_data.nextPixelsRecordReader;
        // asyncReadComplete is not invoked in the first run (is_init_state = true)
        if (scan_data.currPixelsRecordReader != nullptr)
        {
            auto currPixelsRecordReader = std::static_pointer_cast<PixelsRecordReaderImpl>(
                scan_data.currPixelsRecordReader);
            if (!scan_data.cfgDoubleBuffer)
            {
                //single buffer
                PROFILE_START("PixelsScanFunction.curr_reader.read");
                currPixelsRecordReader->read();
                PROFILE_END("PixelsScanFunction.curr_reader.read");

            }

            if (ShouldPrintBufferIdxDebug(scan_data.threadId))
            {
                std::cout << "[BufferIdxDebug] curr_complete"
                          << " thread=" << scan_data.threadId
                          << " file=" << currPixelsRecordReader->getFileName()
                          << " currBufferIdx=" << ::BufferPool::GetCurrentBufferIdx()
                          << " nextBufferIdx=" << ::BufferPool::GetNextBufferIdx()
                          << std::endl;
            }

            PROFILE_START("PixelsScanFunction.curr_reader.complete");
            currPixelsRecordReader->asyncReadComplete((int)scan_data.column_names.size());
            PROFILE_END("PixelsScanFunction.curr_reader.complete");
        }
        if (scan_data.next_file_index < StorageInstance->getFileSum(scan_data.deviceID))
        {
            auto builder = std::make_shared<PixelsReaderBuilder>();
            std::shared_ptr<::Storage> storage = StorageFactory::getInstance()->getStorage(::Storage::file);
            scan_data.next_file_name = StorageInstance->getFileName(scan_data.deviceID, scan_data.next_file_index);
            bool usePrefetchRing = scan_data.cfgDoubleBuffer && scan_data.prefetchRing != nullptr;
            if (usePrefetchRing)
            {
                pixels::ThreadContext::UsePrefetchRing();
            }
            PROFILE_START("PixelsScanFunction.StateNext.BuildPixelsReader");
            scan_data.nextReader = builder->setPath(scan_data.next_file_name)
                                          ->setStorage(storage)
                                          ->setPixelsFooterCache(parallel_state.footerCache)
                                          ->build();
            PROFILE_END("PixelsScanFunction.StateNext.BuildPixelsReader");
            if (usePrefetchRing)
            {
                pixels::ThreadContext::UseCurrentRing();
            }

            PixelsReaderOption option = GetPixelsReaderOption(scan_data, parallel_state);
            PROFILE_START("PixelsScanFunction.StateNext.CreateRecordReader");
            scan_data.nextPixelsRecordReader = scan_data.nextReader->read(option);
            PROFILE_END("PixelsScanFunction.StateNext.CreateRecordReader");
            auto nextPixelsRecordReader = std::static_pointer_cast<PixelsRecordReaderImpl>(
                scan_data.nextPixelsRecordReader);

            if (scan_data.cfgDoubleBuffer)
            {
                //double buffer
                if (ShouldPrintBufferIdxDebug(scan_data.threadId))
                {
                    std::cout << "[BufferIdxDebug] next_read"
                              << " thread=" << scan_data.threadId
                              << " file=" << nextPixelsRecordReader->getFileName()
                              << " currBufferIdx=" << ::BufferPool::GetCurrentBufferIdx()
                              << " nextBufferIdx=" << ::BufferPool::GetNextBufferIdx()
                              << std::endl;
                }
                PROFILE_START("PixelsScanFunction.next_reader.read");
                nextPixelsRecordReader->read();
                PROFILE_END("PixelsScanFunction.next_reader.read");
            }
        }
        else
        {
            scan_data.nextReader = nullptr;
            scan_data.nextPixelsRecordReader = nullptr;
        }
        PROFILE_END("PixelsScanFunction.PixelsParallelStateNext.Total");
        return true;
    }

    PixelsReaderOption
    PixelsScanFunction::GetPixelsReaderOption(PixelsReadLocalState& local_state, PixelsReadGlobalState& global_state)
    {
        PixelsReaderOption option;
        option.setSkipCorruptRecords(true);
        option.setTolerantSchemaEvolution(true);
        option.setEnableEncodedColumnVector(true);
        option.setFilter(global_state.filters);
        option.setEnabledFilterPushDown(enable_filter_pushdown);
        // includeCols comes from the caller of PixelsPageSource
        option.setIncludeCols(local_state.column_names);
        option.setRGRange(0, local_state.nextReader->getRowGroupNum());
        option.setQueryId(1);
        int stride = std::stoi(ConfigFactory::Instance().getProperty("pixel.stride"));
        option.setBatchSize(stride);
        return option;
    }
}
