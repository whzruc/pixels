add_executable(PixelsIoUringScanBenchmark EXCLUDE_FROM_ALL PixelsIoUringScanBenchmark.cpp)

target_link_libraries(PixelsIoUringScanBenchmark PRIVATE pixels-common)

target_include_directories(
    PixelsIoUringScanBenchmark
    PRIVATE
    ${PROJECT_SOURCE_DIR}/pixels-common/include
    ${CMAKE_CURRENT_BINARY_DIR}/../../pixels-common/liburing/src/include
)
