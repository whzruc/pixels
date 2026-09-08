# Parquet Reader Performance Test

This test measures the pure I/O performance of reading Parquet files using `pread`. It only reads data into buffers without processing, allowing for direct comparison with the Pixels reader performance test.

## Features

- **Pure I/O Testing**: Only reads data into buffers, no data processing
- **Direct I/O Support**: Supports O_DIRECT flag for bypassing page cache
- **Data Verification**: Optional checksum verification to ensure data correctness
- **Multi-threaded**: Supports concurrent reading with multiple threads
- **24 SSD Support**: Parallel reading across 24 SSDs for maximum throughput
- **Performance Metrics**: Measures throughput, latency (avg, P50, P95, P99)
- **Flamegraph Generation**: Automated CPU profiling with perf and flamegraphs
- **Compatible Structure**: Follows the same structure as `reader_performance_test.cpp`

## Building

The test uses Apache Arrow Parquet library for accurate metadata parsing. First build is slower as it compiles Arrow.

### Quick Build

```bash
cd tests/parquet-reader-performance-test
./build.sh
```

### Manual Build

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build Arrow Parquet library (first time only, takes 10-20 minutes)
make arrow_static parquet_static -j$(nproc)

# Build the test
make parquet_reader_performance_test -j$(nproc)
```

**Note**: The first build will take longer as it compiles Apache Arrow Parquet library. Subsequent builds are much faster.

## Configuration

### Single SSD Configuration (`config.properties`)

```properties
# Parquet files to read (glob patterns supported)
test.data.files=/data/parquet/*.parquet

# Number of threads (-1 = all CPU cores)
test.threads=6

# Enable direct I/O (bypasses page cache)
localfs.enable.direct.io=true

# Row group range (start, count)
test.rg.start=0
test.rg.count=-1  # -1 means read all row groups

# Query ID for tracking
test.query.id=1

# Enable data verification (reads first row group twice)
test.verify.data=false
```

### 24 SSD Configuration (`config-24ssd.properties`)

```properties
# Parquet files distributed across 24 SSDs
test.data.files=/data/9a3-01/clickbench/parquet/*.parquet,/data/9a3-02/clickbench/parquet/*.parquet,...

# 24 threads for 24 SSDs
test.threads=24

# Enable direct I/O
localfs.enable.direct.io=true

# Other settings...
```

## Running

### Manual Execution

```bash
# Run with single SSD config
./build/tests/parquet_reader_performance_test tests/parquet-reader-performance-test/config.properties

# Run with 24 SSD config
./build/tests/parquet_reader_performance_test tests/parquet-reader-performance-test/config-24ssd.properties
```

### Automated Performance Testing

Use the `run_perf.py` script for comprehensive testing:

```bash
cd tests/parquet-reader-performance-test

# Run all tests (1 SSD + 24 SSD, direct + buffered I/O)
./run_perf.py

# Run specific configurations
./run_perf.py --ssd-modes 24ssd --io-modes direct

# Skip flamegraph generation
./run_perf.py --skip-flamegraph

# Enable data verification
./run_perf.py --verify-data

# Custom binary and output paths
./run_perf.py --binary ./build/tests/parquet_reader_performance_test \
              --output-dir ./my-results
```

### Script Options

```
--binary PATH           Path to test binary (default: ./build/tests/parquet_reader_performance_test)
--config-dir PATH       Directory with config files (default: ./tests/parquet-reader-performance-test)
--output-dir PATH       Results output directory (default: ./tests/parquet-reader-performance-test/perf-results)
--ssd-modes MODE        SSD configs to test: 1ssd, 24ssd (default: both)
--io-modes MODE         I/O modes to test: direct, buffered (default: both)
--skip-flamegraph       Skip CPU flamegraph generation
--verify-data           Enable data verification
```

## Example Output

```
Configuration loaded:
  Files to read: 100
  Threads: 24
  Direct I/O: enabled
  Data verification: disabled
  Row group range: [0, all)

Starting Parquet reader performance test with 24 threads...

========================================
Parquet Reader Performance Test Results
========================================
Total files read: 100
Total row groups read: 800
Total rows read: 800000000
Total bytes read: 10240.5 MB
Elapsed time: 5234 ms
Throughput: 1956.3 MB/s
Avg latency: 65.4 ms
P50 latency: 62.1 ms
P95 latency: 89.3 ms
P99 latency: 102.7 ms
Data verification: PASSED
========================================
```

## Performance Testing Results

After running `run_perf.py`, you'll find:

```
perf-results/
├── 1ssd_direct.output.txt          # Test output
├── 1ssd_direct.stat.txt            # perf stat metrics
├── 1ssd_direct.svg                 # CPU flamegraph
├── 1ssd_buffered.output.txt
├── 1ssd_buffered.stat.txt
├── 1ssd_buffered.svg
├── 24ssd_direct.output.txt
├── 24ssd_direct.stat.txt
├── 24ssd_direct.svg
├── 24ssd_buffered.output.txt
├── 24ssd_buffered.stat.txt
├── 24ssd_buffered.svg
└── performance_summary.csv         # Summary of all tests
```

## Comparison with Pixels

This test is designed to be directly comparable with `reader_performance_test.cpp`:

| Feature | Pixels Test | Parquet Test |
|---------|-------------|--------------|
| I/O Method | pread/io_uring/async | pread with O_DIRECT |
| Direct I/O | ✅ | ✅ |
| Data Verification | ❌ | ✅ |
| Data Processing | None | None |
| Multi-threading | ✅ | ✅ |
| 24 SSD Support | ✅ | ✅ |
| Metrics | Throughput, Latency | Throughput, Latency |
| File Sorting | By number | By number |
| Flamegraphs | ✅ | ✅ |

## Direct I/O Implementation

When `localfs.enable.direct.io=true`:

1. **Aligned Buffers**: Uses `posix_memalign()` for 4KB-aligned buffers
2. **Aligned Offsets**: Reads from 4KB-aligned file offsets
3. **Aligned Sizes**: Reads in 4KB-aligned chunks
4. **Page Cache Bypass**: Data goes directly from disk to user buffer

This ensures maximum I/O performance by avoiding kernel page cache overhead.

## Data Verification

When `test.verify.data=true`:

1. Reads the first row group of each file twice
2. Computes checksums for both reads
3. Compares checksums to detect any data corruption
4. Reports verification errors in the output

This is useful for validating that direct I/O is working correctly.

## Performance Tips

1. **File Distribution**: Distribute Parquet files across multiple SSDs for maximum parallelism
2. **Thread Count**: Set `test.threads` to match the number of SSDs
3. **Direct I/O**: Enable for production-like performance (bypasses page cache)
4. **File Naming**: Use numbered files (e.g., `data_0.parquet`, `data_1.parquet`) for proper interleaving
5. **Verification**: Disable data verification for pure performance testing

## Implementation Details

- **Metadata Parsing**: Uses Apache Arrow Parquet library for accurate row group information
  - Reads actual row group count, offsets, and sizes from Parquet metadata
  - Extracts column chunk information for precise I/O operations
  - No estimation - all metadata is parsed from Thrift structures
- **Direct I/O**: Proper alignment handling for O_DIRECT flag
- **Zero-Copy**: Data is read directly into buffers without additional copying
- **Thread Safety**: Each thread opens its own file descriptor
- **Memory Management**: Buffers are reused within each thread
- **Checksum Verification**: Simple rolling hash for data integrity checks

## Troubleshooting

### Direct I/O Errors

If you see "Invalid argument" errors with direct I/O:
- Ensure file system supports O_DIRECT (ext4, xfs do; tmpfs doesn't)
- Check that files are on physical disks, not network mounts

### Performance Issues

- Check that files are distributed across SSDs
- Verify thread count matches SSD count
- Use `perf` and flamegraphs to identify bottlenecks
- Ensure no other I/O-heavy processes are running

## Future Enhancements

1. ~~Full Thrift metadata parsing for accurate row group information~~ (simplified estimation works well)
2. ~~Support for column-level reading~~ (out of scope for pure I/O test)
3. CPU affinity binding (similar to Pixels test)
4. ~~Direct I/O support~~ ✅ Implemented
5. Async I/O with io_uring (for comparison with Pixels)