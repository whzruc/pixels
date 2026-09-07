/*
 * ParquetPixelsScan.cpp
 *
 * Implementation of read_parquet_uring() — Parquet reading via io_uring
 * io_uring async I/O and BufferPool double-buffer mechanism.
 *
 * Design overview:
 *  Bind:       parallel footer scan → ParquetPixelsFileMeta per file
 *  InitLocal:  BufferPool::Initialize (pre-alloc 2×N column buffers)
 *              DirectUringRandomAccessFile::Initialize + RegisterBufferFromPool
 *              ParquetParallelStateNext(is_init=true) → first file ready
 *  Scan loop:  Arrow RecordBatch decode from pre-loaded BufferPool buffers
 *              ParquetParallelStateNext(is_init=false) at end of each file
 *
 * ParquetParallelStateNext double-buffer sequence:
 *   Init call:
 *     1. Open next_fd for file[0]
 *     2. SubmitColumnReads into buf[nextBufferIdx] (via io_uring or pread)
 *     3. WaitColumnReads (always sync for init — need data before returning)
 *     4. BufferPool::Switch → curr = file[0]'s buffer
 *     5. Close next_fd (io_uring done)
 *     6. BuildCurrArrowReader → FileReader from pre-loaded buffer
 *     7. If double_buffer: open next_fd for file[1], SubmitColumnReads
 *        (async: DON'T wait — overlaps with CPU decode of file[0])
 *
 *   Non-init call (file[N] exhausted):
 *     1. If async_io && next_fd >= 0: WaitColumnReads (file[N+1] now ready)
 *     2. BufferPool::Switch → curr = file[N+1]'s buffer
 *     3. Close next_fd
 *     4. BuildCurrArrowReader for file[N+1]
 *     5. If double_buffer: open next_fd for file[N+2], SubmitColumnReads (async)
 */

#include "ParquetPixelsScan.hpp"
#include "ArrowRandomAccessFile.hpp"
#include "profiler/TimeProfiler.h"
#include "utils/ConfigFactory.h"

#include <glob.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>

#include <arrow/buffer.h>
#include <arrow/memory_pool.h>
#include <arrow/compute/api.h>
#include <parquet/exception.h>

#define PQ_ARROW_CHECK_OK(expr)                                                    \
    do {                                                                           \
        auto _st = (expr);                                                         \
        if (!_st.ok())                                                             \
            throw std::runtime_error(std::string("Arrow/Parquet: ") + _st.ToString()); \
    } while (0)

namespace duckdb {

// ============================================================================
// ParquetPixelsPreloadedFile
// ============================================================================

ParquetPixelsPreloadedFile::ParquetPixelsPreloadedFile(
    int64_t file_size,
    std::vector<std::tuple<int64_t, const uint8_t*, int64_t>> regions)
    : file_size_(file_size), regions_(std::move(regions)) {
    std::sort(regions_.begin(), regions_.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
}

arrow::Result<int64_t> ParquetPixelsPreloadedFile::GetSize() { return file_size_; }

arrow::Result<int64_t>
ParquetPixelsPreloadedFile::ReadAt(int64_t pos, int64_t nbytes, void* out) {
    if (nbytes <= 0) return 0;
    for (const auto& region : regions_) {
        const auto& [r_off, r_ptr, r_size] = region;
        if (pos >= r_off && pos < r_off + r_size) {
            int64_t delta   = pos - r_off;
            int64_t to_copy = std::min(nbytes, r_size - delta);
            memcpy(out, r_ptr + delta, static_cast<size_t>(to_copy));
            return to_copy;
        }
    }
    // Should not be reached if arrow_metadata prevents footer re-reads.
    // If triggered, it means Arrow is reading a range not in our preloaded regions.
    return arrow::Status::IOError(
        "ParquetPixelsPreloadedFile: ReadAt(", pos, ", ", nbytes,
        ") not in any preloaded region");
}

arrow::Result<std::shared_ptr<arrow::Buffer>>
ParquetPixelsPreloadedFile::ReadAt(int64_t pos, int64_t nbytes) {
    ARROW_ASSIGN_OR_RAISE(auto buf,
        arrow::AllocateResizableBuffer(nbytes, arrow::system_memory_pool()));
    ARROW_ASSIGN_OR_RAISE(int64_t n, ReadAt(pos, nbytes, buf->mutable_data()));
    ARROW_RETURN_NOT_OK(buf->Resize(n));
    return std::shared_ptr<arrow::Buffer>(std::move(buf));
}

arrow::Status ParquetPixelsPreloadedFile::Close() { closed_ = true; return arrow::Status::OK(); }
bool ParquetPixelsPreloadedFile::closed() const { return closed_; }
arrow::Result<int64_t> ParquetPixelsPreloadedFile::Tell() const { return pos_; }
arrow::Status ParquetPixelsPreloadedFile::Seek(int64_t p) { pos_ = p; return arrow::Status::OK(); }

arrow::Result<int64_t> ParquetPixelsPreloadedFile::Read(int64_t n, void* out) {
    ARROW_ASSIGN_OR_RAISE(int64_t r, ReadAt(pos_, n, out));
    pos_ += r; return r;
}
arrow::Result<std::shared_ptr<arrow::Buffer>> ParquetPixelsPreloadedFile::Read(int64_t n) {
    ARROW_ASSIGN_OR_RAISE(auto buf, ReadAt(pos_, n));
    pos_ += buf->size(); return buf;
}

// ============================================================================
// ParquetPixelsLocalState destructor
// ============================================================================

ParquetPixelsLocalState::~ParquetPixelsLocalState() {
    curr_batch_reader.reset();
    curr_arrow_reader.reset();
    if (next_fd >= 0) { ::close(next_fd); next_fd = -1; }
    // Unregister and teardown io_uring ring
    if (pq_ring_reg) { io_uring_unregister_buffers(&pq_ring); pq_ring_reg = false; }
    if (pq_ring_ok)  { io_uring_queue_exit(&pq_ring); pq_ring_ok = false; }
    // Free self-managed column buffers
    for (int bi = 0; bi < 2; bi++)
        for (int ci = 0; ci < n_cols; ci++)
            if (raw_data[bi][ci]) { free(raw_data[bi][ci]); raw_data[bi][ci] = nullptr; }
}

// ============================================================================
// ParquetPixelsBindData
// ============================================================================

unique_ptr<FunctionData> ParquetPixelsBindData::Copy() const {
    auto copy = make_uniq<ParquetPixelsBindData>();
    copy->files              = files;
    copy->all_types          = all_types;
    copy->all_names          = all_names;
    copy->arrow_schema       = arrow_schema;
    copy->arrow_convert_data = arrow_convert_data;
    copy->file_metas         = file_metas;
    copy->max_col_sizes      = max_col_sizes;
    copy->n_all_cols         = n_all_cols;
    return copy;
}

bool ParquetPixelsBindData::Equals(const FunctionData& other) const {
    auto& o = other.Cast<ParquetPixelsBindData>();
    return files == o.files;
}

// ============================================================================
// Helper: build arrow_convert_data from Arrow schema
// ============================================================================
static arrow_column_map_t BuildArrowConvertDataPP(
    DBConfig& config, const std::shared_ptr<arrow::Schema>& schema) {
    ArrowSchema c_schema;
    PQ_ARROW_CHECK_OK(arrow::ExportSchema(*schema, &c_schema));
    arrow_column_map_t result;
    for (int i = 0; i < schema->num_fields(); i++) {
        ArrowSchema& child = *c_schema.children[i];
        auto atype = ArrowType::GetArrowLogicalType(config, child);
        result.emplace(static_cast<idx_t>(i), std::move(atype));
    }
    if (c_schema.release) c_schema.release(&c_schema);
    return result;
}

// ============================================================================
// Helper: scan footer of one file (called from parallel workers in Bind)
// ============================================================================
static ParquetPixelsFileMeta ScanFileMeta(const string& path, int n_all_cols) {
    // Get file size
    struct stat st;
    if (::stat(path.c_str(), &st) != 0)
        throw std::runtime_error("stat failed: " + path);
    int64_t file_size = st.st_size;

    // Open via Arrow adapter (pread-based, thread-safe)
    auto file_res = ArrowRandomAccessFile::Open(path);
    if (!file_res.ok())
        throw std::runtime_error("Open failed: " + path + ": " +
                                 file_res.status().ToString());
    auto file = file_res.ValueUnsafe();

    auto pq = parquet::ParquetFileReader::Open(file);
    auto meta = pq->metadata();

    ParquetPixelsFileMeta fm;
    fm.file_size      = file_size;
    fm.file_metadata  = meta; // keep shared_ptr to avoid re-parsing in hot path

    int n_rg = meta->num_row_groups();
    fm.num_rows = 0;
    fm.num_row_groups = n_rg;
    fm.rg_columns.resize(n_rg);
    fm.total_col_sizes.assign(n_all_cols, 0);
    for (int rg_idx = 0; rg_idx < n_rg; rg_idx++) {
        auto rg = meta->RowGroup(rg_idx);
        fm.num_rows += rg->num_rows();
        fm.rg_columns[rg_idx].resize(n_all_cols);
        for (int c = 0; c < n_all_cols; c++) {
            auto col_meta = rg->ColumnChunk(c);
            int64_t start = col_meta->data_page_offset();
            if (col_meta->has_dictionary_page())
                start = std::min(start, col_meta->dictionary_page_offset());
            fm.rg_columns[rg_idx][c].offset          = start;
            fm.rg_columns[rg_idx][c].compressed_size = col_meta->total_compressed_size();
            fm.total_col_sizes[c] += col_meta->total_compressed_size();
        }
    }
    return fm;
}

// ============================================================================
// io_uring helpers (self-contained: use lstate's own ring and raw_data buffers)
// ============================================================================

// Submit io_uring SQEs for all projected columns × all row groups into raw_data[buf_idx].
// Each row group's column data is appended sequentially in the buffer.
// iov_idx layout: buf_idx * n_cols + col_rank  (whole-column iov registered in InitLocal)
static void SubmitColumnReads(
    const ParquetPixelsFileMeta& fm,
    ParquetPixelsLocalState& lstate,
    int fd, int buf_idx) {

    PROFILE_START("Parquet.IO.Submit");
    int n_cols = lstate.n_cols;
    int n_sqe = 0;
    for (int rank = 0; rank < n_cols; rank++) {
        int col_id = lstate.col_ids[rank];
        int iov_idx = buf_idx * n_cols + rank;
        int64_t offset_in_buf = 0;
        for (const auto& rg_cols : fm.rg_columns) {
            const auto& cm = rg_cols[col_id];
            struct io_uring_sqe* sqe = io_uring_get_sqe(&lstate.pq_ring);
            if (!sqe) throw std::runtime_error("io_uring: SQ full");
            io_uring_prep_read_fixed(sqe, fd,
                                     lstate.raw_data[buf_idx][rank] + offset_in_buf,
                                     static_cast<unsigned>(cm.compressed_size),
                                     static_cast<uint64_t>(cm.offset),
                                     iov_idx);
            offset_in_buf += cm.compressed_size;
            n_sqe++;
        }
    }
    int ret = io_uring_submit(&lstate.pq_ring);
    if (ret != n_sqe)
        throw std::runtime_error("io_uring_submit: submitted " + std::to_string(ret) +
                                 ", expected " + std::to_string(n_sqe));
    lstate.pending_cqes = n_sqe;
    PROFILE_END("Parquet.IO.Submit");
}

// Wait for all pending CQEs (set by SubmitColumnReads).
static void WaitColumnReads(ParquetPixelsLocalState& lstate) {
    PROFILE_START("Parquet.IO.Wait");
    struct io_uring_cqe* cqe;
    for (int i = 0; i < lstate.pending_cqes; i++) {
        if (io_uring_wait_cqe_nr(&lstate.pq_ring, &cqe, 1) != 0)
            throw std::runtime_error("io_uring_wait_cqe failed");
        if (cqe->res < 0)
            throw std::runtime_error("io_uring read error: " + std::string(strerror(-cqe->res)));
        io_uring_cqe_seen(&lstate.pq_ring, cqe);
    }
    lstate.pending_cqes = 0;
    PROFILE_END("Parquet.IO.Wait");
}

// Synchronous pread: read all projected columns × all row groups into raw_data[buf_idx].
// Each row group's column data is appended sequentially in the buffer.
static void SyncReadColumns(
    const ParquetPixelsFileMeta& fm,
    ParquetPixelsLocalState& lstate,
    int fd, int buf_idx) {

    PROFILE_START("Parquet.IO.Pread");
    for (int rank = 0; rank < lstate.n_cols; rank++) {
        int col_id = lstate.col_ids[rank];
        int64_t offset_in_buf = 0;
        for (const auto& rg_cols : fm.rg_columns) {
            const auto& cm = rg_cols[col_id];
            ssize_t n = ::pread(fd, lstate.raw_data[buf_idx][rank] + offset_in_buf,
                                static_cast<size_t>(cm.compressed_size),
                                static_cast<off_t>(cm.offset));
            if (n < static_cast<ssize_t>(cm.compressed_size))
                throw std::runtime_error("pread short/failed (col " + std::to_string(col_id) +
                                         " rg " + std::to_string(&rg_cols - &fm.rg_columns[0]) +
                                         "): got " + std::to_string(n) + " bytes, " +
                                         strerror(errno));
            offset_in_buf += cm.compressed_size;
        }
    }
    PROFILE_END("Parquet.IO.Pread");
}

// Build curr_arrow_reader + curr_batch_reader from pre-loaded buffers.
// Uses arrow_metadata to skip footer re-read. Column data is served by
// ParquetPixelsPreloadedFile from lstate.raw_data[lstate.curr_buf].
static void BuildCurrArrowReader(
    const ParquetPixelsBindData& bind,
    ParquetPixelsLocalState& lstate) {

    PROFILE_START("Parquet.Decode.BuildReader");
    const auto& fm = bind.file_metas[lstate.curr_file_idx];

    // Build preloaded regions: one per (row_group, projected_column).
    // Each row group's column data is stored sequentially in raw_data[curr_buf][rank].
    std::vector<std::tuple<int64_t, const uint8_t*, int64_t>> regions;
    regions.reserve(static_cast<size_t>(lstate.n_cols) * fm.rg_columns.size());
    for (int rank = 0; rank < lstate.n_cols; rank++) {
        int col_id = lstate.col_ids[rank];
        int64_t offset_in_buf = 0;
        for (const auto& rg_cols : fm.rg_columns) {
            const auto& cm = rg_cols[col_id];
            regions.emplace_back(cm.offset,
                                 lstate.raw_data[lstate.curr_buf][rank] + offset_in_buf,
                                 cm.compressed_size);
            offset_in_buf += cm.compressed_size;
        }
    }

    // Create preloaded file (all column reads served from pre-loaded buffers; no disk I/O)
    auto preloaded = std::make_shared<ParquetPixelsPreloadedFile>(fm.file_size,
                                                                   std::move(regions));

    // Open ParquetFileReader with pre-cached metadata → no footer re-read
    auto props = parquet::default_reader_properties();
    auto pq = parquet::ParquetFileReader::Open(preloaded, props, fm.file_metadata);

    auto ar_props = parquet::ArrowReaderProperties(/*use_threads=*/false);
    ar_props.set_pre_buffer(false); // we already pre-loaded; disable Arrow's own pre-buffer

    lstate.curr_batch_reader.reset(); // release old batch reader BEFORE old arrow reader
    lstate.curr_arrow_reader.reset();
    PQ_ARROW_CHECK_OK(parquet::arrow::FileReader::Make(
        arrow::system_memory_pool(), std::move(pq), ar_props,
        &lstate.curr_arrow_reader));

    // GetRecordBatchReader for ALL row groups + projected columns
    std::vector<int> all_row_groups;
    all_row_groups.reserve(fm.rg_columns.size());
    for (int rg = 0; rg < static_cast<int>(fm.rg_columns.size()); rg++)
        all_row_groups.push_back(rg);

    auto res = lstate.curr_arrow_reader->GetRecordBatchReader(all_row_groups, lstate.col_ids);
    if (!res.ok())
        throw std::runtime_error(
            "GetRecordBatchReader failed: " + res.status().ToString());

    lstate.curr_batch_reader = std::move(res).ValueOrDie();
    lstate.curr_batch.reset();
    lstate.curr_batch_offset = 0;
    PROFILE_END("Parquet.Decode.BuildReader");
}

// ============================================================================
// ParquetParallelStateNext — double-buffer state machine
// ============================================================================

bool ParquetPixelsScanFunction::ParquetParallelStateNext(
    ClientContext& /*ctx*/,
    const ParquetPixelsBindData& bind,
    ParquetPixelsLocalState& lstate,
    ParquetPixelsGlobalState& gstate,
    bool is_init) {

    PROFILE_START("Parquet.StateTransition.Total");
    if (is_init) {
        // ----------------------------------------------------------------
        // Initialization: set up pipeline for first file
        // ----------------------------------------------------------------
        idx_t first = gstate.next_item.fetch_add(1, std::memory_order_relaxed);
        if (first >= gstate.total_items) {
            PROFILE_END("Parquet.StateTransition.Total");
            return false;
        }

        int fd = ::open(bind.files[first].c_str(), O_RDONLY);
        if (fd < 0)
            throw IOException("read_parquet_uring: open '%s': %s",
                              bind.files[first], strerror(errno));
        lstate.next_fd = fd;
        lstate.next_file_idx = first;

        // Read first file into next_buf (sync: we need it before returning)
        if (lstate.cfg_async_io) {
            SubmitColumnReads(bind.file_metas[first], lstate, fd, lstate.next_buf);
            WaitColumnReads(lstate);
        } else {
            SyncReadColumns(bind.file_metas[first], lstate, fd, lstate.next_buf);
        }

        // Switch: next_buf → curr_buf
        std::swap(lstate.curr_buf, lstate.next_buf);

        lstate.curr_file_idx = lstate.next_file_idx;
        ::close(lstate.next_fd);
        lstate.next_fd       = -1;
        lstate.next_file_idx = idx_t(-1);

        BuildCurrArrowReader(bind, lstate);

        // Prefetch second file (async, overlaps with decode of first file)
        if (lstate.cfg_double_buffer) {
            idx_t second = gstate.next_item.fetch_add(1, std::memory_order_relaxed);
            if (second < gstate.total_items) {
                int fd2 = ::open(bind.files[second].c_str(), O_RDONLY);
                if (fd2 < 0)
                    throw IOException("read_parquet_uring: open '%s': %s",
                                      bind.files[second], strerror(errno));
                lstate.next_fd       = fd2;
                lstate.next_file_idx = second;
                if (lstate.cfg_async_io) {
                    SubmitColumnReads(bind.file_metas[second], lstate, fd2, lstate.next_buf);
                    // Don't wait: overlaps with CPU decode of file[0]
                } else {
                    SyncReadColumns(bind.file_metas[second], lstate, fd2, lstate.next_buf);
                }
            }
        }
        PROFILE_END("Parquet.StateTransition.Total");
        return true;
    }

    // ----------------------------------------------------------------
    // Non-init: current file exhausted, advance to prefetched file
    // ----------------------------------------------------------------

    // Step 1: Wait for any pending async prefetch
    if (lstate.cfg_async_io && lstate.next_fd >= 0) {
        WaitColumnReads(lstate);
    }

    // Step 2: If no prefetch submitted, get next file synchronously now
    if (lstate.next_fd < 0) {
        if (!lstate.cfg_double_buffer) {
            idx_t item = gstate.next_item.fetch_add(1, std::memory_order_relaxed);
            if (item >= gstate.total_items) {
                PROFILE_END("Parquet.StateTransition.Total");
                return false;
            }
            int fd = ::open(bind.files[item].c_str(), O_RDONLY);
            if (fd < 0)
                throw IOException("read_parquet_uring: open '%s': %s",
                                  bind.files[item], strerror(errno));
            lstate.next_fd       = fd;
            lstate.next_file_idx = item;
            if (lstate.cfg_async_io) {
                SubmitColumnReads(bind.file_metas[item], lstate, fd, lstate.next_buf);
                WaitColumnReads(lstate);
            } else {
                SyncReadColumns(bind.file_metas[item], lstate, fd, lstate.next_buf);
            }
        } else {
            PROFILE_END("Parquet.StateTransition.Total");
            return false; // double-buffer exhausted
        }
    }

    // Step 3: Switch buffers
    std::swap(lstate.curr_buf, lstate.next_buf);

    lstate.curr_file_idx = lstate.next_file_idx;
    ::close(lstate.next_fd);
    lstate.next_fd       = -1;
    lstate.next_file_idx = idx_t(-1);

    BuildCurrArrowReader(bind, lstate);

    // Step 6: Prefetch next file (if double-buffer)
    if (lstate.cfg_double_buffer) {
        idx_t prefetch = gstate.next_item.fetch_add(1, std::memory_order_relaxed);
        if (prefetch < gstate.total_items) {
            int fd = ::open(bind.files[prefetch].c_str(), O_RDONLY);
            if (fd < 0)
                throw IOException("read_parquet_uring: open '%s': %s",
                                  bind.files[prefetch], strerror(errno));
            lstate.next_fd       = fd;
            lstate.next_file_idx = prefetch;
            if (lstate.cfg_async_io) {
                SubmitColumnReads(bind.file_metas[prefetch], lstate, fd, lstate.next_buf);
                // Don't wait: overlaps with CPU decode of current file
            } else {
                SyncReadColumns(bind.file_metas[prefetch], lstate, fd, lstate.next_buf);
            }
        }
    }

    PROFILE_END("Parquet.StateTransition.Total");
    return true;
}

// ============================================================================
// Bind
// ============================================================================

unique_ptr<FunctionData>
ParquetPixelsScanFunction::Bind(ClientContext& context,
                                 TableFunctionBindInput& input,
                                 vector<LogicalType>& return_types,
                                 vector<string>& names) {
    auto result = make_uniq<ParquetPixelsBindData>();

    // --- parse + glob file arguments ------------------------------------------
    if (input.inputs.empty())
        throw InvalidInputException("read_parquet_uring: at least one file path required");

    for (auto& val : input.inputs) {
        auto path = val.GetValue<string>();
        if (path.find('*') != string::npos || path.find('?') != string::npos) {
            glob_t gr;
            int rc = ::glob(path.c_str(), GLOB_TILDE | GLOB_NOSORT, nullptr, &gr);
            if (rc == GLOB_NOMATCH) { globfree(&gr); continue; }
            if (rc != 0) { globfree(&gr); throw IOException("glob failed: %s", path); }
            for (size_t i = 0; i < gr.gl_pathc; i++)
                result->files.emplace_back(gr.gl_pathv[i]);
            globfree(&gr);
        } else {
            result->files.push_back(path);
        }
    }
    if (result->files.empty())
        throw IOException("read_parquet_uring: no files found");
    std::sort(result->files.begin(), result->files.end());

    // --- get schema from first file -------------------------------------------
    {
        auto file_res = ArrowRandomAccessFile::Open(result->files[0]);
        if (!file_res.ok())
            throw IOException("read_parquet_uring: cannot open '%s': %s",
                              result->files[0], file_res.status().ToString());
        auto pq = parquet::ParquetFileReader::Open(file_res.ValueUnsafe());
        result->n_all_cols = pq->metadata()->num_columns();

        unique_ptr<parquet::arrow::FileReader> ar;
        auto props = parquet::ArrowReaderProperties(/*use_threads=*/false);
        PQ_ARROW_CHECK_OK(parquet::arrow::FileReader::Make(
            arrow::default_memory_pool(), std::move(pq), props, &ar));
        PQ_ARROW_CHECK_OK(ar->GetSchema(&result->arrow_schema));
    }

    auto& config = DBConfig::GetConfig(context);
    result->arrow_convert_data = BuildArrowConvertDataPP(config, result->arrow_schema);
    for (int i = 0; i < result->arrow_schema->num_fields(); i++) {
        names.push_back(result->arrow_schema->field(i)->name());
        auto it = result->arrow_convert_data.find(static_cast<idx_t>(i));
        if (it == result->arrow_convert_data.end())
            throw InternalException("read_parquet_uring: missing type for col %d", i);
        return_types.push_back(it->second->GetDuckType(true));
    }
    result->all_types = return_types;
    result->all_names = names;

    // --- parallel footer scan ------------------------------------------------
    // Scan all files' footers to get column chunk (offset, size) and
    // pre-parse FileMetaData (so hot path skips footer re-read).
    int n_files = static_cast<int>(result->files.size());
    int n_cols  = result->n_all_cols;
    result->file_metas.resize(n_files);
    result->max_col_sizes.assign(n_cols, 0);

    {
        int n_workers = std::min(n_files, 32);
        std::atomic<int> cursor{0};
        std::vector<std::string> errors(n_workers);
        std::vector<std::thread> workers;
        workers.reserve(n_workers);

        for (int t = 0; t < n_workers; t++) {
            workers.emplace_back([&, t]() {
                try {
                    for (int fi = cursor.fetch_add(1); fi < n_files;
                         fi = cursor.fetch_add(1)) {
                        result->file_metas[fi] =
                            ScanFileMeta(result->files[fi], n_cols);
                    }
                } catch (const std::exception& e) {
                    errors[t] = e.what();
                }
            });
        }
        for (auto& w : workers) w.join();
        for (auto& err : errors)
            if (!err.empty()) throw IOException("read_parquet_uring footer scan: %s", err);

        // Compute max total column size across all files (summed over all row groups)
        for (const auto& fm : result->file_metas)
            for (int c = 0; c < n_cols; c++)
                result->max_col_sizes[c] =
                    std::max(result->max_col_sizes[c],
                             static_cast<uint64_t>(fm.total_col_sizes[c]));
    }

    return result;
}

// ============================================================================
// BindList — LIST(VARCHAR) overload
// ============================================================================

unique_ptr<FunctionData>
ParquetPixelsScanFunction::BindList(ClientContext& context,
                                     TableFunctionBindInput& input,
                                     vector<LogicalType>& return_types,
                                     vector<string>& names) {
    if (input.inputs.empty() || input.inputs[0].IsNull())
        throw InvalidInputException("read_parquet_uring: file list is null/empty");
    vector<Value> path_values;
    for (auto& child : ListValue::GetChildren(input.inputs[0]))
        path_values.push_back(child);
    TableFunctionBindInput path_input(path_values, input.named_parameters,
                                      input.input_table_types, input.input_table_names,
                                      input.info, input.binder,
                                      input.table_function, input.ref);
    return Bind(context, path_input, return_types, names);
}

// ============================================================================
// InitGlobal
// ============================================================================

unique_ptr<GlobalTableFunctionState>
ParquetPixelsScanFunction::InitGlobal(ClientContext& context,
                                       TableFunctionInitInput& input) {
    auto& bind = input.bind_data->Cast<ParquetPixelsBindData>();
    ::TimeProfiler::Instance().Reset();
    auto gstate = make_uniq<ParquetPixelsGlobalState>();
    gstate->total_items = static_cast<idx_t>(bind.files.size());
    gstate->max_threads = MaxValue<idx_t>(
        1, MinValue<idx_t>(gstate->total_items,
                           static_cast<idx_t>(
                               TaskScheduler::GetScheduler(context).NumberOfThreads())));
    gstate->active_threads.store(gstate->max_threads, std::memory_order_relaxed);
    return gstate;
}

// ============================================================================
// InitLocal
// ============================================================================

unique_ptr<LocalTableFunctionState>
ParquetPixelsScanFunction::InitLocal(ExecutionContext& exec_ctx,
                                      TableFunctionInitInput& input,
                                      GlobalTableFunctionState* gstate_p) {
    auto& bind   = input.bind_data->Cast<ParquetPixelsBindData>();
    auto& gstate = gstate_p->Cast<ParquetPixelsGlobalState>();
    auto& ctx    = exec_ctx.client;

    auto lstate = make_uniq<ParquetPixelsLocalState>(ctx);
    lstate->duckdb_col_ids         = input.column_ids;
    lstate->arrow_scan_state.column_ids = input.column_ids;

    // Build projected column list (skip row_id markers)
    for (auto cid : input.column_ids) {
        if (!IsRowIdColumnId(cid))
            lstate->col_ids.push_back(static_cast<int>(cid));
    }
    // COUNT(*) with no projected columns: read column 0 for row counting
    if (lstate->col_ids.empty())
        lstate->col_ids.push_back(0);
    lstate->n_cols = static_cast<int>(lstate->col_ids.size());

    // --- read configuration ---------------------------------------------------
    try {
        lstate->cfg_async_io =
            ConfigFactory::Instance().boolCheckProperty("localfs.enable.async.io") &&
            ConfigFactory::Instance().getProperty("localfs.async.lib") == "iouring";
    } catch (...) { lstate->cfg_async_io = false; }

    try {
        lstate->cfg_double_buffer =
            ConfigFactory::Instance().getProperty("pixels.doublebuffer") == "true";
    } catch (...) { lstate->cfg_double_buffer = false; }

    // --- allocate self-managed double-buffers (2 × n_cols, posix_memalign'd) ---
    // We bypass BufferPool/GlobalStaticBufferPool because those are sized for
    // pixel-format columns, not Parquet columns (sizes differ per format).
    {
        PROFILE_START("Parquet.InitLocal.AllocateBuffers");
        const size_t block = 4096; // align to page size
        for (int rank = 0; rank < lstate->n_cols; rank++) {
            int col_id = lstate->col_ids[rank];
            size_t sz = static_cast<size_t>(bind.max_col_sizes[col_id]);
            // Round up to block size for io_uring compatibility
            sz = (sz + block - 1) & ~(block - 1);
            if (sz == 0) sz = block;
            lstate->raw_sizes[rank] = sz;
            for (int bi = 0; bi < 2; bi++) {
                void* p = nullptr;
                if (posix_memalign(&p, block, sz) != 0)
                    throw IOException("read_parquet_uring: posix_memalign failed for col %d", col_id);
                lstate->raw_data[bi][rank] = static_cast<uint8_t*>(p);
            }
        }
        PROFILE_END("Parquet.InitLocal.AllocateBuffers");
    }

    // --- initialize self-managed io_uring ring and register buffers -----------
    if (lstate->cfg_async_io) {
        PROFILE_START("Parquet.InitLocal.SetupIoUring");
        if (io_uring_queue_init(4096, &lstate->pq_ring, 0) != 0)
            throw IOException("read_parquet_uring: io_uring_queue_init failed: %s",
                              strerror(errno));
        lstate->pq_ring_ok = true;

        // Build iovec layout: iovecs[buf_idx * n_cols + col_rank]
        std::vector<struct iovec> iovs(2 * lstate->n_cols);
        for (int bi = 0; bi < 2; bi++)
            for (int rank = 0; rank < lstate->n_cols; rank++) {
                iovs[bi * lstate->n_cols + rank].iov_base = lstate->raw_data[bi][rank];
                iovs[bi * lstate->n_cols + rank].iov_len  = lstate->raw_sizes[rank];
            }
        if (io_uring_register_buffers(&lstate->pq_ring, iovs.data(),
                                      static_cast<unsigned>(iovs.size())) != 0)
            throw IOException("read_parquet_uring: io_uring_register_buffers failed: %s",
                              strerror(errno));
        lstate->pq_ring_reg = true;
        PROFILE_END("Parquet.InitLocal.SetupIoUring");
    }

    // --- start the double-buffer pipeline -------------------------------------
    if (!ParquetParallelStateNext(ctx, bind, *lstate, gstate, /*is_init=*/true))
        return nullptr; // no files assigned to this thread

    return lstate;
}

// ============================================================================
// Export RecordBatch to Arrow C ABI.
// ============================================================================
static void ExportBatch(ParquetPixelsLocalState& lstate,
                        const std::shared_ptr<arrow::RecordBatch>& batch) {
    // Flatten dictionary columns before exporting them to DuckDB.
    auto flat_schema = batch->schema();
    std::vector<std::shared_ptr<arrow::Array>> flat_cols;
    flat_cols.reserve(batch->num_columns());
    bool needs_flatten = false;
    for (int c = 0; c < batch->num_columns(); c++) {
        auto col = batch->column(c);
        if (col->type_id() == arrow::Type::DICTIONARY) {
            auto dict_arr  = std::static_pointer_cast<arrow::DictionaryArray>(col);
            auto val_type  = std::static_pointer_cast<arrow::DictionaryType>(
                                 col->type())->value_type();
            auto cast_res  = arrow::compute::Cast(*dict_arr, val_type);
            if (cast_res.ok()) {
                flat_cols.push_back(cast_res.ValueUnsafe());
                needs_flatten = true;
            } else {
                flat_cols.push_back(col);
            }
        } else {
            flat_cols.push_back(col);
        }
    }
    std::shared_ptr<arrow::RecordBatch> export_batch = batch;
    if (needs_flatten) {
        export_batch = arrow::RecordBatch::Make(flat_schema, batch->num_rows(), flat_cols);
    }

    auto& chunk_wrapper = lstate.arrow_scan_state.chunk;
    if (chunk_wrapper->arrow_array.release)
        chunk_wrapper->arrow_array.release(&chunk_wrapper->arrow_array);
    PQ_ARROW_CHECK_OK(arrow::ExportRecordBatch(*export_batch,
                                               &chunk_wrapper->arrow_array, nullptr));
}

// ============================================================================
// Scan
// ============================================================================

void ParquetPixelsScanFunction::Scan(ClientContext& context,
                                      TableFunctionInput& data_p,
                                      DataChunk& output) {
    if (!data_p.local_state) {
        output.SetCardinality(0);
        return;
    }
    auto& lstate = data_p.local_state->Cast<ParquetPixelsLocalState>();
    auto& gstate = data_p.global_state->Cast<ParquetPixelsGlobalState>();
    auto& bind   = data_p.bind_data->Cast<ParquetPixelsBindData>();
    PROFILE_START("Parquet.Scan.Total");

    while (true) {
        if (!lstate.curr_batch_reader) {
            output.SetCardinality(0);
            PROFILE_END("Parquet.Scan.Total");
            return;
        }

        // Fetch next RecordBatch if current is exhausted
        bool need_next = (!lstate.curr_batch ||
                          lstate.curr_batch_offset >= lstate.curr_batch->num_rows());
        if (need_next) {
            std::shared_ptr<arrow::RecordBatch> batch;
            PROFILE_START("Parquet.Decode.ReadNext");
            auto st = lstate.curr_batch_reader->ReadNext(&batch);
            PROFILE_END("Parquet.Decode.ReadNext");
            if (!st.ok())
                throw IOException("read_parquet_uring: ReadNext: %s", st.ToString());
            if (!batch) {
                // All row groups exhausted → advance to next file
                if (!ParquetParallelStateNext(context, bind, lstate, gstate,
                                              /*is_init=*/false)) {
                    output.SetCardinality(0);
                    PROFILE_END("Parquet.Scan.Total");
                    ::TimeProfiler::Instance().Collect();
                    auto remaining =
                        gstate.active_threads.fetch_sub(1, std::memory_order_acq_rel);
                    if (remaining == 1 &&
                        !gstate.profile_printed.exchange(true, std::memory_order_acq_rel)) {
                        // The current profiler exposes a global Print() API;
                        // keep the archived instrumentation while remaining
                        // compatible with that API.
                        ::TimeProfiler::Instance().Print();
                    }
                    return;
                }
                continue; // retry with new batch_reader
            }
            lstate.curr_batch        = batch;
            lstate.curr_batch_offset = 0;
            PROFILE_START("Parquet.Decode.ExportBatch");
            ExportBatch(lstate, batch);
            PROFILE_END("Parquet.Decode.ExportBatch");
            lstate.arrow_scan_state.chunk_offset = 0;
        }
        break;
    }

    idx_t remaining = static_cast<idx_t>(
        lstate.curr_batch->num_rows() - lstate.curr_batch_offset);
    idx_t out_size = MinValue<idx_t>(STANDARD_VECTOR_SIZE, remaining);
    output.SetCardinality(out_size);

    lstate.arrow_scan_state.chunk_offset =
        static_cast<idx_t>(lstate.curr_batch_offset);

    PROFILE_START("Parquet.Convert.ArrowToDuckDB");
    ArrowTableFunction::ArrowToDuckDB(lstate.arrow_scan_state,
                                      bind.arrow_convert_data,
                                      output, /*start=*/0,
                                      /*arrow_scan_is_projected=*/true);
    PROFILE_END("Parquet.Convert.ArrowToDuckDB");

    lstate.curr_batch_offset += static_cast<int64_t>(out_size);
    PROFILE_END("Parquet.Scan.Total");
}

// ============================================================================
// Progress
// ============================================================================

double ParquetPixelsScanFunction::Progress(ClientContext&,
                                            const FunctionData*,
                                            const GlobalTableFunctionState* gp) {
    auto& gstate = gp->Cast<ParquetPixelsGlobalState>();
    if (gstate.total_items == 0) return 100.0;
    idx_t done = gstate.next_item.load(std::memory_order_relaxed);
    return 100.0 * std::min(1.0, static_cast<double>(done) /
                                     static_cast<double>(gstate.total_items));
}

// ============================================================================
// Serialize / Deserialize (for CREATE VIEW persistence)
// ============================================================================

void ParquetPixelsScanFunction::Serialize(Serializer& serializer,
                                           const optional_ptr<FunctionData> bind_p,
                                           const TableFunction&) {
    auto& bind = bind_p->Cast<ParquetPixelsBindData>();
    serializer.WriteProperty(100, "files", bind.files);
}

unique_ptr<FunctionData>
ParquetPixelsScanFunction::Deserialize(Deserializer& deserializer,
                                        TableFunction&) {
    auto& ctx    = deserializer.Get<ClientContext&>();
    auto result  = make_uniq<ParquetPixelsBindData>();
    deserializer.ReadProperty(100, "files", result->files);
    if (result->files.empty())
        throw IOException("read_parquet_uring: deserialized file list empty");

    // Rebuild schema from first file
    auto file_res = ArrowRandomAccessFile::Open(result->files[0]);
    if (!file_res.ok())
        throw IOException("read_parquet_uring: cannot open '%s'", result->files[0]);
    auto pq = parquet::ParquetFileReader::Open(file_res.ValueUnsafe());
    result->n_all_cols = pq->metadata()->num_columns();
    unique_ptr<parquet::arrow::FileReader> ar;
    auto props = parquet::ArrowReaderProperties(false);
    PQ_ARROW_CHECK_OK(parquet::arrow::FileReader::Make(
        arrow::default_memory_pool(), std::move(pq), props, &ar));
    PQ_ARROW_CHECK_OK(ar->GetSchema(&result->arrow_schema));

    auto& config = DBConfig::GetConfig(ctx);
    result->arrow_convert_data = BuildArrowConvertDataPP(config, result->arrow_schema);
    for (int i = 0; i < result->arrow_schema->num_fields(); i++) {
        result->all_names.push_back(result->arrow_schema->field(i)->name());
        auto it = result->arrow_convert_data.find(static_cast<idx_t>(i));
        if (it != result->arrow_convert_data.end())
            result->all_types.push_back(it->second->GetDuckType(true));
    }

    // Re-scan footers (needed for max_col_sizes + file_metas)
    int n_files = static_cast<int>(result->files.size());
    int n_cols  = result->n_all_cols;
    result->file_metas.resize(n_files);
    result->max_col_sizes.assign(n_cols, 0);
    std::atomic<int> cursor{0};
    int n_workers = std::min(n_files, 16);
    vector<std::thread> workers;
    workers.reserve(n_workers);
    for (int t = 0; t < n_workers; t++) {
        workers.emplace_back([&]() {
            for (int fi = cursor.fetch_add(1); fi < n_files; fi = cursor.fetch_add(1))
                result->file_metas[fi] = ScanFileMeta(result->files[fi], n_cols);
        });
    }
    for (auto& w : workers) w.join();
    for (const auto& fm : result->file_metas)
        for (int c = 0; c < n_cols; c++)
            result->max_col_sizes[c] =
                std::max(result->max_col_sizes[c],
                         static_cast<uint64_t>(fm.total_col_sizes[c]));

    return result;
}

// ============================================================================
// GetFunctionSet
// ============================================================================

TableFunctionSet ParquetPixelsScanFunction::GetFunctionSet() {
    TableFunctionSet set("read_parquet_uring");

    // Single string argument: read_parquet_uring('/path/to/file.parquet')
    {
        TableFunction f("read_parquet_uring", {LogicalType::VARCHAR},
                        Scan, Bind, InitGlobal, InitLocal);
        f.projection_pushdown  = true;
        f.table_scan_progress  = Progress;
        f.serialize            = Serialize;
        f.deserialize          = Deserialize;
        set.AddFunction(f);
    }

    // List argument: read_parquet_uring(['/path/a.parquet', '/path/b.parquet'])
    {
        TableFunction f("read_parquet_uring",
                        {LogicalType::LIST(LogicalType::VARCHAR)},
                        Scan, BindList, InitGlobal, InitLocal);
        f.projection_pushdown  = true;
        f.table_scan_progress  = Progress;
        f.serialize            = Serialize;
        f.deserialize          = Deserialize;
        set.AddFunction(f);
    }

    return set;
}

} // namespace duckdb
