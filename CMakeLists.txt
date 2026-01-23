cmake_minimum_required(VERSION 4.0)
project(imgui_fc_visualizer)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_TOOLCHAIN_FILE "E:/SW/html5/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")

add_subdirectory(3rd_party)
# vulkan sdk on NON apple platform
if (NOT APPLE AND NOT EMSCRIPTEN)
    find_package(Vulkan REQUIRED)
endif ()

# Main application with audio visualizer and NES emulator
add_executable(imgui_fc_visualizer
    main.cpp
    impl.cpp
    AudioVisualizer.cpp 
    AudioVisualizer.h
    PianoVisualizer.cpp
    PianoVisualizer.h
    NesEmulator.cpp
    NesEmulator.h
    A2A03Visualizer.cpp
    A2A03Visualizer.h
    # 2A03 chip data from v6502r
    v6502r/src/2a03/segdefs.c
    v6502r/src/2a03/nodenames.c
    # perfect2a03 transistor-level simulator
    v6502r/ext/perfect6502/netlist_sim.c
    v6502r/ext/perfect6502/perfect2a03.c
)
# Link libraries - conditionally include nfd
if (EMSCRIPTEN)
    target_link_libraries(imgui_fc_visualizer PRIVATE sokol imgui game_music_emu agnes TinySoundFont)
    # Define preprocessor macro for Web platform
    target_compile_definitions(imgui_fc_visualizer PRIVATE EMSCRIPTEN_PLATFORM)
    target_compile_options(imgui_fc_visualizer PRIVATE --use-port=emdawnwebgpu -sASSERTIONS)
else()
    target_link_libraries(imgui_fc_visualizer PRIVATE sokol imgui game_music_emu agnes nfd TinySoundFont)
endif()
target_compile_options(imgui_fc_visualizer PUBLIC $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<NOT:$<COMPILE_LANGUAGE:CUDA>>>:/wd4819>)

if (EMSCRIPTEN)
    target_compile_options(imgui_fc_visualizer PRIVATE -pthread -Oz --closure 1)
    target_link_libraries(imgui_fc_visualizer PRIVATE
        -sASSERTIONS
        -sALLOW_MEMORY_GROWTH
        -sUSE_PTHREADS=1
        -sPTHREAD_POOL_SIZE=4
        -sOFFSCREENCANVAS_SUPPORT
        --use-port=emdawnwebgpu
        -sWASM_BIGINT
        -sEXPORTED_FUNCTIONS="['_main','_emscripten_file_selected','_emscripten_check_file_selection']"
        -sEXPORTED_RUNTIME_METHODS="['ccall','cwrap','stringToNewUTF8','UTF8ToString','FS']"
        -sFORCE_FILESYSTEM
        "--closure 1")
endif ()

# Include directories for gme headers and v6502r
target_include_directories(imgui_fc_visualizer PRIVATE 
    ${CMAKE_SOURCE_DIR}/3rd_party/Game_Music_Emu
    ${CMAKE_SOURCE_DIR}/3rd_party
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/v6502r/ext/perfect6502
)

if (NOT APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(imgui_fc_visualizer PRIVATE Vulkan::Vulkan)
endif ()

add_custom_command(
    TARGET imgui_fc_visualizer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ARGS "${CMAKE_CURRENT_SOURCE_DIR}/SoundFont" "${CMAKE_CURRENT_BINARY_DIR}/SoundFont"
)
