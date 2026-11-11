# Maschine MK2 controller module (spec: docs/mk2-controller-spec.md)

add_library(himmel_mk2 STATIC
    src/map.cpp
    src/input.cpp
    src/output.cpp
    src/shared.cpp
    src/controller.cpp
    src/sim_link.cpp
    src/pages.cpp)
target_include_directories(himmel_mk2 PUBLIC include)
target_link_libraries(himmel_mk2 PUBLIC nlohmann_json::nlohmann_json PRIVATE hidapi::hidapi ws2_32)

if(MSVC)
    target_compile_options(himmel_mk2 PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(mk2_controller app/mk2_controller_main.cpp)
target_link_libraries(mk2_controller PRIVATE himmel_mk2 winmm)

add_executable(mk2_monitor app/mk2_monitor_main.cpp)
target_link_libraries(mk2_monitor PRIVATE himmel_mk2)

add_executable(mk2_pages_check app/mk2_pages_check_main.cpp)
target_link_libraries(mk2_pages_check PRIVATE himmel_mk2)

add_executable(mk2_pages_demo app/mk2_pages_demo_main.cpp)
target_link_libraries(mk2_pages_demo PRIVATE himmel_mk2)

add_executable(mk2_tests ${PROJECT_SOURCE_DIR}/tests/controller/mk2_tests.cpp)
target_link_libraries(mk2_tests PRIVATE himmel_mk2)
target_compile_definitions(mk2_tests PRIVATE HIMMEL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME mk2_tests COMMAND mk2_tests)

# Generic weapons (Lightpad, LUMI Keys, NuVJ, ...): shared block Local\HimmelWeapon<N>,
# one driver process per weapon kind, MIDI over WinMM (part of Windows) or UDP (--sim).
add_library(himmel_weapon STATIC
    src/weapon/shared.cpp
    src/weapon/midi.cpp
    src/weapon/driver.cpp
    src/weapon/blocks_codec.cpp
    src/weapon/blocks_session.cpp
    src/weapon/nuvj.cpp)
target_include_directories(himmel_weapon PUBLIC include)
target_link_libraries(himmel_weapon PUBLIC himmel_mk2 nlohmann_json::nlohmann_json PRIVATE winmm)

add_executable(weapon_blocks app/weapon_blocks_main.cpp)
target_link_libraries(weapon_blocks PRIVATE himmel_weapon winmm)

add_executable(weapon_nuvj app/weapon_nuvj_main.cpp)
target_link_libraries(weapon_nuvj PRIVATE himmel_weapon winmm)

add_executable(weapon_monitor app/weapon_monitor_main.cpp)
target_link_libraries(weapon_monitor PRIVATE himmel_weapon)

add_executable(weapon_tests ${PROJECT_SOURCE_DIR}/tests/controller/weapon_tests.cpp)
target_link_libraries(weapon_tests PRIVATE himmel_weapon)
target_compile_definitions(weapon_tests PRIVATE HIMMEL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME weapon_tests COMMAND weapon_tests)

foreach(t mk2_controller mk2_monitor mk2_pages_check mk2_pages_demo mk2_tests
          himmel_weapon weapon_blocks weapon_nuvj weapon_monitor weapon_tests)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endif()
endforeach()
