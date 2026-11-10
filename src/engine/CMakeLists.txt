# Engine skeleton: supervisor (himmel.exe) + render process (himmel_render.exe).

add_library(himmel_engine_shared STATIC src/engine_shared.cpp src/clip_player.cpp src/camera_capture.cpp src/touch_fx.cpp)
target_include_directories(himmel_engine_shared PUBLIC include)
target_link_libraries(himmel_engine_shared PUBLIC himmel_media PRIVATE d3d11 mfplat mf mfreadwrite mfuuid ole32)

add_executable(himmel_render render/render_main.cpp)
target_link_libraries(himmel_render PRIVATE himmel_engine_shared himmel_mk2 d3d11 dxgi d3dcompiler winmm)

add_executable(himmel supervisor/supervisor_main.cpp)
target_link_libraries(himmel PRIVATE himmel_engine_shared himmel_mk2)

foreach(t himmel_engine_shared himmel_render himmel)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endif()
endforeach()

# Touch FX (docs/touch-fx-entrancer.md): unit tests and a PNG / GPU-time check.
add_executable(touch_fx_tests ${PROJECT_SOURCE_DIR}/tests/engine/touch_fx_tests.cpp)
target_link_libraries(touch_fx_tests PRIVATE himmel_engine_shared)
add_test(NAME touch_fx_tests COMMAND touch_fx_tests)

add_executable(touch_fx_check tools/touch_fx_check_main.cpp)
target_link_libraries(touch_fx_check PRIVATE himmel_engine_shared d3d11 d3dcompiler)
foreach(t touch_fx_tests touch_fx_check)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endif()
endforeach()
