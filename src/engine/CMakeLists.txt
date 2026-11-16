# Engine skeleton: supervisor (himmel.exe) + render process (himmel_render.exe).

# Effects and generators (FX track): himmel_fx, himmel_fx_gpu, fx_tests, fx_check.
add_subdirectory(fx)

add_library(himmel_engine_shared STATIC src/engine_shared.cpp src/clip_player.cpp src/camera_capture.cpp src/touch_fx.cpp src/entrancer.cpp src/entrancer_link.cpp src/midi_in.cpp)
target_include_directories(himmel_engine_shared PUBLIC include)
target_link_libraries(himmel_engine_shared PUBLIC himmel_media PRIVATE d3d11 mfplat mf mfreadwrite mfuuid ole32 ws2_32 winmm)

# The slice's show model (pure: no GPU, no Windows) - docs/interfaces/command-protocol.md.
add_library(himmel_engine_model STATIC src/deck_model.cpp src/fx_catalog.cpp)
target_include_directories(himmel_engine_model PUBLIC include)
target_link_libraries(himmel_engine_model PUBLIC himmel_protocol himmel_fx)

# Named shared-memory blocks + the clip catalog (Windows, start-up only).
add_library(himmel_engine_ipc STATIC src/shm_block.cpp src/clip_catalog.cpp)
target_include_directories(himmel_engine_ipc PUBLIC include)
target_link_libraries(himmel_engine_ipc PUBLIC himmel_protocol himmel_media)

# The slice engine (decks, FX, previews, MK2 Perform).
add_executable(himmel_render render/render_main.cpp)
target_link_libraries(himmel_render PRIVATE himmel_engine_shared himmel_engine_model himmel_engine_ipc himmel_perform
                      himmel_mk2 himmel_fx_gpu d3d11 dxgi d3dcompiler winmm)

# PARKED pre-slice renderer: test card, camera + latency test, Touch FX, Entrancer, weapon knob pages.
add_executable(himmel_render_legacy render/legacy_main.cpp)
target_link_libraries(himmel_render_legacy PRIVATE himmel_engine_shared himmel_mk2 himmel_weapon d3d11 dxgi d3dcompiler winmm)

add_executable(himmel supervisor/supervisor_main.cpp)
target_link_libraries(himmel PRIVATE himmel_engine_shared himmel_mk2 himmel_weapon)

# Tools: commands from the command line (producer slot 3), preview block -> PNG.
add_executable(himmel_cmd tools/himmel_cmd_main.cpp)
target_link_libraries(himmel_cmd PRIVATE himmel_engine_model himmel_engine_ipc)
add_executable(preview_dump tools/preview_dump_main.cpp)
target_link_libraries(preview_dump PRIVATE himmel_engine_ipc)

add_executable(deck_tests ${PROJECT_SOURCE_DIR}/tests/engine/deck_tests.cpp)
target_link_libraries(deck_tests PRIVATE himmel_engine_model)
add_test(NAME deck_tests COMMAND deck_tests)

foreach(t himmel_engine_shared himmel_engine_model himmel_engine_ipc himmel_render himmel_render_legacy himmel himmel_cmd
          preview_dump deck_tests)
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

add_executable(entrancer_tests ${PROJECT_SOURCE_DIR}/tests/engine/entrancer_tests.cpp)
target_link_libraries(entrancer_tests PRIVATE himmel_engine_shared)
add_test(NAME entrancer_tests COMMAND entrancer_tests)
if(MSVC)
    target_compile_options(entrancer_tests PRIVATE /W4 /permissive- /utf-8)
endif()
