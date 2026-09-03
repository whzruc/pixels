# DuckDB Pixels parallel-scan validation

This test uses ClickBench Q24 as an end-to-end workload. It keeps the query,
input files, column-size metadata, and DuckDB binary identical while varying
the worker count and io_uring backend. Raw output directories are local-only
and must not be committed.

## Functional checks

Build the release tree and run the focused tests:

```bash
make release -j12
build/release/extension/pixels/tests/BufferPool/CPUAffinityTest
build/release/extension/pixels/tests/metadata/PixelsFooterCacheTest
build/release/extension/pixels/tests/BufferPool/ColumnVectorBufferPoolTest
build/release/extension/pixels/tests/BufferPool/DynamicBufferPoolTest
build/release/extension/pixels/tests/BufferPool/GlobalStaticBufferPoolTest
build/release/extension/pixels/tests/BufferPool/DirectUringNonFixedTest
```

The io_uring-dependent tests report `SKIPPED` when the host does not expose
io_uring; this is expected and is not a functional pass on such a host.

## Performance matrix

Run from the repository root on a host with io_uring enabled:

```bash
python3 testcase/io-uring/benchmark-q24-backends.py \
  --pixels-glob '/data/<device>/clickbench/pixels-e0-fb/*.pxl' \
  --column-sizes /path/to/clickbench-column-sizes.csv \
  --threads 12 24 48 \
  --modes non-fixed dynamic static \
  --column-vector-pool true false \
  --repeats 5 \
  --timeout 14400 \
  --output testcase/io-uring/results/pr7-q24-$(date +%Y%m%d-%H%M%S)
```

The script randomizes measured runs, performs a warm-up per configuration,
checks result hashes, and writes `timings.csv`, `summary.csv`, and the
column-vector comparison. Compare medians and dispersion; do not treat a
small speedup as meaningful without the permutation-test result.

For CPU-affinity validation, create a private copy of
`etc/pixels-cpp.properties` and set:

```properties
pixels.enable.cpu.affinity=true
pixels.cpu.affinity.strategy=round-robin
pixels.cpu.affinity.core.mapping=
```

Run the same matrix once with affinity disabled and once enabled, using fresh
output directories. The production default remains disabled.
