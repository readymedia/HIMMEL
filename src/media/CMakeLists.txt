# Media: MOV reader + HAP frame decoder. Pure C++, no Windows, no GPU.

add_library(himmel_media STATIC src/mov.cpp src/hap.cpp)
target_include_directories(himmel_media PUBLIC include)
target_link_libraries(himmel_media PRIVATE snappy)

add_executable(hap_probe tools/hap_probe_main.cpp)
target_link_libraries(hap_probe PRIVATE himmel_media)

foreach(t himmel_media hap_probe)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endif()
endforeach()

add_executable(media_tests ${PROJECT_SOURCE_DIR}/tests/media/media_tests.cpp)
target_link_libraries(media_tests PRIVATE himmel_media snappy)
target_compile_definitions(media_tests PRIVATE HIMMEL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME media_tests COMMAND media_tests)
if(MSVC)
    target_compile_options(media_tests PRIVATE /W4 /permissive- /utf-8)
endif()
