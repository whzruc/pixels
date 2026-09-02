# C++ Buffer Pool Validation

The C++ reader supports four values for `pixel.bufferpool.mode`:

- `legacy`: the existing registered-buffer path using fixed reads.
- `non-fixed`: the legacy allocator without io_uring buffer registration.
- `dynamic`: thread-local buffers allocated and registered on demand.
- `static`: per-column double buffers preallocated for a bounded number of scan threads.

The static pool uses `pixel.column.size.path` to size each column buffer and
`pixel.static.buffer.threads` to bound the number of preallocated thread slots.
Set `pixel.static.buffer.hugepage=true` to request transparent HugePages with
`MADV_HUGEPAGE`. This is an advisory optimization and requires Linux support.

Build the project, then validate the registered pool modes with the same ClickBench data:

```bash
testcase/buffer-pool/validate-clickbench.sh \
  /home/whz/pixels/clickbench-size-e0.csv \
  '/data/9a3-01/clickbench/pixels-e0-fb/*' \
  1
```

The script runs a row count, a filtered count, and a numeric aggregate. It
fails if any mode returns a different result. Increase the thread count only
after accounting for static-pool memory: each thread owns two buffers for every
column.
