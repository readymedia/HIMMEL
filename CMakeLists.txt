cmake_minimum_required(VERSION 3.24)
project(Himmel VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)

# hidapi (agreed stack: controller access). Static, no test tools.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(HIDAPI_WITH_HIDTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(hidapi
    URL https://github.com/libusb/hidapi/archive/refs/tags/hidapi-0.14.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# nlohmann/json (header-only) for the map file and later the show/preset JSON.
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# Snappy (BSD) for HAP's second-stage compression (decision 2026-11-10).
set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SNAPPY_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(snappy
    URL https://github.com/google/snappy/archive/refs/tags/1.2.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(hidapi json snappy)

enable_testing()
add_subdirectory(src/controller)
add_subdirectory(src/media)
add_subdirectory(src/engine)
