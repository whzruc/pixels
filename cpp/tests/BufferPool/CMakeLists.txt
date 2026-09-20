add_executable(
        BufferPoolTest
        dynamic_buffer_pool_test.cpp
)

if (CMAKE_BUILD_TYPE MATCHES "Debug")
    set(CMAKE_CPP_FLAGS "${CMAKE_CPP_FLAGS} -fsanitize=undefined -fsanitize=address")
    target_link_options(BufferPoolTest BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address)
endif ()

target_link_libraries(
        BufferPoolTest
        PRIVATE
        gtest_main
        duckdb
        pixels-core
)

set(GTEST_DIR "${PROJECT_SOURCE_DIR}/third-party/googletest")
include_directories(${GTEST_DIR}/googletest/include)
include_directories(${PROJECT_SOURCE_DIR}/pixels-core/include)
include_directories(${PROJECT_SOURCE_DIR}/pixels-common/include)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/../../pixels-common/liburing/src/include)

# Pure scheduling test: no SSD, DuckDB process, or io_uring privilege needed.
add_executable(
        SelectiveBufferSchedulerTest
        EXCLUDE_FROM_ALL
        SelectiveBufferSchedulerTest.cpp
)
target_include_directories(
        SelectiveBufferSchedulerTest
        PRIVATE
        ${PROJECT_SOURCE_DIR}/pixels-common/include
)
target_compile_options(SelectiveBufferSchedulerTest PRIVATE -UNDEBUG)
target_link_libraries(SelectiveBufferSchedulerTest PRIVATE pthread)
