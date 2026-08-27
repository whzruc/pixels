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
