# Engine skeleton: supervisor (himmel.exe) + render process (himmel_render.exe).

add_library(himmel_engine_shared STATIC src/engine_shared.cpp src/clip_player.cpp src/camera_capture.cpp)
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
