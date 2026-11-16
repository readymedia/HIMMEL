# FX track (docs/interfaces/effect-metadata.md):
#   himmel_fx      - effect metadata (embedded fx/ folder), PRESS / scope / reaction / flash-cap
#                    resolution, the stub signal source. Pure: no GPU, no Windows.
#   himmel_fx_gpu  - one D3D11 pixel shader per effect (compiled at start-up), runs FX chains.
file(GLOB HIMMEL_FX_INPUTS CONFIGURE_DEPENDS
     ${PROJECT_SOURCE_DIR}/fx/*.json ${PROJECT_SOURCE_DIR}/fx/*.hlsl
     ${PROJECT_SOURCE_DIR}/fx/*/*.json ${PROJECT_SOURCE_DIR}/fx/*/*.hlsl)
set(HIMMEL_FX_EMBED ${CMAKE_CURRENT_BINARY_DIR}/generated/himmel/fx/builtin_data.h)
add_custom_command(OUTPUT ${HIMMEL_FX_EMBED}
    COMMAND ${CMAKE_COMMAND} -DFX_DIR=${PROJECT_SOURCE_DIR}/fx -DOUT=${HIMMEL_FX_EMBED}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_fx.cmake
    DEPENDS ${HIMMEL_FX_INPUTS} ${CMAKE_CURRENT_SOURCE_DIR}/embed_fx.cmake
    COMMENT "Embedding fx/ (effect metadata + HLSL)")

add_library(himmel_fx STATIC src/library.cpp src/resolve.cpp src/signals.cpp ${HIMMEL_FX_EMBED})
target_include_directories(himmel_fx PUBLIC include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(himmel_fx PUBLIC himmel_protocol PRIVATE nlohmann_json::nlohmann_json)

add_library(himmel_fx_gpu STATIC src/fx_gpu.cpp)
target_link_libraries(himmel_fx_gpu PUBLIC himmel_fx PRIVATE himmel_mk2 d3d11 d3dcompiler)

add_executable(fx_tests ${PROJECT_SOURCE_DIR}/tests/fx/fx_tests.cpp)
target_link_libraries(fx_tests PRIVATE himmel_fx himmel_engine_model d3dcompiler)
add_test(NAME fx_tests COMMAND fx_tests)

add_executable(fx_check ${PROJECT_SOURCE_DIR}/tools/fx_check/fx_check_main.cpp)
target_link_libraries(fx_check PRIVATE himmel_fx_gpu himmel_mk2 d3d11 dxgi)

foreach(t himmel_fx himmel_fx_gpu fx_tests fx_check)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endif()
endforeach()
