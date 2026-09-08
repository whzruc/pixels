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
#define DUCKDB_EXTENSION_MAIN

#include "pixels_extension.hpp"
#include "PixelsScanFunction.hpp"
#include "PixelsReadBindData.hpp"
#include "ParquetPixelsScan.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "physical/GlobalByteBufferPool.h"
#include "physical/GlobalStaticBufferPool.h"
#include "utils/ConfigFactory.h"

#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <cstdlib>
#include <fstream>
#include <unistd.h>

namespace duckdb
{

    // Pixels Scan Replacement for duckdb 1.1
    unique_ptr <TableRef> PixelsScanReplacement(ClientContext &context, ReplacementScanInput &input,
                                                optional_ptr <ReplacementScanData> data)
    {
        auto table_name = ReplacementScan::GetFullPath(input);
//    if(!ReplacementScan::CanReplace(table_name,{"pixels"})){
//        return nullptr;
//    }
        auto lower_name = StringUtil::Lower(table_name);
        if (!StringUtil::EndsWith(lower_name, ".pxl") && !StringUtil::Contains(lower_name, ".pxl?"))
        {
            return nullptr;
        }
        auto table_function = make_uniq<TableFunctionRef>();
        vector <unique_ptr<ParsedExpression>> children;
        children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
        table_function->function = make_uniq<FunctionExpression>("pixels_scan", std::move(children));
        if (!FileSystem::HasGlob(table_name))
        {
            auto &fs = FileSystem::GetFileSystem(context);
            table_function->alias = fs.ExtractBaseName(table_name);
        }
        return std::move(table_function);
    }


    void PixelsExtension::Load(ExtensionLoader &loader) {
#if defined(__GLIBC__)
        // 方案A: 避免大块列缓冲(如 BinaryColumnVector 的 string_t 数组, stride=10000 时约 156KB,
        // 超过 glibc 默认 M_MMAP_THRESHOLD=128KB)每个文件反复走 mmap/munmap。
        // 在 48 线程并发下, 这些 mmap/munmap(写锁) 与首次触摸缺页(读锁) 会争抢进程级 mmap_lock,
        // 表现为 setRef/createColumn 大量 off-CPU 阻塞在 do_user_addr_fault -> down_read。
        // 关闭 malloc 的 mmap 路径并禁止堆回缩, 让大块分配从 arena 复用、不再反复 munmap。
        {
            bool tuneMalloc = true;
            try {
                tuneMalloc = ConfigFactory::Instance().boolCheckProperty("pixels.malloc.tune");
            } catch (...) {
                tuneMalloc = true; // 默认开启
            }
            if (tuneMalloc) {
                mallopt(M_MMAP_MAX, 0);          // 禁止 malloc 家族使用 mmap, 大块分配走 arena 并复用
                mallopt(M_TRIM_THRESHOLD, -1);   // 不把堆内存归还内核, 避免反复 munmap

                // arena 限制(可选, 默认关闭): glibc 默认每进程最多 8 * nproc 个 arena。在 48 核下
                // 会创建大量 arena, 各自 new_heap(mmap)/grow_heap(mprotect) 扩容, off-CPU 火焰图里
                // 表现为 __GI___mprotect(~43%) + sysmalloc(~23%)。理论上限制 arena 数能减少 mprotect。
                // 但实测(q45/doublebuffer/t48): M_ARENA_MAX=8 反而把墙钟从 ~13.6s 拉到 ~26s ——
                // 那些 mprotect off-CPU 实际与多 arena 的并行分配重叠, 并非墙钟关键路径; 收紧 arena
                // 只是把争用从 mmap_lock 移到 arena 互斥锁, 得不偿失。因此默认 0(不限制, 沿用 glibc),
                // 仅保留 pixels.malloc.arena_max 作为可调旋钮(>0 才调用 mallopt 收紧)。
                int arenaMax = 0;
                try {
                    arenaMax = std::stoi(ConfigFactory::Instance().getProperty("pixels.malloc.arena_max"));
                } catch (...) {
                    arenaMax = 0; // 默认不限制
                }
                if (arenaMax > 0) {
                    mallopt(M_ARENA_MAX, arenaMax);
                }
                std::cout << "Pixels Extension: glibc malloc tuned (M_MMAP_MAX=0, M_TRIM_THRESHOLD=-1, "
                          << "M_ARENA_MAX=" << (arenaMax > 0 ? std::to_string(arenaMax) : std::string("unlimited"))
                          << ")" << std::endl;
            }
        }
#endif
        auto &db_instance=loader.GetDatabaseInstance();
        Connection con(db_instance);
        con.BeginTransaction();

        auto &context=*con.context;
        auto &catalog=Catalog::GetSystemCatalog(*con.context);

        auto scan_fun=PixelsScanFunction::GetFunctionSet();

        CreateTableFunctionInfo cinfo(scan_fun);
        cinfo.name = "pixels_scan";

        catalog.CreateTableFunction(context, &cinfo);

        // Register read_parquet_uring (io_uring + double-buffer)
        auto pq_pixels_fun = ParquetPixelsScanFunction::GetFunctionSet();
        CreateTableFunctionInfo pq_pixels_info(pq_pixels_fun);
        catalog.CreateTableFunction(context, &pq_pixels_info);

        con.Commit();

        auto &config = DBConfig::GetConfig(db_instance);
        config.replacement_scans.emplace_back(PixelsScanReplacement);

        // Initialize Global Static ByteBuffer Pool (if enabled)
        if (ConfigFactory::Instance().getBoolProperty("pixel.enable.globalStaticBytebuffer", false))
        {
            if (ConfigFactory::Instance().getBoolProperty("pixels.enable.dynamic.buffer", false))
            {
                throw InvalidArgumentException("PixelsExtension::Load globalStaticBytebuffer is conflits with dynamic buffer");
            }
            try {
                // Get column size CSV path
                std::string columnSizeCSVPath = ConfigFactory::Instance().getProperty("pixel.globalStaticBytebuffer.columnSize");

                // Get filesystem block size
                int fsBlockSize = 4096; // Default value
                try {
                    fsBlockSize = std::stoi(ConfigFactory::Instance().getProperty("localfs.block.size"));
                } catch (...) {
                    // Use default if config not available
                }

                // Maximum threads (48 threads * 2 buffers = 96 buffers per column)
                int maxThreads = 48;

                // Initialize the global static buffer pool
                GlobalStaticBufferPool::Instance().Initialize(columnSizeCSVPath, fsBlockSize, maxThreads);

                std::cout << "Pixels Extension: Global Static ByteBuffer Pool initialized successfully" << std::endl;

                // Notify external profiler (e.g. perf stat -p) that init is done.
                // The ready-file path is passed via PIXELS_PERF_READY_FILE; if the
                // env var is not set this is a no-op.
                const char* ready_file = std::getenv("PIXELS_PERF_READY_FILE");
                if (ready_file && ready_file[0] != '\0') {
                    std::string tmp = std::string(ready_file) + ".tmp";
                    { std::ofstream f(tmp); f << getpid() << "\n"; }
                    ::rename(tmp.c_str(), ready_file);
                }
            } catch (const std::exception& e) {
                std::cerr << "Pixels Extension: Failed to initialize Global Static ByteBuffer Pool: "
                          << e.what() << std::endl;
                std::cerr << "Pixels Extension: Continuing without global static buffer pool (will use local allocation)"
                          << std::endl;
            }
        }
        // Initialize Global Dynamic ByteBuffer Pool (if enabled)
        else if (ConfigFactory::Instance().getBoolProperty("pixel.enable.globalBytebuffer", false))
        {
            try {
                // Get configuration path
                std::string configPath = "/home/whz/test/pixels/cpp/global-bufferpool.properties";
                
                // Get number of threads - use a safe default that covers typical usage
                // Note: User may change threads later with SET threads=N,
                // so we pre-allocate for max expected threads
                int numThreads = 48; // Default to 48 threads (safe upper bound)
                try {
                    std::string threadsStr = ConfigFactory::Instance().getProperty("pixel.threads");
                    int configThreads = std::stoi(threadsStr);
                    // Only use config value if it's valid (positive)
                    if (configThreads > 0) {
                        numThreads = configThreads;
                    }
                } catch (...) {
                    // Use default if config not available or invalid
                }
                
                // Get filesystem block size
                int fsBlockSize = 4096; // Default value
                try {
                    fsBlockSize = std::stoi(ConfigFactory::Instance().getProperty("localfs.block.size"));
                } catch (...) {
                    // Use default if config not available
                }
                
                std::cout << "Pixels Extension: Initializing Global ByteBuffer Pool for " << numThreads << " threads" << std::endl;
                
                // Initialize the global buffer pool
                GlobalByteBufferPool::Instance().Initialize(configPath, fsBlockSize, numThreads);
                
                std::cout << "Pixels Extension: Global ByteBuffer Pool initialized successfully" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Pixels Extension: Failed to initialize Global ByteBuffer Pool: " 
                          << e.what() << std::endl;
                std::cerr << "Pixels Extension: Continuing without global buffer pool (will use DirectIoLib fallback)" 
                          << std::endl;
            }
        }

    }

    std::string PixelsExtension::Name()
    {
        return "pixels";
    }


} // namespace duckdb

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
