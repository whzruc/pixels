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

add_executable(
        ColumnVectorBufferPoolTest
        ColumnVectorBufferPoolTest.cpp
)

target_link_libraries(
        ColumnVectorBufferPoolTest
        PRIVATE
        gtest_main
        pixels-core
)

target_include_directories(ColumnVectorBufferPoolTest PRIVATE
        ${GTEST_DIR}/googletest/include
        ${PROJECT_SOURCE_DIR}/pixels-common/include
        ${PROJECT_SOURCE_DIR}/pixels-core/include
)

add_executable(CPUAffinityTest CPUAffinityTest.cpp)
target_link_libraries(CPUAffinityTest PRIVATE gtest_main)
target_include_directories(CPUAffinityTest PRIVATE
        ${GTEST_DIR}/googletest/include
        ${PROJECT_SOURCE_DIR}/include
)
