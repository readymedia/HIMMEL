# SHOW track (M3): projects, show files, scenes, undo, dice - docs/interfaces/show-files.md.

# Pure logic (no Windows, no heap in the frame-side functions): the model, looks -> commands, dice.
add_library(himmel_show_model STATIC src/model.cpp src/look_ops.cpp src/dice.cpp)
target_include_directories(himmel_show_model PUBLIC include)
target_link_libraries(himmel_show_model PUBLIC himmel_protocol himmel_engine_model)

# Files (worker side: JSON, atomic writes, journal, zip) + the runtime (frame side + save worker).
add_library(himmel_show STATIC src/files.cpp src/runtime.cpp)
target_include_directories(himmel_show PUBLIC include)
target_link_libraries(himmel_show PUBLIC himmel_show_model PRIVATE nlohmann_json::nlohmann_json shell32 ole32)

# show_cmd.exe: send show commands (slot 3 "Tool") and print the show block.
add_executable(show_cmd tools/show_cmd_main.cpp)
target_link_libraries(show_cmd PRIVATE himmel_show himmel_engine_ipc)

add_executable(show_tests ${PROJECT_SOURCE_DIR}/tests/show/show_tests.cpp)
target_link_libraries(show_tests PRIVATE himmel_show nlohmann_json::nlohmann_json)
add_test(NAME show_tests COMMAND show_tests)

# Kill a process in the middle of a save, then check that the autosave brings everything back.
add_executable(show_crash_test ${PROJECT_SOURCE_DIR}/tests/show/show_crash_test.cpp)
target_link_libraries(show_crash_test PRIVATE himmel_show)
add_test(NAME show_crash_test COMMAND show_crash_test)

foreach(t himmel_show_model himmel_show show_cmd show_tests show_crash_test)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endif()
endforeach()
