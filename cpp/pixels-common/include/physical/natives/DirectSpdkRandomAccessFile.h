/*
 * Copyright 2026 PixelsDB.
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
 * SPDK user-space NVMe driver implementation for Pixels.
 *
 * Architecture:
 *   - SpdkGlobal: process-wide singleton; probes all VFIO-bound PCIe NVMe
 *     devices and loads the file→LBA mapping built by gen_lba_map.py.
 *   - DirectSpdkRandomAccessFile: per-file handle; each instance maps to one
 *     set of LBA extents. Per-thread qpairs are allocated on first use.
 *
 * Async model (mirrors DirectUringRandomAccessFile):
 *   readAsync()        → spdk_nvme_ns_cmd_read into internal DMA buffers
 *   readAsyncSubmit()  → no-op (SPDK submits immediately on cmd_read)
 *   readAsyncComplete()→ spdk_nvme_qpair_process_completions + memcpy to
 *                        the caller-supplied output buffers
 *
 * Buffer strategy (copy path):
 *   The existing BufferPool allocates posix_memalign memory which is not
 *   DMA-capable. We therefore allocate a temporary spdk_dma_malloc buffer
 *   per async read and copy the result into the caller's buffer on complete.
 *   This adds ~1% overhead vs. a zero-copy design; it avoids any change to
 *   PixelsRecordReaderImpl or BufferPool.
 */

#pragma once
#ifdef PIXELS_ENABLE_SPDK

#include "physical/natives/PixelsRandomAccessFile.h"
#include "physical/natives/ByteBuffer.h"

#include <spdk/nvme.h>
#include <spdk/env.h>
#include <spdk/nvme_spec.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>

static constexpr uint64_t SPDK_LBA_SIZE = 4096;   // Samsung PM9A3 formatted LBA size

// ─── LBA extent record ────────────────────────────────────────────────────

struct SpdkLbaExtent {
    uint64_t lba_start;   // absolute LBA on the NVMe namespace
    uint32_t lba_count;   // number of 4096-byte LBAs
};

// ─── One in-flight async read operation ───────────────────────────────────

struct SpdkReadOp {
    std::shared_ptr<ByteBuffer> out_buf;   // caller-supplied output buffer
    void*    dma_buf;                      // spdk_dma_malloc'd read target
    uint64_t prefix;                       // alignment prefix bytes to skip
    int      data_len;                     // number of bytes to copy to out_buf
    uint64_t io_bytes = 0;                 // bytes submitted to the NVMe device
    bool     done  = false;
    int      rc    = 0;                    // 0=ok, -1=nvme error
};

// ─── Process-wide SPDK context ────────────────────────────────────────────

class SpdkGlobal {
public:
    struct DevInfo {
        struct spdk_nvme_ctrlr* ctrlr      = nullptr;
        struct spdk_nvme_ns*    ns         = nullptr;
        uint32_t                block_size = SPDK_LBA_SIZE;
    };

    struct FileInfo {
        std::string                pci;
        std::vector<SpdkLbaExtent> extents;
        uint64_t                   file_bytes = 0;
    };

    // Thread-safe; safe to call from every DirectSpdkRandomAccessFile ctor.
    static void Initialize(const std::string& lba_map_path);
    static void Shutdown();
    static bool IsReady();

    // Lookup helpers called after Initialize().
    static bool FindFile(const std::string& path, FileInfo& out);
    static bool FindDevice(const std::string& pci_addr, DevInfo& out);

    // Query-scoped device I/O counters. These are independent of the optional
    // Pixels profiler and therefore remain available in benchmark builds.
    static void ResetIoStats();
    static void RecordIoSubmit();
    static void RecordIoComplete(uint64_t bytes);
    static void RecordIoError();
    static void PrintIoStats();

private:
    static std::once_flag init_flag;
    static std::atomic<bool> ready;
    static std::mutex devices_mutex;

    static std::unordered_map<std::string, DevInfo>  devices;   // pci → dev
    static std::unordered_map<std::string, FileInfo> file_map;  // path → info
    static std::atomic<uint64_t> io_submitted;
    static std::atomic<uint64_t> io_completed;
    static std::atomic<uint64_t> io_errors;
    static std::atomic<uint64_t> io_bytes;
    static std::atomic<uint64_t> io_start_ns;
    static std::atomic<uint64_t> io_end_ns;

    // spdk_nvme_probe callbacks
    static bool probe_cb(void* ctx,
                         const struct spdk_nvme_transport_id* trid,
                         struct spdk_nvme_ctrlr_opts* opts);
    static void attach_cb(void* ctx,
                          const struct spdk_nvme_transport_id* trid,
                          struct spdk_nvme_ctrlr* ctrlr,
                          const struct spdk_nvme_ctrlr_opts* opts);
};

// ─── Per-file SPDK random-access file ─────────────────────────────────────

class DirectSpdkRandomAccessFile : public PixelsRandomAccessFile {
public:
    explicit DirectSpdkRandomAccessFile(const std::string& path);
    ~DirectSpdkRandomAccessFile();

    // ── Static lifecycle (mirrors DirectUringRandomAccessFile) ─────────────

    // Initialize thread-local NVMe I/O queue pair.
    // Called once per worker thread before any readAsync().
    static void Initialize();

    // Release thread-local queue pair.
    // Called once per worker thread at scan teardown.
    static void Reset();

    // No-ops: SPDK does not use pre-registered buffer vectors.
    static void RegisterBufferFromPool(std::vector<uint32_t> /*colIds*/) {}
    static void RegisterBuffer(std::vector<std::shared_ptr<ByteBuffer>> /*bufs*/) {}

    // ── PixelsRandomAccessFile interface ───────────────────────────────────

    void seek(long off) override;
    long length() override;

    // Synchronous reads (used for Pixels file header/footer)
    std::shared_ptr<ByteBuffer> readFully(int len) override;
    std::shared_ptr<ByteBuffer> readFully(int len, std::shared_ptr<ByteBuffer> bb) override;
    long readLong() override;
    int  readInt()  override;
    char readChar() override;
    void close() override;

    // ── Async read interface ───────────────────────────────────────────────

    // Submit an asynchronous NVMe read.  The data will be available in
    // `buffer` after readAsyncComplete() returns.
    std::shared_ptr<ByteBuffer> readAsync(int length,
                                          std::shared_ptr<ByteBuffer> buffer,
                                          int index);

    // No-op: SPDK does not batch-submit; each spdk_nvme_ns_cmd_read is
    // immediately enqueued on the hardware SQ.
    void readAsyncSubmit(int size);

    // Poll the CQ until `size` operations complete, then memcpy each DMA
    // buffer into its corresponding output buffer.
    void readAsyncComplete(int size);

private:
    // Issue a synchronous NVMe read of `byte_count` bytes starting at
    // `byte_offset` in the file; copy result into `dst`.
    // `dst` must be large enough for the aligned read, but only `byte_count`
    // bytes are meaningful starting at offset 0.
    void syncRead(uint64_t byte_offset, int byte_count, void* dst);

    std::string  file_path;
    long         file_offset = 0;
    long         file_bytes  = 0;

    struct spdk_nvme_ctrlr* ctrlr      = nullptr;
    struct spdk_nvme_ns*    ns         = nullptr;
    uint32_t                block_size = SPDK_LBA_SIZE;
    std::vector<SpdkLbaExtent> extents;

    // Per-instance pointer into the thread-local qpair cache (not owned).
    // Points to tls_qpairs[ctrlr] which is allocated once per thread per
    // controller and persists for the thread's lifetime.
    struct spdk_nvme_qpair* inst_qpair = nullptr;

    // Per-instance pending async operations for the current read batch.
    // Populated by readAsync(), drained by readAsyncComplete().
    std::vector<SpdkReadOp*> pending_ops;

    // Thread-local qpair cache: one qpair per (thread × controller).
    // Allocated on first file open for that controller on this thread,
    // freed by Reset() or thread exit.  Eliminates the per-file
    // alloc_io_qpair / free_io_qpair admin-queue round-trips.
    static thread_local std::unordered_map<struct spdk_nvme_ctrlr*,
                                           struct spdk_nvme_qpair*> tls_qpairs;
};

#endif // PIXELS_ENABLE_SPDK
