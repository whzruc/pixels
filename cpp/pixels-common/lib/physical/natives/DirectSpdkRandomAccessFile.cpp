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

#ifdef PIXELS_ENABLE_SPDK

#include "physical/natives/DirectSpdkRandomAccessFile.h"
#include "profiler/TimeProfiler.h"
#include "utils/ConfigFactory.h"
#include "exception/InvalidArgumentException.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <cstring>
#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>

using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════════════════
//  SpdkGlobal  —  process-wide state
// ═══════════════════════════════════════════════════════════════════════════

std::once_flag                                         SpdkGlobal::init_flag;
std::atomic<bool>                                      SpdkGlobal::ready{false};
std::mutex                                             SpdkGlobal::devices_mutex;
std::unordered_map<std::string, SpdkGlobal::DevInfo>  SpdkGlobal::devices;
std::unordered_map<std::string, SpdkGlobal::FileInfo> SpdkGlobal::file_map;
std::atomic<uint64_t> SpdkGlobal::io_submitted{0};
std::atomic<uint64_t> SpdkGlobal::io_completed{0};
std::atomic<uint64_t> SpdkGlobal::io_errors{0};
std::atomic<uint64_t> SpdkGlobal::io_bytes{0};
std::atomic<uint64_t> SpdkGlobal::io_start_ns{0};
std::atomic<uint64_t> SpdkGlobal::io_end_ns{0};

static uint64_t SpdkSteadyNowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void SpdkGlobal::ResetIoStats()
{
    io_submitted.store(0, std::memory_order_relaxed);
    io_completed.store(0, std::memory_order_relaxed);
    io_errors.store(0, std::memory_order_relaxed);
    io_bytes.store(0, std::memory_order_relaxed);
    io_start_ns.store(0, std::memory_order_relaxed);
    io_end_ns.store(0, std::memory_order_relaxed);
}

void SpdkGlobal::RecordIoSubmit()
{
    const uint64_t now = SpdkSteadyNowNs();
    uint64_t unset = 0;
    io_start_ns.compare_exchange_strong(unset, now, std::memory_order_relaxed);
    io_submitted.fetch_add(1, std::memory_order_relaxed);
}

void SpdkGlobal::RecordIoComplete(uint64_t bytes)
{
    io_completed.fetch_add(1, std::memory_order_relaxed);
    io_bytes.fetch_add(bytes, std::memory_order_relaxed);
    io_end_ns.store(SpdkSteadyNowNs(), std::memory_order_relaxed);
}

void SpdkGlobal::RecordIoError()
{
    io_errors.fetch_add(1, std::memory_order_relaxed);
    io_end_ns.store(SpdkSteadyNowNs(), std::memory_order_relaxed);
}

void SpdkGlobal::PrintIoStats()
{
    const uint64_t submitted = io_submitted.load(std::memory_order_relaxed);
    const uint64_t completed = io_completed.load(std::memory_order_relaxed);
    const uint64_t errors = io_errors.load(std::memory_order_relaxed);
    const uint64_t bytes = io_bytes.load(std::memory_order_relaxed);
    const uint64_t start = io_start_ns.load(std::memory_order_relaxed);
    const uint64_t end = io_end_ns.load(std::memory_order_relaxed);
    const double seconds = end > start ? static_cast<double>(end - start) / 1e9 : 0.0;
    const double iops = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
    const double mib_s = seconds > 0.0
        ? static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds : 0.0;
    std::cout << std::fixed << std::setprecision(6)
              << "SPDK_IO_STATS"
              << " submitted=" << submitted
              << " completed=" << completed
              << " errors=" << errors
              << " read_bytes=" << bytes
              << " elapsed_s=" << seconds
              << " read_iops=" << iops
              << " read_mib_s=" << mib_s
              << std::endl;
}

bool SpdkGlobal::probe_cb(void* /*ctx*/,
                           const struct spdk_nvme_transport_id* /*trid*/,
                           struct spdk_nvme_ctrlr_opts* /*opts*/)
{
    // Accept every PCIe NVMe device offered by the OS (VFIO-bound ones).
    return true;
}

void SpdkGlobal::attach_cb(void* /*ctx*/,
                            const struct spdk_nvme_transport_id* trid,
                            struct spdk_nvme_ctrlr* ctrlr,
                            const struct spdk_nvme_ctrlr_opts* /*opts*/)
{
    // NVMe namespace 1 is the only namespace used by Pixels.
    struct spdk_nvme_ns* ns = spdk_nvme_ctrlr_get_ns(ctrlr, 1);
    if (!ns || !spdk_nvme_ns_is_active(ns)) {
        return;
    }

    DevInfo info;
    info.ctrlr      = ctrlr;
    info.ns         = ns;
    info.block_size = spdk_nvme_ns_get_sector_size(ns);

    std::string pci(trid->traddr);
    std::lock_guard<std::mutex> lock(devices_mutex);
    devices[pci] = info;
    (void)0;
}

void SpdkGlobal::Initialize(const std::string& lba_map_path)
{
    std::call_once(init_flag, [&lba_map_path]() {
        PROFILE_START("Spdk.Initialize.Total");
        // ── 1. Init SPDK/DPDK environment ─────────────────────────────────
        struct spdk_env_opts opts;
        spdk_env_opts_init(&opts);
        opts.name     = "pixels_spdk";
        opts.shm_id   = -1;  // private hugepage pool (no shared memory)
        opts.mem_size = 0;   // 0 = SPDK default (use available hugepages)

        PROFILE_START("Spdk.Initialize.Environment");
        int env_rc = spdk_env_init(&opts);
        PROFILE_END("Spdk.Initialize.Environment");
        if (env_rc < 0) {
            throw std::runtime_error(
                "SpdkGlobal::Initialize: spdk_env_init failed. "
                "Did you configure hugepages? "
                "(echo 8192 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)");
        }
        PROFILE_START("Spdk.Initialize.Probe");
        int rc = spdk_nvme_probe(nullptr, nullptr, probe_cb, attach_cb, nullptr);
        PROFILE_END("Spdk.Initialize.Probe");
        if (rc != 0) {
            throw std::runtime_error(
                "SpdkGlobal::Initialize: spdk_nvme_probe failed, rc=" +
                std::to_string(rc));
        }

        if (devices.empty()) {
            throw std::runtime_error(
                "SpdkGlobal::Initialize: no NVMe devices found via SPDK. "
                "Run testcase/spdk/bind_vfio.sh first.");
        }


        // ── 3. Load LBA map JSON ───────────────────────────────────────────
        PROFILE_START("Spdk.Initialize.LoadLbaMap");
        std::ifstream f(lba_map_path);
        if (!f.is_open()) {
            throw std::runtime_error(
                "SpdkGlobal::Initialize: cannot open LBA map: " + lba_map_path +
                "\nRun testcase/spdk/gen_lba_map.py first.");
        }

        json j;
        try {
            f >> j;
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "SpdkGlobal::Initialize: malformed JSON in " + lba_map_path +
                ": " + e.what());
        }

        for (auto& [path, entry] : j.items()) {
            FileInfo fi;
            fi.pci        = entry["pci"].get<std::string>();
            fi.file_bytes = 0;
            for (auto& ext : entry["extents"]) {
                SpdkLbaExtent e;
                e.lba_start = ext["lba_start"].get<uint64_t>();
                e.lba_count = ext["lba_count"].get<uint32_t>();
                fi.file_bytes += static_cast<uint64_t>(e.lba_count) * SPDK_LBA_SIZE;
                fi.extents.push_back(e);
            }
            // Prefer the actual file size stored by gen_lba_map.py over the
            // LBA-aligned size so PixelsReader seeks to the correct postscript offset.
            if (entry.contains("file_bytes")) {
                fi.file_bytes = entry["file_bytes"].get<uint64_t>();
            }
            file_map[path] = std::move(fi);
        }
        PROFILE_END("Spdk.Initialize.LoadLbaMap");

        ready.store(true, std::memory_order_release);
        PROFILE_END("Spdk.Initialize.Total");
    });
}

void SpdkGlobal::Shutdown()
{
    if (!ready.load()) return;
    for (auto& [pci, dev] : devices) {
        spdk_nvme_detach(dev.ctrlr);
    }
    devices.clear();
    file_map.clear();
    ready.store(false, std::memory_order_release);
}

bool SpdkGlobal::IsReady()
{
    return ready.load(std::memory_order_acquire);
}

bool SpdkGlobal::FindFile(const std::string& path, FileInfo& out)
{
    auto it = file_map.find(path);
    if (it == file_map.end()) return false;
    out = it->second;
    return true;
}

bool SpdkGlobal::FindDevice(const std::string& pci_addr, DevInfo& out)
{
    auto it = devices.find(pci_addr);
    if (it == devices.end()) return false;
    out = it->second;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  DirectSpdkRandomAccessFile  —  per-file handle
// ═══════════════════════════════════════════════════════════════════════════

thread_local std::unordered_map<struct spdk_nvme_ctrlr*, struct spdk_nvme_qpair*>
    DirectSpdkRandomAccessFile::tls_qpairs;

DirectSpdkRandomAccessFile::DirectSpdkRandomAccessFile(const std::string& path)
    : file_path(path)
{
    // Get LBA map path from config (default: /tmp/pixels_lba_map.json)
    std::string lba_map = "/tmp/pixels_lba_map.json";
    try {
        lba_map = ConfigFactory::Instance().getProperty("localfs.spdk.lba_map");
    } catch (...) {}

    // Initialize global SPDK state (idempotent via std::call_once)
    SpdkGlobal::Initialize(lba_map);

    // Strip "file://" prefix if present
    std::string real_path = path;
    if (real_path.substr(0, 7) == "file://") {
        real_path = real_path.substr(7);
    }
    file_path = real_path;

    // Look up the file in the LBA map
    SpdkGlobal::FileInfo fi;
    if (!SpdkGlobal::FindFile(real_path, fi)) {
        throw InvalidArgumentException(
            "DirectSpdkRandomAccessFile: file not in LBA map: " + real_path +
            "\nRun testcase/spdk/gen_lba_map.py to regenerate the map.");
    }

    // Look up the NVMe device
    SpdkGlobal::DevInfo dev;
    if (!SpdkGlobal::FindDevice(fi.pci, dev)) {
        throw InvalidArgumentException(
            "DirectSpdkRandomAccessFile: NVMe device not found for PCI " +
            fi.pci + " (file: " + real_path + ")");
    }

    ctrlr      = dev.ctrlr;
    ns         = dev.ns;
    block_size = dev.block_size;
    extents    = fi.extents;
    file_bytes = static_cast<long>(fi.file_bytes);

    // Get or create the per-thread qpair for this controller.
    // The qpair is keyed by ctrlr pointer so files on different controllers
    // get separate qpairs while files on the same controller share one.
    // This avoids the expensive admin-queue alloc/free per file.
    auto& cached_qpair = tls_qpairs[ctrlr];
    if (cached_qpair == nullptr) {
        cached_qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, nullptr, 0);
        if (!cached_qpair) {
            tls_qpairs.erase(ctrlr);
            throw std::runtime_error(
                "DirectSpdkRandomAccessFile: spdk_nvme_ctrlr_alloc_io_qpair "
                "failed for " + real_path);
        }
    }
    inst_qpair = cached_qpair;  // non-owning pointer into tls_qpairs
}

DirectSpdkRandomAccessFile::~DirectSpdkRandomAccessFile()
{
    // Cancel any pending ops that weren't completed.
    for (auto* op : pending_ops) {
        if (op->dma_buf) spdk_dma_free(op->dma_buf);
        delete op;
    }
    pending_ops.clear();

    // inst_qpair is owned by tls_qpairs; do not free it here.
    inst_qpair = nullptr;
}

// ── Static lifecycle ───────────────────────────────────────────────────────

void DirectSpdkRandomAccessFile::Initialize()
{
    // Per-instance qpairs are allocated in the constructor; no thread-level
    // setup is required.  This stub preserves API parity with
    // DirectUringRandomAccessFile.
}

void DirectSpdkRandomAccessFile::Reset()
{
    for (auto& [c, qp] : tls_qpairs) {
        if (qp) spdk_nvme_ctrlr_free_io_qpair(qp);
    }
    tls_qpairs.clear();
}

// ── PixelsRandomAccessFile interface ──────────────────────────────────────

void DirectSpdkRandomAccessFile::seek(long off)
{
    file_offset = off;
}

long DirectSpdkRandomAccessFile::length()
{
    return file_bytes;
}

// Core internal read: aligned SPDK read, result in `dst` starting at byte 0.
void DirectSpdkRandomAccessFile::syncRead(uint64_t byte_offset,
                                           int      byte_count,
                                           void*    dst)
{
    PROFILE_START("Spdk.SyncRead.Total");
    assert(ns     != nullptr);
    assert(ctrlr  != nullptr);
    assert(!extents.empty());

    assert(inst_qpair != nullptr);

    // Align down to block boundary
    uint64_t prefix        = byte_offset % block_size;
    uint64_t aligned_start = byte_offset - prefix;
    uint64_t aligned_len   = ((prefix + byte_count + block_size - 1) / block_size)
                              * block_size;
    uint64_t block_in_file = aligned_start / block_size;
    uint32_t n_blocks      = static_cast<uint32_t>(aligned_len / block_size);

    // Map file block → namespace LBA (single-extent files only for now)
    if (extents.empty() || block_in_file >= extents[0].lba_count) {
        throw std::runtime_error(
            "DirectSpdkRandomAccessFile::syncRead: offset out of range for " +
            file_path);
    }
    uint64_t lba = extents[0].lba_start + block_in_file;

    // Allocate DMA buffer
    PROFILE_START("Spdk.SyncRead.DmaAllocate");
    void* dma_buf = spdk_dma_malloc(aligned_len, block_size, nullptr);
    PROFILE_END("Spdk.SyncRead.DmaAllocate");
    if (!dma_buf) {
        throw std::runtime_error(
            "DirectSpdkRandomAccessFile::syncRead: spdk_dma_malloc failed "
            "(hugepages exhausted?)");
    }
    // Submit and wait synchronously
    struct SyncCtx { volatile bool done; int rc; };
    SyncCtx ctx = {false, 0};

    PROFILE_START("Spdk.SyncRead.Submit");
    int ret = spdk_nvme_ns_cmd_read(
        ns, inst_qpair, dma_buf, lba, n_blocks,
        [](void* arg, const struct spdk_nvme_cpl* cpl) {
            auto* c = static_cast<SyncCtx*>(arg);
            c->rc   = spdk_nvme_cpl_is_error(cpl) ? -1 : 0;
            c->done = true;
        }, &ctx, 0);
    PROFILE_END("Spdk.SyncRead.Submit");

    if (ret != 0) {
        spdk_dma_free(dma_buf);
        throw std::runtime_error(
            "DirectSpdkRandomAccessFile::syncRead: spdk_nvme_ns_cmd_read "
            "submit failed, ret=" + std::to_string(ret));
    }
    SpdkGlobal::RecordIoSubmit();

    PROFILE_START("Spdk.SyncRead.Poll");
    while (!ctx.done) {
        spdk_nvme_qpair_process_completions(inst_qpair, 0);
    }
    PROFILE_END("Spdk.SyncRead.Poll");
    if (ctx.rc != 0) {
        SpdkGlobal::RecordIoError();
        spdk_dma_free(dma_buf);
        throw std::runtime_error(
            "DirectSpdkRandomAccessFile::syncRead: NVMe read error at LBA " +
            std::to_string(lba));
    }
    SpdkGlobal::RecordIoComplete(aligned_len);

    // Copy the relevant bytes to destination
    PROFILE_START("Spdk.SyncRead.Copy");
    memcpy(dst, static_cast<char*>(dma_buf) + prefix, byte_count);
    PROFILE_END("Spdk.SyncRead.Copy");
    PROFILE_START("Spdk.SyncRead.DmaFree");
    spdk_dma_free(dma_buf);
    PROFILE_END("Spdk.SyncRead.DmaFree");
    PROFILE_END("Spdk.SyncRead.Total");
}

std::shared_ptr<ByteBuffer> DirectSpdkRandomAccessFile::readFully(int len)
{
    // Allocate output buffer (caller owns it)
    auto bb = std::make_shared<ByteBuffer>(len);
    syncRead(static_cast<uint64_t>(file_offset), len, bb->getPointer());
    file_offset += len;
    return bb;
}

std::shared_ptr<ByteBuffer> DirectSpdkRandomAccessFile::readFully(
    int len, std::shared_ptr<ByteBuffer> bb)
{
    syncRead(static_cast<uint64_t>(file_offset), len, bb->getPointer());
    file_offset += len;
    return std::make_shared<ByteBuffer>(*bb, 0, len);
}

long DirectSpdkRandomAccessFile::readLong()
{
    uint8_t buf[8];
    syncRead(static_cast<uint64_t>(file_offset), 8, buf);
    file_offset += 8;
    long val = 0;
    memcpy(&val, buf, 8);
    return val;
}

int DirectSpdkRandomAccessFile::readInt()
{
    uint8_t buf[4];
    syncRead(static_cast<uint64_t>(file_offset), 4, buf);
    file_offset += 4;
    int val = 0;
    memcpy(&val, buf, 4);
    return val;
}

char DirectSpdkRandomAccessFile::readChar()
{
    uint8_t buf[1];
    syncRead(static_cast<uint64_t>(file_offset), 1, buf);
    file_offset += 1;
    return static_cast<char>(buf[0]);
}

void DirectSpdkRandomAccessFile::close()
{
    // Ops are already cleaned up by destructor; nothing else to release here.
}

// ── Async read interface ───────────────────────────────────────────────────

std::shared_ptr<ByteBuffer>
DirectSpdkRandomAccessFile::readAsync(int length,
                                       std::shared_ptr<ByteBuffer> buffer,
                                       int /*index*/)
{
    PROFILE_START("Spdk.AsyncRead.Submit");
    assert(ns         != nullptr);
    assert(inst_qpair != nullptr);

    uint64_t byte_offset   = static_cast<uint64_t>(file_offset);
    uint64_t prefix        = byte_offset % block_size;
    uint64_t aligned_start = byte_offset - prefix;
    uint64_t aligned_len   = ((prefix + length + block_size - 1) / block_size)
                              * block_size;
    uint64_t block_in_file = aligned_start / block_size;
    uint32_t n_blocks      = static_cast<uint32_t>(aligned_len / block_size);
    uint64_t lba           = extents[0].lba_start + block_in_file;

    // Zero-copy path: the caller-supplied buffer must already be DMA-capable
    // (allocated via SpdkBufferPool / spdk_dma_malloc).  We read directly
    // into it without a temporary bounce buffer.
    if (aligned_len > buffer->size()) {
        throw std::runtime_error(
            "DirectSpdkRandomAccessFile::readAsync: DMA buffer too small "
            "(need " + std::to_string(aligned_len) +
            ", have " + std::to_string(buffer->size()) + ")");
    }

    auto* op      = new SpdkReadOp();
    op->out_buf   = buffer;
    op->dma_buf   = nullptr;   // not used in zero-copy path
    op->prefix    = prefix;
    op->data_len  = length;
    op->io_bytes  = aligned_len;
    op->done      = false;
    op->rc        = 0;

    int ret = spdk_nvme_ns_cmd_read(
        ns, inst_qpair, buffer->getPointer(), lba, n_blocks,
        [](void* arg, const struct spdk_nvme_cpl* cpl) {
            auto* o = static_cast<SpdkReadOp*>(arg);
            o->rc   = spdk_nvme_cpl_is_error(cpl) ? -1 : 0;
            o->done = true;
        }, op, 0);

    if (ret != 0) {
        delete op;
        throw std::runtime_error(
            "DirectSpdkRandomAccessFile::readAsync: spdk_nvme_ns_cmd_read "
            "submit failed, ret=" + std::to_string(ret));
    }
    SpdkGlobal::RecordIoSubmit();

    pending_ops.push_back(op);

    // Advance file position
    file_offset += length;

    // Return a zero-copy view starting at `prefix` bytes into the DMA buffer.
    auto result =
        std::make_shared<ByteBuffer>(*buffer, static_cast<uint32_t>(prefix), length);
    PROFILE_END("Spdk.AsyncRead.Submit");
    return result;
}

void DirectSpdkRandomAccessFile::readAsyncSubmit(int /*size*/)
{
    // SPDK enqueues commands to the hardware SQ immediately on ns_cmd_read;
    // no separate submit step is required.
}

void DirectSpdkRandomAccessFile::readAsyncComplete(int size)
{
    if (pending_ops.empty()) return;
    PROFILE_START("Spdk.AsyncComplete.Total");

    // Poll the CQ until every pending op has a completion callback
    PROFILE_START("Spdk.AsyncComplete.Poll");
    int remaining = static_cast<int>(pending_ops.size());
    while (remaining > 0) {
        int32_t completed = spdk_nvme_qpair_process_completions(inst_qpair, 0);
        if (completed < 0) {
            throw std::runtime_error(
                "DirectSpdkRandomAccessFile::readAsyncComplete: "
                "process_completions returned error " + std::to_string(completed));
        }
        remaining = 0;
        for (auto* op : pending_ops) {
            if (!op->done) remaining++;
        }
    }
    PROFILE_END("Spdk.AsyncComplete.Poll");

    // Zero-copy path: data is already in the caller's DMA buffer.
    // Just check for errors and release the op descriptors.
    PROFILE_START("Spdk.AsyncComplete.ReleaseOps");
    for (auto* op : pending_ops) {
        if (op->rc != 0) {
            SpdkGlobal::RecordIoError();
            for (auto* p : pending_ops) delete p;
            pending_ops.clear();
            throw std::runtime_error(
                "DirectSpdkRandomAccessFile::readAsyncComplete: "
                "NVMe read error in async op");
        }
        SpdkGlobal::RecordIoComplete(op->io_bytes);
        delete op;
    }
    pending_ops.clear();
    PROFILE_END("Spdk.AsyncComplete.ReleaseOps");
    PROFILE_END("Spdk.AsyncComplete.Total");
}

#endif // PIXELS_ENABLE_SPDK
