# Parquet Reader Performance Test Implementation

## Overview

This document describes the implementation of a parquet reader performance test that mirrors the structure and approach of `tests/reader-performance-test/reader_performance_test.cpp`. The test focuses on pure I/O performance without data conversion or processing.

## Implementation Details

### File: `parquet_reader_performance_test.cpp`

The implementation follows the same pattern as the Pixels reader performance test:

#### Key Components

1. **File Sorting by Number**: Files are sorted by their numeric suffix to enable multi-SSD parallelism
2. **Multi-threaded Reading**: Supports concurrent reading with configurable thread count
3. **Global/Local State Management**: Same thread-safe state management pattern as Pixels test
4. **Pure I/O Testing**: Uses `pread()` for raw I/O without data processing

#### Core Functions

- `parseConfig()`: Reads configuration from properties file
- `extractFileNumber()`: Extracts file numbers for sorting
- `updateLocalState()`: Thread-safe work distribution
- `readRowGroupsWithPread()`: Low-level I/O using pread
- `scanImplementation()`: Per-thread scan loop

### Reading Strategy

The test uses a two-phase approach:

1. **Metadata Phase**: Uses Apache Arrow Parquet library to:
   - Parse file metadata
   - Get row group count and locations
   - Extract column chunk offsets and sizes

2. **I/O Phase**: Uses raw `pread()` to:
   - Read data directly from file descriptors
   - Support Direct I/O with O_DIRECT flag
   - Perform aligned reads for Direct I/O

### Direct I/O Support

When `localfs.enable.direct.io=true`:

- Opens files with `O_DIRECT` flag
- Aligns buffer addresses to 4096 bytes using `posix_memalign()`
- Aligns read offsets and sizes to 4096 bytes
- Bypasses kernel page cache for maximum throughput

## Configuration

The test reads from a properties file with the following keys:

```properties
test.data.files=/path/to/parquet/*.parquet      # Glob patterns, comma-separated
test.threads=12                                   # Number of threads (-1 = all cores)
localfs.enable.direct.io=true                    # Enable Direct I/O
test.rg.start=0                                   # First row group to read
test.rg.count=-1                                  # Row groups to read (-1 = all)
test.query.id=1                                   # Query ID for tracking
test.verify.data=false                            # Data verification (not yet implemented)
```

## Key Differences from Pixels Test

| Aspect | Pixels Test | Parquet Test |
|--------|-------------|--------------|
| File Format | Pixels (.pxl) | Parquet (.parquet) |
| Metadata Reading | PixelsFooterCache | Apache Arrow Parquet API |
| I/O Method | pread/io_uring/async | pread with O_DIRECT |
| Column Vector | ColumnVector | Raw buffers |
| Record Reader | PixelsRecordReader | Direct pread |
| Data Structure | Row groups + columns | Row groups + column chunks |

## Similarities with Pixels Test

Both tests share:

1. **File Sorting**: By numeric suffix for SSD interleaving
2. **Thread Pool**: Configurable worker threads
3. **Work Distribution**: Lock-based global state
4. **Direct I/O**: O_DIRECT support with alignment
5. **Configuration**: Properties file parsing
6. **Metrics**: Row count, throughput, elapsed time

## Building

### From Project Root

The test is integrated into the main CMake build:

```bash
cd /home/whz/test/pixels/cpp
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build Arrow/Parquet first (takes time on first build)
make arrow_static parquet_static -j$(nproc)

# Build the test
make parquet_reader_performance_test -j$(nproc)
```

### Using Build Script

```bash
cd tests/parquet-reader-performance-test
./build.sh
```

## Running

### Basic Execution

```bash
cd /home/whz/test/pixels/cpp
./build/tests/parquet_reader_performance_test \
    tests/parquet-reader-performance-test/config.properties
```

### With Performance Monitoring

```bash
# With perf stat
perf stat -e cycles,instructions,cache-misses,cache-references \
    ./build/tests/parquet_reader_performance_test \
    tests/parquet-reader-performance-test/config.properties

# With perf record (for flamegraphs)
perf record -F 99 -g -- \
    ./build/tests/parquet_reader_performance_test \
    tests/parquet-reader-performance-test/config.properties
```

## Example Output

```
Loading configuration from: tests/parquet-reader-performance-test/config.properties
Configuration loaded:
  Files to read: 100
  Threads: 12
  Direct I/O: enabled
  Row group range: [0, all)
  Data verification: disabled

Starting Parquet reader performance test with 12 threads...

========================================
Parquet Reader Performance Test Results
========================================
Total files read: 100
Total row groups read: 800
Total rows read: 800000000
Total bytes read: 10240.5 MB
Elapsed time: 5234 ms
Throughput: 1956.3 MB/s
Row throughput: 152846.7 rows/sec
========================================
```

## Performance Characteristics

### Expected Throughput

- **1 SSD**: ~500-800 MB/s (Direct I/O)
- **6 SSDs**: ~2-3 GB/s (6 threads)
- **12 SSDs**: ~4-6 GB/s (12 threads)
- **24 SSDs**: ~8-12 GB/s (24 threads)

### CPU Usage

- Direct I/O: Low CPU usage (I/O bound)
- Buffered I/O: Higher CPU usage (page cache overhead)

### Memory Usage

- Per-thread buffers for column chunks
- Metadata structures for row groups
- Minimal overhead compared to full data processing

## Future Enhancements

1. **Data Verification**: Checksum validation of read data
2. **Async I/O**: io_uring support for comparison with Pixels
3. **Column Selection**: Read specific columns only
4. **Latency Metrics**: P50, P95, P99 latencies per thread
5. **CPU Affinity**: Bind threads to specific cores

## Testing Methodology

This test is designed for fair comparison with the Pixels reader:

1. **No Data Processing**: Only I/O operations, no deserialization
2. **Same Configuration**: Matching thread count, Direct I/O settings
3. **Same Data Layout**: Files distributed across same SSDs
4. **Same Metrics**: Throughput, row count, elapsed time

## References

- Apache Arrow Parquet: https://arrow.apache.org/docs/cpp/parquet.html
- parq reader reference: https://github.com/nvanwyen/parq
- Pixels reader test: `tests/reader-performance-test/reader_performance_test.cpp`