# AUDIO track (docs/interfaces/audio-block.md): himmel_audio.exe and its analysis.

# The block layout only (header-only): readers such as the engine link this.
add_library(himmel_audio_block INTERFACE)
target_include_directories(himmel_audio_block INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(himmel_audio_block INTERFACE himmel_protocol)

# Pure analysis + MIDI clock tracker (no Windows): testable without hardware.
add_library(himmel_audio_dsp STATIC src/analyzer.cpp src/midi_clock.cpp)
target_link_libraries(himmel_audio_dsp PUBLIC himmel_audio_block)

# Windows input: WASAPI and the WinMM MIDI clock port.
add_library(himmel_audio_io STATIC src/wasapi.cpp src/midi_port.cpp)
target_link_libraries(himmel_audio_io PUBLIC himmel_audio_dsp PRIVATE ole32 winmm)

add_executable(himmel_audio app/audio_main.cpp)
target_link_libraries(himmel_audio PRIVATE himmel_audio_io avrt ole32 winmm)

foreach(t himmel_audio_dsp himmel_audio_io himmel_audio)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
    endif()
endforeach()

# Unit tests (no hardware): the analysis, the MIDI clock, the block, and the
# engine's music feed + zoom pulse through the FX resolver.
add_executable(audio_tests ${PROJECT_SOURCE_DIR}/tests/audio/audio_tests.cpp)
target_link_libraries(audio_tests PRIVATE himmel_audio_dsp himmel_engine_model)
add_test(NAME audio_tests COMMAND audio_tests)
add_test(NAME himmel_audio_selftest COMMAND himmel_audio --selftest)
if(MSVC)
    target_compile_options(audio_tests PRIVATE /W4 /permissive- /utf-8)
endif()
