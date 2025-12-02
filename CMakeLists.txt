cmake_minimum_required(VERSION 4.0)
project(imgui_fc_DAW)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(3rd_party)

# Main application with audio visualizer
add_executable(imgui_fc_DAW 
    main.cpp 
    AudioVisualizer.cpp 
    AudioVisualizer.h
)
target_link_libraries(imgui_fc_DAW PRIVATE sokol imgui game_music_emu nfd)

# Include directories for gme headers
target_include_directories(imgui_fc_DAW PRIVATE 
    ${CMAKE_SOURCE_DIR}/3rd_party/Game_Music_Emu
)
