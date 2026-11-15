# Engine protocol contracts (docs/interfaces/): header-only, owned by the LEAD.
add_library(himmel_protocol INTERFACE)
target_include_directories(himmel_protocol INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(protocol_tests ${PROJECT_SOURCE_DIR}/tests/protocol/protocol_tests.cpp)
target_link_libraries(protocol_tests PRIVATE himmel_protocol)
add_test(NAME protocol_tests COMMAND protocol_tests)
if(MSVC)
    target_compile_options(protocol_tests PRIVATE /W4 /permissive- /utf-8)
endif()
