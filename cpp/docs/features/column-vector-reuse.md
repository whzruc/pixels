# ColumnVector backing-buffer reuse

Pixels readers create a row batch for each file-processing context. String
columns allocate a large aligned descriptor array; repeatedly allocating and
freeing that array creates allocator, page-fault, and page-table contention at
high concurrency.

`ColumnVectorBufferPool` keeps these aligned arrays in a bounded thread-local
free list keyed by size and alignment. Thread-local ownership avoids locks and
keeps reuse on the worker that most recently touched the pages. The string
storage used only by the writer path is allocated lazily.

Set the following property to enable the optimization (the default):

```properties
pixels.columnvector.pool=true
```

Set it to `false` for an A/B baseline. The setting is read once per process, so
each side of a comparison must run in a separate process. Disabling the option
also restores eager construction of the writer-only string container, matching
the pre-optimization behavior.

Column vectors also distinguish owned backing arrays from zero-copy views into
reader buffers. Only owned arrays are freed or returned to a pool.
