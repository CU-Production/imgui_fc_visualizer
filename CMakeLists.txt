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
)
target_link_libraries(imgui_fc_visualizer PRIVATE sokol imgui game_music_emu agnes nfd)
target_compile_options(imgui_fc_visualizer PUBLIC $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<NOT:$<COMPILE_LANGUAGE:CUDA>>>:/wd4819>)

# Include directories for gme headers
target_include_directories(imgui_fc_visualizer PRIVATE 
    ${CMAKE_SOURCE_DIR}/3rd_party/Game_Music_Emu
    ${CMAKE_SOURCE_DIR}/3rd_party
)

if (NOT APPLE)
    target_link_libraries(imgui_fc_visualizer PRIVATE Vulkan::Vulkan)
endif ()
