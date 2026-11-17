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

# Spout 2 (BSD-2, Lynn Jarvis) for Spout out (ENGINE M6, LEAD 2026-11-17): only SpoutDX and the
# few SpoutGL helpers it includes are compiled (src/engine/CMakeLists.txt, target himmel_spoutdx);
# Spout's own CMake is never run (SOURCE_SUBDIR points at a folder without one). Pinned to the
# commit of release 2.007h. The archive is ~60 MB, downloaded once per build folder.
FetchContent_Declare(spout2
    URL https://github.com/leadedge/Spout2/archive/7d32b056f9e00174aeae80e722854ccaffcb2e5a.tar.gz
    URL_HASH SHA256=d746386ee9aff60ab05cef97d969441967c3d97c0a6c34a3e3e9bf6605553932
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR do-not-build)

FetchContent_MakeAvailable(hidapi json snappy spout2)

enable_testing()
add_subdirectory(src/protocol)
add_subdirectory(src/controller)
add_subdirectory(src/perform)
add_subdirectory(src/media)
add_subdirectory(src/library)
add_subdirectory(src/audio)
add_subdirectory(src/engine)
add_subdirectory(src/show)
add_subdirectory(src/ui_bridge)
