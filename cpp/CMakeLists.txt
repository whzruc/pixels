cmake_minimum_required(VERSION 3.19)

set(CMAKE_CXX_STANDARD 20)
set(TARGET_NAME pixels)
set(DCMAKE_EXPORT_COMPILE_COMMANDS=1)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
project(${TARGET_NAME})

add_definitions(-DDUCKDB_EXTENSION_LIBRARY)

include_directories(include)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third-party/flatbuffers EXCLUDE_FROM_ALL)

# Build the Arrow/Parquet ABI used by read_parquet_uring from the pinned
# third-party source tree. Shared libraries avoid collisions with DuckDB's
# own statically linked Arrow compatibility code.
set(ARROW_BUILD_SHARED ON CACHE BOOL "" FORCE)
set(ARROW_BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(ARROW_PARQUET ON CACHE BOOL "" FORCE)
set(ARROW_COMPUTE ON CACHE BOOL "" FORCE)
set(ARROW_IPC ON CACHE BOOL "" FORCE)
set(ARROW_DEPENDENCY_SOURCE BUNDLED CACHE STRING "" FORCE)
# The host already supplies Thrift; Arrow itself and Parquet remain pinned and
# built from cpp/third-party/arrow. This also avoids an old Thrift sub-build
# that is incompatible with CMake 4.
set(Thrift_SOURCE SYSTEM CACHE STRING "" FORCE)
set(ARROW_THRIFT_USE_SHARED ON CACHE BOOL "" FORCE)
# The locally installed Thrift package config exports its static archive even
# when shared linking is requested. Let Arrow's finder select libthrift.so.
set(CMAKE_DISABLE_FIND_PACKAGE_Thrift ON CACHE BOOL "" FORCE)
set(ARROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ARROW_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(ARROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PARQUET_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ARROW_SIMD_LEVEL NONE CACHE STRING "" FORCE)
set(ARROW_RUNTIME_SIMD_LEVEL NONE CACHE STRING "" FORCE)
add_subdirectory(third-party/arrow/cpp EXCLUDE_FROM_ALL)
include_directories(BEFORE
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/arrow/cpp/src
        ${CMAKE_CURRENT_BINARY_DIR}/third-party/arrow/cpp/src)

set(EXTENSION_SOURCES
        pixels-duckdb/pixels_extension.cpp
        pixels-duckdb/PixelsScanFunction.cpp
        pixels-duckdb/ArrowRandomAccessFile.cpp
        pixels-duckdb/ParquetPixelsScan.cpp
)

add_subdirectory(pixels-common)
add_subdirectory(pixels-core)
add_subdirectory(pixels-cli EXCLUDE_FROM_ALL)
add_subdirectory(third-party/googletest)
add_subdirectory(tests)
add_subdirectory(testcase/io-uring)

include_directories(pixels-common/include)
include_directories(pixels-core/include)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR}/pixels-common/liburing/src/include)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
set(PARAMETERS "-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS} ${EXTENSION_SOURCES})

# DuckDB creates this target with the plain target_link_libraries signature.
# Use the shared-library artifacts rather than exporting Arrow's internal CMake
# targets as part of DuckDB's install set.
target_link_libraries(${EXTENSION_NAME}
        pixels-common
        pixels-core
        $<BUILD_INTERFACE:$<TARGET_FILE:arrow_shared>>
        $<BUILD_INTERFACE:$<TARGET_FILE:parquet_shared>>)
add_dependencies(${EXTENSION_NAME} arrow_shared parquet_shared)

# Add the subdirectory that contains the build_loadable_extension definition
message("duckdb export set: ${DUCKDB_EXPORT_SET}" )
message("TARGET NAME: ${TARGET_NAME} EXTENSION NAME: ${EXTENSION_NAME}")

install(
        TARGETS ${EXTENSION_NAME} pixels-core pixels-common
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
