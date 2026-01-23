#include "NesEmulator.h"
#include "sokol_app.h"
#include "util/sokol_imgui.h"
#include <cstring>
#include <fstream>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

// NES color palette (NTSC - from Nestopia)
const uint32_t NesEmulator::nes_palette_[64] = {
    0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4, 0xFF5C007E, 0xFF6E0040, 0xFF6C0600, 0xFF561D00,
    0xFF333500, 0xFF0B4800, 0xFF005200, 0xFF004F08, 0xFF00404D, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE, 0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00,
    0xFF6B6D00, 0xFF388700, 0xFF0C9300, 0xFF008F32, 0xFF007C8D, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFFFFEFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF, 0xFFF36AFF, 0xFFFE6ECC, 0xFFFE8170, 0xFFEA9E22,
    0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082, 0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000,
    0xFFFFFEFF, 0xFFC0DFFF, 0xFFD3D2FF, 0xFFE8C8FF, 0xFFFBC2FF, 0xFFFEC4EA, 0xFFFECCC5, 0xFFF7D8A5,
    0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC, 0xFFB5EBF2, 0xFFB8B8B8, 0xFF000000, 0xFF000000
};

NesEmulator::NesEmulator() {
    memset(screen_pixels_, 0, sizeof(screen_pixels_));
    memset(input_, 0, sizeof(input_));
}

NesEmulator::~NesEmulator() {
    if (agnes_) {
        agnes_destroy(agnes_);
        agnes_ = nullptr;
    }
    // destroyScreenTexture();
}

bool NesEmulator::init(long audio_sample_rate) {
    sample_rate_ = audio_sample_rate;
    
    // Create agnes instance
    agnes_ = agnes_make();
    if (!agnes_) {
        return false;
    }
    
    // Set up APU handlers
    agnes_set_apu_handler(agnes_, apuWriteCallback, apuReadCallback, this);
    
    // Initialize APU
    initApu();
    
    // Create screen texture
    createScreenTexture();
    
    return true;
}

void NesEmulator::initApu() {
    // Set up Blip_Buffer
    // At 44100Hz, each frame generates ~735 samples
    // Use 200ms buffer (~12 frames worth) for smooth audio with low latency
    apu_buffer_.set_sample_rate(sample_rate_, 200);
    apu_buffer_.clock_rate(static_cast<long>(CPU_CLOCK_NTSC));
    
    // Set up APU
    apu_.output(&apu_buffer_);
    apu_.dmc_reader(apuDmcReadCallback, this);
    apu_.reset(false);  // NTSC mode
    
    // Set up VRC6 APU (will be enabled if game uses mapper 24/26)
    vrc6_apu_.output(&apu_buffer_);
    vrc6_apu_.reset();
    has_vrc6_ = false;
    
    last_apu_cycle_ = 0;
}

bool NesEmulator::loadROM(const char* path) {
    FILE* file = nullptr;
    
#ifdef _WIN32
    // Convert UTF-8 path to wide string for Windows
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen > 0) {
        std::vector<wchar_t> wpath(wlen);
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);
        file = _wfopen(wpath.data(), L"rb");
    }
#else
    file = fopen(path, "rb");
#endif
    
    if (!file) {
        return false;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (size <= 0) {
        fclose(file);
        return false;
    }
    
    // Read file data
    rom_data_.resize(size);
    size_t read = fread(rom_data_.data(), 1, size, file);
    fclose(file);
    
    if (read != static_cast<size_t>(size)) {
        return false;
    }
    
    rom_path_ = path;
    return loadROMData(rom_data_.data(), rom_data_.size());
}

bool NesEmulator::loadROMData(const void* data, size_t size) {
    if (!agnes_) return false;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Reset APU
    apu_.reset(false);
    vrc6_apu_.reset();
    apu_buffer_.clear();
    last_apu_cycle_ = 0;
    
    // Load ROM into agnes
    if (!agnes_load_ines_data(agnes_, const_cast<void*>(data), size)) {
        rom_loaded_ = false;
        has_vrc6_ = false;
        return false;
    }
    
    // Check if this ROM uses VRC6 mapper (24 or 26)
    // Parse iNES header to get mapper number
    const uint8_t* header = static_cast<const uint8_t*>(data);
    if (size >= 16 && header[0] == 'N' && header[1] == 'E' && header[2] == 'S' && header[3] == 0x1A) {
        uint8_t mapper_num = ((header[6] & 0xF0) >> 4) | (header[7] & 0xF0);
        has_vrc6_ = (mapper_num == 24 || mapper_num == 26);
    } else {
        has_vrc6_ = false;
    }
    
    rom_loaded_ = true;
    running_ = false;
    
    return true;
}

void NesEmulator::reset() {
    if (!agnes_ || !rom_loaded_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Reload the ROM from cached data to reset
    if (!rom_data_.empty()) {
        agnes_load_ines_data(agnes_, rom_data_.data(), rom_data_.size());
    }
    
    // Reset APU
    apu_.reset(false);
    if (has_vrc6_) {
        vrc6_apu_.reset();
    }
    apu_buffer_.clear();
    last_apu_cycle_ = 0;
}

void NesEmulator::runFrame() {
    if (!agnes_ || !rom_loaded_ || !running_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Set input
    agnes_set_input(agnes_, &input_[0], &input_[1]);
    
    // Run one frame of emulation
    agnes_next_frame(agnes_);
    
    // End APU frame to generate audio samples
    endApuFrame();
    
    // Update screen texture
    updateScreenTexture();
}

void NesEmulator::setInput(int player, const agnes_input_t& input) {
    if (player >= 0 && player < 2) {
        input_[player] = input;
    }
}

void NesEmulator::apuWriteCallback(void* user_data, uint16_t addr, uint8_t val, uint64_t cpu_cycle) {
    NesEmulator* emu = static_cast<NesEmulator*>(user_data);
    if (!emu) return;
    
    // Sync APU to current cycle
    emu->syncApu(cpu_cycle);
    
    nes_time_t time = static_cast<nes_time_t>(cpu_cycle - emu->last_apu_cycle_);
    
    // Check if this is a VRC6 register write
    if (emu->has_vrc6_) {
        // VRC6 Pulse 1: $9000-$9002
        if (addr >= 0x9000 && addr <= 0x9002) {
            emu->vrc6_apu_.write_osc(time, 0, addr & 0x3, val);
            return;
        }
        // VRC6 Pulse 2: $A000-$A002
        if (addr >= 0xA000 && addr <= 0xA002) {
            emu->vrc6_apu_.write_osc(time, 1, addr & 0x3, val);
            return;
        }
        // VRC6 Saw: $B000-$B002
        if (addr >= 0xB000 && addr <= 0xB002) {
            emu->vrc6_apu_.write_osc(time, 2, addr & 0x3, val);
            return;
        }
    }
    
    // Standard APU register write
    emu->apu_.write_register(time, addr, val);
}

uint8_t NesEmulator::apuReadCallback(void* user_data, uint16_t addr, uint64_t cpu_cycle) {
    NesEmulator* emu = static_cast<NesEmulator*>(user_data);
    if (!emu) return 0;
    
    // Sync APU to current cycle
    emu->syncApu(cpu_cycle);
    
    // Read APU status (0x4015)
    if (addr == 0x4015) {
        return emu->apu_.read_status(static_cast<nes_time_t>(cpu_cycle - emu->last_apu_cycle_));
    }
    
    return 0;
}

int NesEmulator::apuDmcReadCallback(void* user_data, unsigned addr) {
    NesEmulator* emu = static_cast<NesEmulator*>(user_data);
    if (!emu || !emu->agnes_) return 0;
    
    // Read from ROM via agnes CPU
    // DMC reads from $8000-$FFFF
    // We need to access agnes memory - for now return 0
    // TODO: Implement proper DMC sample reading
    return 0;
}

void NesEmulator::syncApu(uint64_t cpu_cycle) {
    // APU sync is handled through write_register timing
}

void NesEmulator::endApuFrame() {
    // Now handled inline in generateAudioSamples() for better timing
    uint64_t current_cycle = agnes_get_cpu_cycles(agnes_);
    nes_time_t frame_length = static_cast<nes_time_t>(current_cycle - last_apu_cycle_);
    
    apu_.end_frame(frame_length);
    if (has_vrc6_) {
        vrc6_apu_.end_frame(frame_length);
    }
    apu_buffer_.end_frame(frame_length);
    
    last_apu_cycle_ = current_cycle;
}

long NesEmulator::samplesAvailable() const {
    return apu_buffer_.samples_avail();
}

int NesEmulator::readAudioSamples(short* buffer, int max_samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Just read from buffer - emulation is driven by main thread
    long available = apu_buffer_.samples_avail();
    if (available <= 0) return 0;
    
    int to_read = min(static_cast<int>(available), max_samples);
    return static_cast<int>(apu_buffer_.read_samples(buffer, to_read));
}

void NesEmulator::getApuState(int* periods, int* lengths, int* amplitudes) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (int i = 0; i < 5; ++i) {
        periods[i] = apu_.osc_period(i);
        lengths[i] = apu_.osc_length(i);
        amplitudes[i] = apu_.osc_amplitude(i);
    }
}

void NesEmulator::getVRC6State(int* periods, int* volumes, bool* enabled) {
    if (!has_vrc6_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (int i = 0; i < 3; ++i) {
        periods[i] = vrc6_apu_.osc_period(i);
        volumes[i] = vrc6_apu_.osc_volume(i);
        enabled[i] = vrc6_apu_.osc_enabled(i);
    }
}

void NesEmulator::createScreenTexture() {
    if (texture_created_) return;
    
    // Create image with stream update usage (new sokol API)
    sg_image_desc img_desc = {};
    img_desc.width = AGNES_SCREEN_WIDTH;
    img_desc.height = AGNES_SCREEN_HEIGHT;
    img_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    img_desc.usage.stream_update = true;  // New sokol API for stream updates
    
    screen_texture_ = sg_make_image(&img_desc);
    
    // Create sampler for nearest-neighbor filtering (pixel-perfect look)
    sg_sampler_desc smp_desc = {};
    smp_desc.min_filter = SG_FILTER_NEAREST;
    smp_desc.mag_filter = SG_FILTER_NEAREST;
    smp_desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    smp_desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    
    screen_sampler_ = sg_make_sampler(&smp_desc);
    
    // Create texture view for ImGui binding
    sg_view_desc view_desc = {};
    view_desc.texture.image = screen_texture_;
    
    screen_view_ = sg_make_view(&view_desc);
    
    texture_created_ = true;
}

void NesEmulator::destroyScreenTexture() {
    if (texture_created_) {
        sg_destroy_view(screen_view_);
        sg_destroy_sampler(screen_sampler_);
        sg_destroy_image(screen_texture_);
        texture_created_ = false;
    }
}

void NesEmulator::updateScreenTexture() {
    if (!agnes_ || !texture_created_) return;
    
    // Convert agnes screen buffer to RGBA pixels
    for (int y = 0; y < AGNES_SCREEN_HEIGHT; ++y) {
        for (int x = 0; x < AGNES_SCREEN_WIDTH; ++x) {
            agnes_color_t color = agnes_get_screen_pixel(agnes_, x, y);
            screen_pixels_[y * AGNES_SCREEN_WIDTH + x] = 
                (color.a << 24) | (color.b << 16) | (color.g << 8) | color.r;
        }
    }
    
    // Upload to GPU texture
    sg_image_data data = {};
    data.mip_levels[0].ptr = screen_pixels_;
    data.mip_levels[0].size = sizeof(screen_pixels_);
    sg_update_image(screen_texture_, &data);
}

void NesEmulator::drawScreen(float scale) {
    if (!texture_created_) return;
    
    float width = static_cast<float>(AGNES_SCREEN_WIDTH) * scale;
    float height = static_cast<float>(AGNES_SCREEN_HEIGHT) * scale;
    
    // Get ImTextureID from view and sampler using sokol_imgui helper
    uint64_t imtex_id = simgui_imtextureid_with_sampler(screen_view_, screen_sampler_);
    
    // Draw the texture using ImGui
    ImGui::Image(imtex_id, ImVec2(width, height));
}

uint64_t NesEmulator::getCpuCycles() const {
    if (!agnes_) return 0;
    return agnes_get_cpu_cycles(agnes_);
}

int NesEmulator::getCurrentScanline() const {
    // TODO: Expose scanline from agnes if needed
    return 0;
}

NesEmulator::CpuState NesEmulator::getCpuState() const {
    CpuState state = {};
    if (agnes_) {
        agnes_cpu_state_t agnes_state;
        agnes_get_cpu_state(agnes_, &agnes_state);
        state.a = agnes_state.a;
        state.x = agnes_state.x;
        state.y = agnes_state.y;
        state.sp = agnes_state.sp;
        state.p = agnes_state.p;
        state.pc = agnes_state.pc;
    }
    return state;
}

NesEmulator::ApuState NesEmulator::getApuState() const {
    ApuState state = {};
    // Get APU output levels using osc_amplitude
    // Note: osc_amplitude returns values in different ranges for different channels
    // Square: 0-15, Triangle: 0-15, Noise: 0-15, DMC: 0-127
    state.sq0_out = static_cast<uint8_t>(std::abs(apu_.osc_amplitude(0)) & 0x0F);
    state.sq1_out = static_cast<uint8_t>(std::abs(apu_.osc_amplitude(1)) & 0x0F);
    state.tri_out = static_cast<uint8_t>(std::abs(apu_.osc_amplitude(2)) & 0x0F);
    state.noi_out = static_cast<uint8_t>(std::abs(apu_.osc_amplitude(3)) & 0x0F);
    state.pcm_out = static_cast<uint8_t>(std::abs(apu_.osc_amplitude(4)) & 0x7F);
    
    return state;
}

bool NesEmulator::saveState(std::vector<uint8_t>& out_state) {
    if (!agnes_ || !rom_loaded_) return false;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t state_size = agnes_state_size();
    out_state.resize(state_size);
    
    agnes_dump_state(agnes_, reinterpret_cast<agnes_state_t*>(out_state.data()));
    return true;
}

bool NesEmulator::loadState(const std::vector<uint8_t>& state) {
    if (!agnes_ || !rom_loaded_) return false;
    if (state.size() < agnes_state_size()) return false;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    return agnes_restore_state(agnes_, reinterpret_cast<const agnes_state_t*>(state.data()));
}

uint8_t NesEmulator::readRomByte(uint16_t addr) const {
    if (!agnes_ || !rom_loaded_) return 0;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    return agnes_read_rom_byte(agnes_, addr);
}
