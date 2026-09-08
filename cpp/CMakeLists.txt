cmake_minimum_required(VERSION 3.19)

set(CMAKE_CXX_STANDARD 20)
set(TARGET_NAME pixels)
set(DCMAKE_EXPORT_COMPILE_COMMANDS=1)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
project(${TARGET_NAME})

# perf / bcc / perf script：保留帧指针，便于栈回溯（GCC/Clang；-O2 默认会省略帧指针）
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-fno-omit-frame-pointer)
endif()

add_definitions(-DDUCKDB_EXTENSION_LIBRARY)

include_directories(include)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(EXTENSION_SOURCES
        pixels-duckdb/pixels_extension.cpp
        pixels-duckdb/PixelsScanFunction.cpp
        pixels-duckdb/ArrowRandomAccessFile.cpp
        pixels-duckdb/ParquetPixelsScan.cpp
)

# Forward SPDK toggle to sub-directories.
# Run cmake with -DPIXELS_ENABLE_SPDK=ON to compile SPDK support.
option(PIXELS_ENABLE_SPDK "Build with SPDK user-space NVMe driver support" OFF)
if(PIXELS_ENABLE_SPDK)
    add_definitions(-DPIXELS_ENABLE_SPDK)
    message(STATUS "[pixels] SPDK support: ON")
else()
    message(STATUS "[pixels] SPDK support: OFF (pass -DPIXELS_ENABLE_SPDK=ON to enable)")
endif()

add_subdirectory(pixels-common)
add_subdirectory(pixels-core)
#add_subdirectory(pixels-cli)
add_subdirectory(third-party/googletest)
add_subdirectory(third-party/arrow)
add_subdirectory(tests)

include_directories(pixels-common/include)
include_directories(pixels-core/include)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR}/pixels-common/flatbuffers_out)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/pixels-common/liburing/src/include)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
set(PARAMETERS "-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS} ${EXTENSION_SOURCES})

find_library(JEMALLOC_LIB jemalloc)
if(JEMALLOC_LIB)
    message(STATUS "jemalloc found: ${JEMALLOC_LIB}")
else()
    message(STATUS "jemalloc not found, using system allocator")
endif()

# System Arrow/Parquet (v20) — use shared libs to avoid symbol conflicts
# with the project-embedded Arrow v18 static libs.
find_package(Arrow 20 REQUIRED CONFIG
    PATHS /usr/local/lib/cmake/Arrow
    NO_DEFAULT_PATH)
find_package(Parquet 20 REQUIRED CONFIG
    PATHS /usr/local/lib/cmake/Parquet
    NO_DEFAULT_PATH)
message(STATUS "Arrow found: ${Arrow_VERSION}, Parquet found: ${Parquet_VERSION}")

target_link_libraries(${EXTENSION_NAME}
        pixels-common
        pixels-core
        Arrow::arrow_shared
        Parquet::parquet_shared
#        $<$<BOOL:${JEMALLOC_LIB}>:${JEMALLOC_LIB}>
)

# Add the subdirectory that contains the build_loadable_extension definition
message("duckdb export set: ${DUCKDB_EXPORT_SET}" )
message("TARGET NAME: ${TARGET_NAME} EXTENSION NAME: ${EXTENSION_NAME}")

install(
        TARGETS ${EXTENSION_NAME} pixels-core pixels-common
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
