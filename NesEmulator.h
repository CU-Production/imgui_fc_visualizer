#pragma once

#include "agnes/agnes.h"
#include "gme/Nes_Apu.h"
#include "gme/Nes_Vrc6_Apu.h"
#include "gme/Blip_Buffer.h"
#include "sokol_gfx.h"
#include "imgui.h"

#include <vector>
#include <string>
#include <mutex>
#include <atomic>

// NES Emulator class that integrates agnes (CPU/PPU) with gme's Nes_Apu
class NesEmulator {
public:
    NesEmulator();
    ~NesEmulator();

    // Initialize the emulator
    bool init(long audio_sample_rate);
    
    // Load a ROM file
    bool loadROM(const char* path);
    bool loadROMData(const void* data, size_t size);
    
    // Emulation control
    void reset();
    void runFrame();
    void pause() { running_ = false; }
    void resume() { running_ = true; }
    bool isRunning() const { return running_; }
    bool isLoaded() const { return rom_loaded_; }
    
    // Input
    void setInput(int player, const agnes_input_t& input);
    
    // Audio - read samples from buffer (does NOT run emulation)
    int readAudioSamples(short* buffer, int max_samples);
    
    // Get APU data for visualization
    void getApuState(int* periods, int* lengths, int* amplitudes);
    
    // VRC6 expansion support
    bool hasVRC6() const { return has_vrc6_; }
    void getVRC6State(int* periods, int* volumes, bool* enabled);
    
    // Get samples available in buffer
    long samplesAvailable() const;
    
    // Video - get screen texture for rendering
    sg_image getScreenTexture() const { return screen_texture_; }
    void updateScreenTexture();
    
    // Draw emulator screen in ImGui window
    void drawScreen(float scale = 2.0f);
    
    // State
    uint64_t getCpuCycles() const;
    int getCurrentScanline() const;
    
    // CPU state for visualization
    struct CpuState {
        uint8_t a;      // Accumulator
        uint8_t x;      // X register
        uint8_t y;      // Y register
        uint8_t sp;     // Stack pointer
        uint8_t p;      // Status register
        uint16_t pc;    // Program counter
    };
    CpuState getCpuState() const;
    
    // APU state for visualization
    struct ApuState {
        uint8_t sq0_out;    // Square 1 output (0-15)
        uint8_t sq1_out;    // Square 2 output (0-15)
        uint8_t tri_out;    // Triangle output (0-15)
        uint8_t noi_out;    // Noise output (0-15)
        uint8_t pcm_out;    // DMC/PCM output (0-127)
    };
    ApuState getApuState() const;
    
    // Save/Load state
    bool saveState(std::vector<uint8_t>& out_state);
    bool loadState(const std::vector<uint8_t>& state);

private:
    // Agnes (CPU/PPU/Mappers)
    agnes_t* agnes_ = nullptr;
    
    // APU (from gme)
    Nes_Apu apu_;
    Nes_Vrc6_Apu vrc6_apu_;
    Blip_Buffer apu_buffer_;
    long sample_rate_ = 44100;
    bool has_vrc6_ = false;
    
    // APU timing
    uint64_t last_apu_cycle_ = 0;
    static constexpr double CPU_CLOCK_NTSC = 1789773.0;
    static constexpr int CYCLES_PER_FRAME = 29780;  // ~60fps NTSC
    
    // Screen texture (new sokol API uses image + view + sampler)
    sg_image screen_texture_;
    sg_view screen_view_;
    sg_sampler screen_sampler_;
    uint32_t screen_pixels_[AGNES_SCREEN_WIDTH * AGNES_SCREEN_HEIGHT];
    bool texture_created_ = false;
    
    // State
    std::atomic<bool> running_{false};
    bool rom_loaded_ = false;
    std::string rom_path_;
    std::vector<uint8_t> rom_data_;
    
    // Input
    agnes_input_t input_[2] = {};
    
    // Thread safety
    std::mutex mutex_;
    
    // APU callback functions (static, called by agnes)
    static void apuWriteCallback(void* user_data, uint16_t addr, uint8_t val, uint64_t cpu_cycle);
    static uint8_t apuReadCallback(void* user_data, uint16_t addr, uint64_t cpu_cycle);
    static int apuDmcReadCallback(void* user_data, unsigned addr);
    
    // Internal helpers
    void initApu();
    void syncApu(uint64_t cpu_cycle);
    void endApuFrame();
    void createScreenTexture();
    void destroyScreenTexture();
    
    // NES color palette (NTSC)
    static const uint32_t nes_palette_[64];
};
