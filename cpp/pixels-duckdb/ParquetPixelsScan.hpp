/*
 * ParquetPixelsScan.hpp
 *
 * read_parquet_uring() — DuckDB table function that reads Parquet files using
 * Pixels' io_uring async I/O and BufferPool double-buffer mechanism.
 *
 * Key differences from a conventional synchronous Parquet reader:
 *  - Footer metadata pre-scanned in Bind phase (parallel, once per query)
 *  - Column chunks pre-read via io_uring into pre-allocated BufferPool buffers
 *  - Double-buffer: while decoding file N, io_uring fills file N+1
 *  - Arrow FileReader receives pre-cached FileMetaData → no footer re-read
 *  - Per-thread arrow::system_memory_pool() → eliminates global Arrow pool lock
 *
 * Configuration (from pixels-cpp.properties):
 *   localfs.enable.async.io = true/false   — io_uring vs pread
 *   localfs.async.lib = iouring            — must be "iouring" for async
 *   pixels.doublebuffer = true/false       — overlap IO with CPU decode
 *   localfs.block.size = 4096             — alignment for posix_memalign
 *
 * Modification to pixels-common:
 *   DirectUringRandomAccessFile.h: added public static GetRing() accessor so
 *   ParquetPixelsScan can submit io_uring SQEs directly to the thread-local ring
 *   without going through the instance API. This is read-only access; ring
 *   lifecycle is still managed by DirectUringRandomAccessFile.
 */

#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "duckdb/function/table/arrow/arrow_duck_schema.hpp"

#include <arrow/api.h>
#include <arrow/io/interfaces.h>
#include <arrow/c/bridge.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <parquet/properties.h>
#include <parquet/metadata.h>

#include <atomic>
#include <string>
#include <vector>
#include <tuple>
#include <cstring>

#include <liburing.h>

namespace duckdb {

// ---------------------------------------------------------------------------
// Column chunk metadata extracted from Parquet footer (Bind phase)
// ---------------------------------------------------------------------------
struct ParquetColMeta {
    int64_t offset;          // min(data_page_offset, dict_page_offset)
    int64_t compressed_size; // total_compressed_size
};

// Per-file footer metadata (pre-scanned once in Bind, shared read-only)
struct ParquetPixelsFileMeta {
    int64_t file_size;
    int64_t num_rows;           // total rows across ALL row groups
    int num_row_groups = 1;
    // rg_columns[rg_idx][col_idx] — per-row-group column chunk metadata
    vector<vector<ParquetColMeta>> rg_columns;
    // total_col_sizes[col_idx] = sum of compressed sizes across all row groups
    // Used to size raw_data buffers.
    vector<int64_t> total_col_sizes;
    std::shared_ptr<parquet::FileMetaData> file_metadata; // pre-parsed; passed to
                                                        // ParquetFileReader::Open to
                                                        // skip footer re-read in hot path
};

// ---------------------------------------------------------------------------
// RandomAccessFile backed by pre-loaded ByteBuffers (no disk I/O for column
// data; Arrow uses this to decode from BufferPool without going to disk).
// ---------------------------------------------------------------------------
class ParquetPixelsPreloadedFile : public arrow::io::RandomAccessFile {
public:
    // regions: (file_offset, ptr_into_BufferPool, size)
    // file_size: reported to Arrow via GetSize()
    ParquetPixelsPreloadedFile(int64_t file_size,
        std::vector<std::tuple<int64_t, const uint8_t*, int64_t>> regions);

    arrow::Result<int64_t> GetSize() override;
    arrow::Result<int64_t> ReadAt(int64_t pos, int64_t nbytes, void* out) override;
    arrow::Result<std::shared_ptr<arrow::Buffer>> ReadAt(int64_t pos, int64_t nbytes) override;

    arrow::Status Close() override;
    bool closed() const override;
    arrow::Result<int64_t> Tell() const override;
    arrow::Status Seek(int64_t position) override;
    arrow::Result<int64_t> Read(int64_t n, void* out) override;
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t n) override;

private:
    int64_t file_size_;
    int64_t pos_ = 0;
    bool closed_ = false;
    // sorted by file_offset for O(n) scan (n = projected columns, typically ≤ 105)
    std::vector<std::tuple<int64_t, const uint8_t*, int64_t>> regions_; // (offset, ptr, size)
};

// ---------------------------------------------------------------------------
// Bind data (shared across all scan threads, read-only after Bind)
// ---------------------------------------------------------------------------
struct ParquetPixelsBindData : public FunctionData {
    vector<string> files;
    vector<LogicalType> all_types;
    vector<string> all_names;
    std::shared_ptr<arrow::Schema> arrow_schema;
    arrow_column_map_t arrow_convert_data;

    // Pre-scanned footer metadata (one entry per file)
    vector<ParquetPixelsFileMeta> file_metas;
    // Max compressed_size per file-column-index (for BufferPool::Initialize sizing)
    vector<uint64_t> max_col_sizes; // length = n_all_cols
    int n_all_cols = 0;

    unique_ptr<FunctionData> Copy() const override;
    bool Equals(const FunctionData& other) const override;
};

// ---------------------------------------------------------------------------
// Global state (shared across scan threads)
// ---------------------------------------------------------------------------
struct ParquetPixelsGlobalState : public GlobalTableFunctionState {
    std::atomic<idx_t> next_item{0};
    std::atomic<idx_t> active_threads{0};
    std::atomic<bool> profile_printed{false};
    idx_t total_items = 0; // = files.size()
    idx_t max_threads = 1;
    idx_t MaxThreads() const override { return max_threads; }
};

// ---------------------------------------------------------------------------
// Local state (one per DuckDB scan thread)
// ---------------------------------------------------------------------------
struct ParquetPixelsLocalState : public LocalTableFunctionState {
    // Projection: file-column indices for projected columns
    vector<int>   col_ids;      // projected file-column indices
    vector<idx_t> duckdb_col_ids;
    int n_cols = 0;

    // Config (read once in InitLocal)
    bool cfg_double_buffer = false;
    bool cfg_async_io = false;

    // Self-contained double-buffer: raw_data[buf_idx][col_rank] → posix_memalign'd ptr
    // Independent of BufferPool/GlobalStaticBufferPool (Parquet column sizes differ
    // from pixel column sizes, so we can't reuse the pixels buffer pool).
    uint8_t*     raw_data[2][512]; // [buf_idx][col_rank], up to 512 projected cols
    size_t       raw_sizes[512];   // raw_sizes[col_rank] = allocated bytes for that col
    int          curr_buf = 0;     // index (0 or 1) of buffer holding current file
    int          next_buf = 1;     // index (0 or 1) for pre-fetch buffer

    // Self-contained io_uring ring (bypasses DirectUringRandomAccessFile's thread-local ring)
    struct io_uring pq_ring {};
    bool pq_ring_ok  = false;      // ring initialized
    bool pq_ring_reg = false;      // buffers registered with ring
    int  pending_cqes = 0;         // number of CQEs to wait for (set by SubmitColumnReads)

    // Current file being decoded
    idx_t curr_file_idx = idx_t(-1);
    // Next file being pre-fetched (io_uring SQEs submitted but not yet waited)
    idx_t next_file_idx = idx_t(-1);
    // fd for the next file's io_uring reads; -1 if no prefetch pending
    int next_fd = -1;

    // Arrow decoder for current file
    std::unique_ptr<parquet::arrow::FileReader> curr_arrow_reader;
    std::unique_ptr<arrow::RecordBatchReader>   curr_batch_reader;
    std::shared_ptr<arrow::RecordBatch>         curr_batch;
    int64_t curr_batch_offset = 0;

    // Arrow conversion state
    ArrowScanLocalState arrow_scan_state;

    explicit ParquetPixelsLocalState(ClientContext& ctx)
        : arrow_scan_state(make_uniq<ArrowArrayWrapper>(), ctx) {
        memset(raw_data, 0, sizeof(raw_data));
        memset(raw_sizes, 0, sizeof(raw_sizes));
    }

    ~ParquetPixelsLocalState();
};

// ---------------------------------------------------------------------------
// Main table function struct
// ---------------------------------------------------------------------------
struct ParquetPixelsScanFunction {
    static TableFunctionSet GetFunctionSet();

    static unique_ptr<FunctionData>
    Bind(ClientContext& context, TableFunctionBindInput& input,
         vector<LogicalType>& return_types, vector<string>& names);

    static unique_ptr<FunctionData>
    BindList(ClientContext& context, TableFunctionBindInput& input,
             vector<LogicalType>& return_types, vector<string>& names);

    static unique_ptr<GlobalTableFunctionState>
    InitGlobal(ClientContext& context, TableFunctionInitInput& input);

    static unique_ptr<LocalTableFunctionState>
    InitLocal(ExecutionContext& context, TableFunctionInitInput& input,
              GlobalTableFunctionState* gstate);

    static void Scan(ClientContext& context, TableFunctionInput& data, DataChunk& output);

    static double Progress(ClientContext& context,
                           const FunctionData* bind_data,
                           const GlobalTableFunctionState* gstate);

    static void Serialize(Serializer& serializer,
                          const optional_ptr<FunctionData> bind_data,
                          const TableFunction& function);

    static unique_ptr<FunctionData> Deserialize(Deserializer& deserializer,
                                                 TableFunction& function);

private:
    // Core double-buffer state machine (analogous to PixelsParallelStateNext).
    // is_init=true: called from InitLocal to start the pipeline.
    // is_init=false: called from Scan when current file is exhausted.
    // Returns false when there are no more files to process.
    static bool ParquetParallelStateNext(
        ClientContext& ctx,
        const ParquetPixelsBindData& bind,
        ParquetPixelsLocalState& lstate,
        ParquetPixelsGlobalState& gstate,
        bool is_init);
};

} // namespace duckdb
