cmake_minimum_required(VERSION 4.0)
project(imgui_fc_visualizer)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(3rd_party)
# vulkan sdk on NON apple platform
if (NOT APPLE)
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
)
target_link_libraries(imgui_fc_visualizer PRIVATE sokol imgui game_music_emu agnes nfd TinySoundFont)
target_compile_options(imgui_fc_visualizer PUBLIC $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<NOT:$<COMPILE_LANGUAGE:CUDA>>>:/wd4819>)

# Include directories for gme headers and v6502r
target_include_directories(imgui_fc_visualizer PRIVATE 
    ${CMAKE_SOURCE_DIR}/3rd_party/Game_Music_Emu
    ${CMAKE_SOURCE_DIR}/3rd_party
    ${CMAKE_SOURCE_DIR}
)

if (NOT APPLE)
    target_link_libraries(imgui_fc_visualizer PRIVATE Vulkan::Vulkan)
endif ()

add_custom_command(
    TARGET imgui_fc_visualizer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ARGS "${CMAKE_CURRENT_SOURCE_DIR}/SoundFont" "${CMAKE_CURRENT_BINARY_DIR}/SoundFont"
)
