cmake_minimum_required(VERSION 3.19)

add_executable(parquet_reader_performance_test
    parquet_reader_performance_test.cpp
)

target_link_libraries(parquet_reader_performance_test PRIVATE
    arrow_shared
    parquet_shared
    Threads::Threads
)

target_compile_features(parquet_reader_performance_test PRIVATE cxx_std_17)
target_compile_options(parquet_reader_performance_test PRIVATE
    -Wall
    -Wextra
    -fno-omit-frame-pointer
)
