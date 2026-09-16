cmake_minimum_required(VERSION 3.19)

add_executable(parquet_reader_performance_test
    parquet_reader_performance_test.cpp
)

target_link_libraries(parquet_reader_performance_test PRIVATE
    arrow_shared
    parquet_shared
    Threads::Threads
)

# libparquet.so carries a DT_NEEDED entry for the vendored Arrow SONAME
# (libarrow.so.1800).  The linker does not use the runtime RPATH to resolve
# that dependency while linking this standalone executable, so explicitly add
# the Arrow target directory as an rpath-link search path.  Without this,
# builds can accidentally mix the system Arrow version or report unresolved
# Arrow symbols even though the vendored libraries were built successfully.
target_link_options(parquet_reader_performance_test PRIVATE
    "-Wl,-rpath-link,$<TARGET_FILE_DIR:arrow_shared>"
)

target_compile_features(parquet_reader_performance_test PRIVATE cxx_std_17)
target_compile_options(parquet_reader_performance_test PRIVATE
    -Wall
    -Wextra
    -fno-omit-frame-pointer
)
