# Engine skeleton: supervisor (himmel.exe) + render process (himmel_render.exe).

# Effects and generators (FX track): himmel_fx, himmel_fx_gpu, fx_tests, fx_check.
add_subdirectory(fx)

add_library(himmel_engine_shared STATIC src/engine_shared.cpp src/clip_player.cpp src/camera_capture.cpp src/touch_fx.cpp src/entrancer.cpp src/entrancer_link.cpp src/midi_in.cpp
            src/display_watch.cpp)
target_include_directories(himmel_engine_shared PUBLIC include)
target_link_libraries(himmel_engine_shared PUBLIC himmel_media himmel_protocol himmel_library PRIVATE d3d11 mfplat mf mfreadwrite mfuuid ole32 ws2_32 winmm)

# The slice's show model (pure: no GPU, no Windows) - docs/interfaces/command-protocol.md.
add_library(himmel_engine_model STATIC src/deck_model.cpp src/transport.cpp src/fx_catalog.cpp src/reactions.cpp
            src/output_model.cpp)
target_include_directories(himmel_engine_model PUBLIC include)
target_link_libraries(himmel_engine_model PUBLIC himmel_protocol himmel_fx himmel_audio_block)

# Named shared-memory blocks + the clip catalog (Windows, start-up only).
add_library(himmel_engine_ipc STATIC src/shm_block.cpp src/clip_catalog.cpp src/reactions_link.cpp)
target_include_directories(himmel_engine_ipc PUBLIC include)
target_link_libraries(himmel_engine_ipc PUBLIC himmel_protocol himmel_media himmel_fx himmel_audio_block)

# ==== ENGINE (M6): Spout out - SpoutDX (BSD-2) as a small static library, no OpenGL part ====
set(SPOUT_SDK ${spout2_SOURCE_DIR}/SPOUTSDK)
add_library(himmel_spoutdx STATIC
    ${SPOUT_SDK}/SpoutDirectX/SpoutDX/SpoutDX.cpp
    ${SPOUT_SDK}/SpoutGL/SpoutSenderNames.cpp ${SPOUT_SDK}/SpoutGL/SpoutDirectX.cpp
    ${SPOUT_SDK}/SpoutGL/SpoutFrameCount.cpp ${SPOUT_SDK}/SpoutGL/SpoutCopy.cpp
    ${SPOUT_SDK}/SpoutGL/SpoutUtils.cpp ${SPOUT_SDK}/SpoutGL/SpoutSharedMemory.cpp)
target_include_directories(himmel_spoutdx SYSTEM PUBLIC ${SPOUT_SDK}/SpoutDirectX/SpoutDX ${SPOUT_SDK}/SpoutGL)
target_link_libraries(himmel_spoutdx PUBLIC d3d11 dxgi shlwapi winmm version)
if(MSVC)
    target_compile_options(himmel_spoutdx PRIVATE /W0 /utf-8)
endif()
add_library(himmel_spout_out STATIC src/spout_out.cpp)
target_include_directories(himmel_spout_out PUBLIC include)
target_link_libraries(himmel_spout_out PRIVATE himmel_spoutdx)
add_executable(spout_probe tools/spout_probe_main.cpp)  # a Spout receiver for checks
target_link_libraries(spout_probe PRIVATE himmel_spoutdx)
# ==== end ENGINE (M6) ====

# The slice engine (decks, FX, previews, MK2 Perform).
add_executable(himmel_render render/render_main.cpp)
target_link_libraries(himmel_render PRIVATE himmel_engine_shared himmel_engine_model himmel_engine_ipc himmel_perform
                      himmel_mk2 himmel_weapon himmel_fx_gpu himmel_show himmel_library himmel_spout_out d3d11 dxgi d3dcompiler winmm dwmapi)

# PARKED pre-slice renderer: test card, camera + latency test, Touch FX, Entrancer, weapon knob pages.
add_executable(himmel_render_legacy render/legacy_main.cpp)
target_link_libraries(himmel_render_legacy PRIVATE himmel_engine_shared himmel_mk2 himmel_weapon d3d11 dxgi d3dcompiler winmm)

add_executable(himmel supervisor/supervisor_main.cpp)
target_link_libraries(himmel PRIVATE himmel_engine_shared himmel_engine_ipc himmel_mk2 himmel_weapon himmel_audio_block himmel_library dwmapi)

# Tools: commands from the command line (producer slot 3), preview block -> PNG.
add_executable(himmel_cmd tools/himmel_cmd_main.cpp)
target_link_libraries(himmel_cmd PRIVATE himmel_engine_model himmel_engine_ipc)
add_executable(preview_dump tools/preview_dump_main.cpp)
target_link_libraries(preview_dump PRIVATE himmel_engine_ipc)

add_executable(deck_tests ${PROJECT_SOURCE_DIR}/tests/engine/deck_tests.cpp)
target_link_libraries(deck_tests PRIVATE himmel_engine_model)
add_test(NAME deck_tests COMMAND deck_tests)
add_executable(transport_tests ${PROJECT_SOURCE_DIR}/tests/engine/transport_tests.cpp)
target_link_libraries(transport_tests PRIVATE himmel_engine_model)
add_test(NAME transport_tests COMMAND transport_tests)
add_executable(output_tests ${PROJECT_SOURCE_DIR}/tests/engine/output_tests.cpp)
target_link_libraries(output_tests PRIVATE himmel_engine_model)
add_test(NAME output_tests COMMAND output_tests)

foreach(t himmel_engine_shared himmel_engine_model himmel_engine_ipc himmel_spout_out himmel_render himmel_render_legacy himmel himmel_cmd
          preview_dump deck_tests transport_tests output_tests)
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
