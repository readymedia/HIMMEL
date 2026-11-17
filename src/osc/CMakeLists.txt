# OSC in (M13, CONTROL track): docs/interfaces/osc.md.
add_library(himmel_osc_lib STATIC src/osc.cpp)
target_include_directories(himmel_osc_lib PUBLIC include)
target_link_libraries(himmel_osc_lib PUBLIC himmel_protocol)

add_executable(himmel_osc app/osc_main.cpp)
target_link_libraries(himmel_osc PRIVATE himmel_osc_lib himmel_engine_ipc ws2_32)

add_executable(osc_tests ${PROJECT_SOURCE_DIR}/tests/osc/osc_tests.cpp)
target_link_libraries(osc_tests PRIVATE himmel_osc_lib)
add_test(NAME osc_tests COMMAND osc_tests)

foreach(t himmel_osc_lib himmel_osc osc_tests)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endif()
endforeach()
