cmake_minimum_required(VERSION 4.0)
project(imgui_fc_DAW)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(3rd_party)

add_executable(imgui_fc_DAW main.cpp)
target_link_libraries(imgui_fc_DAW PRIVATE sokol imgui game_music_emu)
