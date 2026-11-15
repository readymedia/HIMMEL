# The web GUI bridge (GUI track, D43): himmel_ui.exe + fake_engine.exe + tests.
# No new dependencies: Winsock + nlohmann/json (already fetched).

add_library(himmel_ui_core STATIC src/json_codec.cpp src/http_util.cpp)
target_include_directories(himmel_ui_core PUBLIC include PRIVATE src)
target_link_libraries(himmel_ui_core PUBLIC himmel_protocol nlohmann_json::nlohmann_json)

add_executable(himmel_ui src/ui_main.cpp)
target_include_directories(himmel_ui PRIVATE src)
target_link_libraries(himmel_ui PRIVATE himmel_ui_core ws2_32)

add_executable(fake_engine tools/fake_engine_main.cpp)
target_link_libraries(fake_engine PRIVATE himmel_ui_core winmm)

add_executable(ui_bridge_tests ${PROJECT_SOURCE_DIR}/tests/ui_bridge/ui_bridge_tests.cpp)
target_include_directories(ui_bridge_tests PRIVATE src)
target_link_libraries(ui_bridge_tests PRIVATE himmel_ui_core)
add_test(NAME ui_bridge_tests COMMAND ui_bridge_tests)

foreach(t himmel_ui_core himmel_ui fake_engine ui_bridge_tests)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
endforeach()
