# himmel_ingest.exe (MEDIA, M7): watch folders -> ffmpeg.exe (external tool) -> HAP,
# thumbnails, analysis, sidecar + SQLite (docs/interfaces/library-db.md).
add_library(himmel_ingest_core STATIC src/scan.cpp src/ffmpeg.cpp src/svg.cpp src/ingest_ops.cpp src/selftest.cpp src/guard.cpp src/library_move.cpp)
target_include_directories(himmel_ingest_core PUBLIC include)
target_link_libraries(himmel_ingest_core PUBLIC himmel_library PRIVATE nlohmann_json::nlohmann_json d2d1 windowscodecs shlwapi ole32)

add_executable(himmel_ingest src/ingest_main.cpp)
# engine_shared.h is read for the EngineStatus layout only (Local\HimmelEngine, header-only use).
target_include_directories(himmel_ingest PRIVATE ${PROJECT_SOURCE_DIR}/src/engine/include)
target_link_libraries(himmel_ingest PRIVATE himmel_ingest_core himmel_library himmel_media himmel_protocol nlohmann_json::nlohmann_json)

foreach(t himmel_ingest_core himmel_ingest)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endif()
endforeach()
