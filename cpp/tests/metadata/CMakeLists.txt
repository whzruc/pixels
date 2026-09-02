add_executable(
        PixelsFooterCacheTest
        PixelsFooterCacheTest.cpp
)

target_link_libraries(
        PixelsFooterCacheTest
        PRIVATE
        gtest_main
        pixels-core
)

set(GTEST_DIR "${PROJECT_SOURCE_DIR}/third-party/googletest")
target_include_directories(PixelsFooterCacheTest PRIVATE
        ${GTEST_DIR}/googletest/include
        ${PROJECT_SOURCE_DIR}/pixels-common/include
        ${PROJECT_SOURCE_DIR}/pixels-core/include
)
