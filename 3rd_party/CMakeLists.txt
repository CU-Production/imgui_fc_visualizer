# sokol
add_library(sokol INTERFACE)
target_include_directories(sokol INTERFACE sokol)

# imgui
add_library(imgui
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp)
target_include_directories(imgui PUBLIC imgui)

# Game_Music_Emu - NES music emulator library
add_library(game_music_emu STATIC
    # Core library files
    Game_Music_Emu/gme/gme.cpp
    Game_Music_Emu/gme/Music_Emu.cpp
    Game_Music_Emu/gme/Gme_File.cpp
    Game_Music_Emu/gme/Data_Reader.cpp
    Game_Music_Emu/gme/Classic_Emu.cpp
    Game_Music_Emu/gme/M3u_Playlist.cpp
    
    # Audio buffer and effects
    Game_Music_Emu/gme/Blip_Buffer.cpp
    Game_Music_Emu/gme/Multi_Buffer.cpp
    Game_Music_Emu/gme/Effects_Buffer.cpp
    Game_Music_Emu/gme/Dual_Resampler.cpp
    Game_Music_Emu/gme/Fir_Resampler.cpp
    
    # NSF/NSFE emulator (NES)
    Game_Music_Emu/gme/Nsf_Emu.cpp
    Game_Music_Emu/gme/Nsfe_Emu.cpp
    Game_Music_Emu/gme/Nes_Apu.cpp
    Game_Music_Emu/gme/Nes_Cpu.cpp
    Game_Music_Emu/gme/Nes_Oscs.cpp
    Game_Music_Emu/gme/Nes_Vrc6_Apu.cpp
    Game_Music_Emu/gme/Nes_Fme7_Apu.cpp
    Game_Music_Emu/gme/Nes_Namco_Apu.cpp
    
    # Other emulators (optional, but included for completeness)
    Game_Music_Emu/gme/Ay_Emu.cpp
    Game_Music_Emu/gme/Ay_Apu.cpp
    Game_Music_Emu/gme/Ay_Cpu.cpp
    Game_Music_Emu/gme/Gbs_Emu.cpp
    Game_Music_Emu/gme/Gb_Apu.cpp
    Game_Music_Emu/gme/Gb_Cpu.cpp
    Game_Music_Emu/gme/Gb_Oscs.cpp
    Game_Music_Emu/gme/Gym_Emu.cpp
    Game_Music_Emu/gme/Hes_Emu.cpp
    Game_Music_Emu/gme/Hes_Apu.cpp
    Game_Music_Emu/gme/Hes_Cpu.cpp
    Game_Music_Emu/gme/Kss_Emu.cpp
    Game_Music_Emu/gme/Kss_Cpu.cpp
    Game_Music_Emu/gme/Kss_Scc_Apu.cpp
    Game_Music_Emu/gme/Sap_Emu.cpp
    Game_Music_Emu/gme/Sap_Apu.cpp
    Game_Music_Emu/gme/Sap_Cpu.cpp
    Game_Music_Emu/gme/Spc_Emu.cpp
    Game_Music_Emu/gme/Snes_Spc.cpp
    Game_Music_Emu/gme/Spc_Cpu.cpp
    Game_Music_Emu/gme/Spc_Dsp.cpp
    Game_Music_Emu/gme/Vgm_Emu.cpp
    Game_Music_Emu/gme/Vgm_Emu_Impl.cpp
    Game_Music_Emu/gme/Ym2413_Emu.cpp
    Game_Music_Emu/gme/Ym2612_Emu.cpp
    Game_Music_Emu/gme/Sms_Apu.cpp)
target_include_directories(game_music_emu PUBLIC Game_Music_Emu)

# agnes - NES emulator core (CPU/PPU/Mappers)
add_library(agnes STATIC
    agnes/agnes.c
)
target_include_directories(agnes PUBLIC agnes)

# nfd - skip on Emscripten (Web platform doesn't support native file dialogs)
if (NOT EMSCRIPTEN)
    add_subdirectory(nativefiledialog-extended)
endif()

# TinySoundFont
add_library(TinySoundFont INTERFACE)
target_include_directories(TinySoundFont INTERFACE TinySoundFont)
