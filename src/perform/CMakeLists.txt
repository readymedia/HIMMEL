# Weapon -> command mappers that run inside the render process (CONTROL track).
add_library(himmel_perform STATIC src/mk2_perform.cpp src/lightpad_perform.cpp)
target_include_directories(himmel_perform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(himmel_perform PUBLIC himmel_protocol himmel_mk2)
if(MSVC)
    target_compile_options(himmel_perform PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(perform_tests ${PROJECT_SOURCE_DIR}/tests/perform/perform_tests.cpp)
target_link_libraries(perform_tests PRIVATE himmel_perform)
add_test(NAME perform_tests COMMAND perform_tests)
if(MSVC)
    target_compile_options(perform_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(lightpad_tests ${PROJECT_SOURCE_DIR}/tests/perform/lightpad_tests.cpp)
target_link_libraries(lightpad_tests PRIVATE himmel_perform)
add_test(NAME lightpad_tests COMMAND lightpad_tests)
if(MSVC)
    target_compile_options(lightpad_tests PRIVATE /W4 /permissive- /utf-8)
endif()

# The whole MK2 chain without hardware: UDP simulator -> controller -> ring 0 ->
# Mk2Perform -> commands, and LEDs / screens back (tests/controller/mk2_chain_tests.cpp).
add_executable(mk2_chain_tests ${PROJECT_SOURCE_DIR}/tests/controller/mk2_chain_tests.cpp)
target_link_libraries(mk2_chain_tests PRIVATE himmel_perform himmel_mk2 ws2_32)
add_test(NAME mk2_chain_tests COMMAND mk2_chain_tests)
if(MSVC)
    target_compile_options(mk2_chain_tests PRIVATE /W4 /permissive- /utf-8)
endif()
