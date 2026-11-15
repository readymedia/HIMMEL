# Weapon -> command mappers that run inside the render process (CONTROL track).
add_library(himmel_perform STATIC src/mk2_perform.cpp)
target_include_directories(himmel_perform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(himmel_perform PUBLIC himmel_protocol himmel_mk2)
if(MSVC)
    target_compile_options(himmel_perform PRIVATE /W4 /permissive- /utf-8)
endif()
