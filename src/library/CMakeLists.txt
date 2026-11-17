# Library (MEDIA, M7): the SQLite index, sidecars, analysis v1 and the engine's
# library feed (docs/interfaces/library-db.md).

# SQLite (public domain), the amalgamation, compiled in (THIRD_PARTY_NOTICES.md).
FetchContent_Declare(sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip
    URL_HASH SHA3_256=0e4df2263ef8169f9396b9535d9654d8d64ae8eb1a339ba06c7b12a72e6fb020
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite3)
add_library(himmel_sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(himmel_sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(himmel_sqlite3 PRIVATE SQLITE_THREADSAFE=2 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
                           SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)

add_library(himmel_library STATIC src/record.cpp src/hash.cpp src/analysis.cpp src/sidecar.cpp src/library_db.cpp src/feed.cpp src/location.cpp)
target_include_directories(himmel_library PUBLIC include)
target_link_libraries(himmel_library PUBLIC himmel_protocol PRIVATE himmel_sqlite3 nlohmann_json::nlohmann_json shell32 ole32)

add_executable(library_tests ${PROJECT_SOURCE_DIR}/tests/library/library_tests.cpp)
target_link_libraries(library_tests PRIVATE himmel_library himmel_ingest_core himmel_sqlite3)
add_test(NAME library_tests COMMAND library_tests)
add_dependencies(library_tests himmel_ingest)  # test_ingest_cli runs the exe next to it

foreach(t himmel_library library_tests)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endif()
endforeach()
