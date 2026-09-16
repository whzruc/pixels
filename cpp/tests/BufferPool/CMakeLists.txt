add_executable(
        DynamicBufferPoolTest
        DynamicBufferPoolTest.cpp
)

target_link_libraries(
        DynamicBufferPoolTest
        PRIVATE
        gtest_main
        pixels-common
)

set(GTEST_DIR "${PROJECT_SOURCE_DIR}/third-party/googletest")
target_include_directories(DynamicBufferPoolTest PRIVATE
        ${GTEST_DIR}/googletest/include
        ${PROJECT_SOURCE_DIR}/pixels-common/include
        ${CMAKE_CURRENT_BINARY_DIR}/../../pixels-common/liburing/src/include
)

add_executable(
        GlobalStaticBufferPoolTest
        GlobalStaticBufferPoolTest.cpp
)

target_link_libraries(
        GlobalStaticBufferPoolTest
        PRIVATE
        gtest_main
        pixels-common
)

target_include_directories(GlobalStaticBufferPoolTest PRIVATE
        ${GTEST_DIR}/googletest/include
        ${PROJECT_SOURCE_DIR}/pixels-common/include
        ${CMAKE_CURRENT_BINARY_DIR}/../../pixels-common/liburing/src/include
)

add_executable(
        DirectUringNonFixedTest
        DirectUringNonFixedTest.cpp
)

target_link_libraries(
        DirectUringNonFixedTest
        PRIVATE
        gtest_main
        pixels-common
)

target_include_directories(DirectUringNonFixedTest PRIVATE
        ${GTEST_DIR}/googletest/include
        ${PROJECT_SOURCE_DIR}/pixels-common/include
        ${CMAKE_CURRENT_BINARY_DIR}/../../pixels-common/liburing/src/include
)
add_executable(CPUAffinityTest CPUAffinityTest.cpp)
target_link_libraries(CPUAffinityTest PRIVATE gtest_main)
target_include_directories(CPUAffinityTest PRIVATE
        ${GTEST_DIR}/googletest/include
        ${PROJECT_SOURCE_DIR}/include
)

# Pure scheduling tests: no SSDs, DuckDB runtime, or io_uring privileges needed.
add_executable(SelectiveBufferSchedulerTest SelectiveBufferSchedulerTest.cpp)
target_include_directories(SelectiveBufferSchedulerTest PRIVATE
        ${PROJECT_SOURCE_DIR}/pixels-common/include
)
target_compile_options(SelectiveBufferSchedulerTest PRIVATE -UNDEBUG)
target_link_libraries(SelectiveBufferSchedulerTest PRIVATE pthread)
add_test(NAME SelectiveBufferSchedulerTest COMMAND SelectiveBufferSchedulerTest)

# Validate the allocator bridge without requiring a full DuckDB process. Test
# doubles use the same prefixed symbols exported by DuckDB's jemalloc build.
add_executable(AlignedMemoryTest AlignedMemoryTest.cpp)
target_compile_definitions(AlignedMemoryTest PRIVATE PIXELS_USE_DUCKDB_JEMALLOC)
target_include_directories(AlignedMemoryTest PRIVATE
        ${GTEST_DIR}/googletest/include
        ${PROJECT_SOURCE_DIR}/pixels-common/include
)
target_link_libraries(AlignedMemoryTest PRIVATE gtest_main)
add_test(NAME AlignedMemoryTest COMMAND AlignedMemoryTest)
