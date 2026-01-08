#if defined(__APPLE__)
#define SOKOL_METAL
#else
#define SOKOL_VULKAN
#endif
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_glue.h"
#include "sokol_audio.h"
#include <vector>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <atomic>

#include "imgui.h"
#include "util/sokol_imgui.h"

// Game_Music_Emu for NSF file support
#include "gme/gme.h"
#include "gme/Nsf_Emu.h"
#include "gme/Nes_Apu.h"
#include "gme/Nes_Vrc6_Apu.h"

// Native File Dialog for file selection
#include "nfd.h"

// Audio Visualizer
#include "AudioVisualizer.h"

// Piano Visualizer
#include "PianoVisualizer.h"

// NES Emulator
#include "NesEmulator.h"

#include <cctype>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

// UTF-8 file reading helper for Windows
// Returns true if file was read successfully, data is stored in out_data
static bool read_file_utf8(const char* path, std::vector<uint8_t>& out_data) {
    out_data.clear();
    
#ifdef _WIN32
    // Convert UTF-8 path to wide string for Windows
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen <= 0) return false;
    
    std::vector<wchar_t> wpath(wlen);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);
    
    // Open file with wide path
    FILE* file = _wfopen(wpath.data(), L"rb");
    if (!file) return false;
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (size <= 0) {
        fclose(file);
        return false;
    }
    
    // Read file data
    out_data.resize(size);
    size_t read = fread(out_data.data(), 1, size, file);
    fclose(file);
    
    return read == static_cast<size_t>(size);
#else
    // On non-Windows, use standard fopen (most Unix systems handle UTF-8 natively)
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (size <= 0) {
        fclose(file);
        return false;
    }
    
    out_data.resize(size);
    size_t read = fread(out_data.data(), 1, size, file);
    fclose(file);
    
    return read == static_cast<size_t>(size);
#endif
}

// Helper function to check file extension (case-insensitive)
static bool has_extension(const char* path, const char* ext) {
    if (!path || !ext) return false;
    size_t path_len = strlen(path);
    size_t ext_len = strlen(ext);
    if (path_len < ext_len + 1) return false;
    
    const char* file_ext = path + path_len - ext_len;
    if (file_ext[-1] != '.') return false;
    
    for (size_t i = 0; i < ext_len; i++) {
        if (tolower((unsigned char)file_ext[i]) != tolower((unsigned char)ext[i])) {
            return false;
        }
    }
    return true;
}

static bool show_demo_window = false;
static bool show_visualizer = true;
static bool show_piano = true;
static bool show_emulator = false;

// Application mode: NSF Player or NES Emulator
enum class AppMode {
    NSF_PLAYER,
    NES_EMULATOR
};
static AppMode current_mode = AppMode::NSF_PLAYER;

// Keyboard state tracking for NES input
static bool key_states[512] = {};  // Track key press states

// Mutex for protecting audio operations
static std::mutex audio_mutex;

// application state
static struct {
    sg_pass_action pass_action;
    
    // Game_Music_Emu state
    Music_Emu* emu = nullptr;
    std::atomic<bool> is_playing{false};
    int current_track = 0;
    int track_count = 0;
    char loaded_file[512] = "";
    char error_msg[512] = "";
    
    // Audio state
    bool audio_initialized = false;
    const long sample_rate = 44100;
    
    // Playback info
    float tempo = 1.0f;
    float volume_db = 0.0f;
    
    // Seek request (set by UI thread, processed by audio thread)
    std::atomic<long> seek_request{-1};  // -1 means no seek requested
    
    // Audio visualizer
    AudioVisualizer visualizer;
    
    // Piano visualizer
    PianoVisualizer piano;
    
    // Playback time in seconds
    std::atomic<float> playback_time{0.0f};
    
    // Piano preprocessing state
    std::atomic<bool> preprocessing{false};
    std::atomic<float> preprocess_progress{0.0f};
    
    // Audio buffer for visualization (double buffered)
    std::vector<short> viz_buffer;
    int viz_buffer_pos = 0;
    
    // NES Emulator
    NesEmulator nes_emu;
    bool nes_rom_loaded = false;
    agnes_input_t nes_input = {};  // Current controller input
    float nes_screen_scale = 2.0f;
} state;

// Audio stream callback - called from audio thread
void audio_stream_callback(float* buffer, int num_frames, int num_channels, void* user_data) {
    const int num_samples = num_frames * num_channels;
    
    // Handle NES Emulator mode
    if (current_mode == AppMode::NES_EMULATOR && state.nes_emu.isRunning()) {
        static std::vector<short> nes_temp_buffer;
        
        // NES APU outputs mono, we need num_frames mono samples
        nes_temp_buffer.resize(num_frames);
        
        // Read audio samples from emulator (mono)
        int samples_read = state.nes_emu.readAudioSamples(nes_temp_buffer.data(), num_frames);
        
        // If we got fewer samples than needed, fill the rest with silence
        for (int i = samples_read; i < num_frames; ++i) {
            nes_temp_buffer[i] = 0;
        }
        
        // Update visualizer with audio data (convert mono to stereo for visualizer)
        static std::vector<short> stereo_buffer;
        stereo_buffer.resize(num_samples);
        for (int i = 0; i < num_frames; ++i) {
            stereo_buffer[i * 2] = nes_temp_buffer[i];
            stereo_buffer[i * 2 + 1] = nes_temp_buffer[i];
        }
        state.visualizer.updateAudioData(stereo_buffer.data(), num_samples);
        
        // Update piano visualizer and channel levels with APU data
        int periods[5], lengths[5], amplitudes[5];
        state.nes_emu.getApuState(periods, lengths, amplitudes);
        state.visualizer.updateChannelAmplitudesFromAPU(amplitudes, lengths);
        float current_time = static_cast<float>(state.nes_emu.getCpuCycles()) / 1789773.0f;
        state.piano.updateFromAPU(periods, lengths, amplitudes, current_time);
        
        // VRC6 expansion support for NES emulator
        if (state.nes_emu.hasVRC6()) {
            state.visualizer.setVRC6Enabled(true);
            state.piano.setVRC6Enabled(true);
            
            int vrc6_periods[3], vrc6_volumes[3];
            bool vrc6_enabled[3];
            state.nes_emu.getVRC6State(vrc6_periods, vrc6_volumes, vrc6_enabled);
            
            int vrc6_amplitudes[3];
            for (int i = 0; i < 3; ++i) {
                vrc6_amplitudes[i] = vrc6_enabled[i] ? vrc6_volumes[i] : 0;
            }
            state.visualizer.updateVRC6ChannelAmplitudes(vrc6_amplitudes);
            state.piano.updateFromVRC6(vrc6_periods, vrc6_volumes, vrc6_enabled, current_time);
        } else {
            state.visualizer.setVRC6Enabled(false);
            state.piano.setVRC6Enabled(false);
        }
        
        // Convert mono to stereo float output
        float volume_linear = std::pow(10.0f, state.volume_db / 20.0f);
        for (int i = 0; i < num_frames; ++i) {
            float sample = (nes_temp_buffer[i] / 32768.0f) * volume_linear;
            buffer[i * 2] = sample;      // Left channel
            buffer[i * 2 + 1] = sample;  // Right channel
        }
        return;
    }
    
    // Handle NSF Player mode
    if (!state.emu || !state.is_playing.load()) {
        // Fill with silence
        std::fill(buffer, buffer + num_samples, 0.0f);
        return;
    }
    
    std::lock_guard<std::mutex> lock(audio_mutex);
    
    // Process seek request if any
    long seek_pos = state.seek_request.exchange(-1);
    if (seek_pos >= 0 && state.emu) {
        gme_seek(state.emu, seek_pos);
    }
    
    // Game_Music_Emu generates 16-bit signed samples (stereo)
    static std::vector<short> temp_buffer;
    temp_buffer.resize(num_samples);
    
    // Generate samples from Game_Music_Emu
    gme_err_t err = gme_play(state.emu, num_samples, temp_buffer.data());
    if (err) {
        std::fill(buffer, buffer + num_samples, 0.0f);
        return;
    }
    
    // Update visualizer with audio data
    state.visualizer.updateAudioData(temp_buffer.data(), num_samples);
    
    // Update playback time
    float current_time = gme_tell(state.emu) / 1000.0f;
    state.playback_time.store(current_time);
    
    // Update piano visualizer and channel levels with APU data
    // Try to get APU data directly from Nsf_Emu
    Nsf_Emu* nsf = dynamic_cast<Nsf_Emu*>(state.emu);
    if (nsf) {
        Nes_Apu* apu = nsf->apu_();
        if (apu) {
            int periods[5], lengths[5], amplitudes[5];
            for (int i = 0; i < 5; ++i) {
                periods[i] = apu->osc_period(i);
                lengths[i] = apu->osc_length(i);
                amplitudes[i] = apu->osc_amplitude(i);
            }
            state.visualizer.updateChannelAmplitudesFromAPU(amplitudes, lengths);
            state.piano.updateFromAPU(periods, lengths, amplitudes, current_time);
        }
        
        // VRC6 expansion chip support
        Nes_Vrc6_Apu* vrc6 = nsf->vrc6_();
        if (vrc6) {
            // Enable VRC6 mode in visualizers
            state.visualizer.setVRC6Enabled(true);
            state.piano.setVRC6Enabled(true);
            
            // Get VRC6 channel data
            int vrc6_amplitudes[3];
            int vrc6_periods[3];
            int vrc6_volumes[3];
            bool vrc6_enabled[3];
            for (int i = 0; i < 3; ++i) {
                vrc6_periods[i] = vrc6->osc_period(i);
                vrc6_amplitudes[i] = vrc6->osc_amplitude(i);
                vrc6_volumes[i] = vrc6->osc_volume(i);
                vrc6_enabled[i] = vrc6->osc_enabled(i);
            }
            state.visualizer.updateVRC6ChannelAmplitudes(vrc6_amplitudes);
            state.piano.updateFromVRC6(vrc6_periods, vrc6_volumes, vrc6_enabled, current_time);
        } else {
            state.visualizer.setVRC6Enabled(false);
            state.piano.setVRC6Enabled(false);
        }
    }
    
    // Convert 16-bit signed integer to 32-bit float (-1.0 to 1.0)
    // Apply volume control
    float volume_linear = std::pow(10.0f, state.volume_db / 20.0f);
    for (int i = 0; i < num_samples; i++) {
        buffer[i] = (temp_buffer[i] / 32768.0f) * volume_linear;
    }
}

// Helper to get APU from emulator
Nes_Apu* getApuFromEmu(Music_Emu* emu) {
    Nsf_Emu* nsf = dynamic_cast<Nsf_Emu*>(emu);
    if (nsf) {
        return nsf->apu_();
    }
    return nullptr;
}

// Preprocess current track for piano visualization
void preprocess_piano_track() {
    if (!state.emu || state.preprocessing.load()) return;
    
    state.preprocessing.store(true);
    state.preprocess_progress.store(0.0f);
    
    // Load file data with UTF-8 path support
    std::vector<uint8_t> file_data;
    if (!read_file_utf8(state.loaded_file, file_data)) {
        state.preprocessing.store(false);
        return;
    }
    
    // Create a separate emulator instance for preprocessing
    Music_Emu* preprocess_emu = nullptr;
    gme_err_t err = gme_open_data(file_data.data(), static_cast<long>(file_data.size()), 
                                   &preprocess_emu, state.sample_rate);
    
    if (err || !preprocess_emu) {
        state.preprocessing.store(false);
        return;
    }
    
    // Preprocess the track
    state.piano.preprocessTrack(
        preprocess_emu, 
        state.current_track, 
        state.sample_rate,
        [](Music_Emu* emu) -> Nes_Apu* {
            Nsf_Emu* nsf = dynamic_cast<Nsf_Emu*>(emu);
            return nsf ? nsf->apu_() : nullptr;
        },
        [](float progress) {
            state.preprocess_progress.store(progress);
        },
        [](Music_Emu* emu) -> Nes_Vrc6_Apu* {
            Nsf_Emu* nsf = dynamic_cast<Nsf_Emu*>(emu);
            return nsf ? nsf->vrc6_() : nullptr;
        }
    );
    
    // Cleanup preprocessing emulator
    gme_delete(preprocess_emu);
    
    state.preprocessing.store(false);
    state.preprocess_progress.store(1.0f);
}

// Safe track start - can be called from UI thread
void safe_start_track(int track) {
    if (!state.emu) return;
    
    // Request the audio thread to start the track
    state.is_playing.store(false);  // Pause playback
    
    std::lock_guard<std::mutex> lock(audio_mutex);
    state.seek_request.store(-1);  // Clear any pending seek
    gme_start_track(state.emu, track);
    state.is_playing.store(true);  // Resume playback
}

// Start track and preprocess for piano
void start_track_with_preprocess(int track) {
    state.current_track = track;
    
    // Preprocess first (this will use a separate emulator)
    preprocess_piano_track();
    
    // Then start playback
    safe_start_track(track);
}

void load_nsf_file(const char* path) {
    // Stop playback first
    state.is_playing.store(false);
    
    // Wait for audio thread to stop using the emulator
    std::lock_guard<std::mutex> lock(audio_mutex);
    
    // Clean up previous emulator
    if (state.emu) {
        gme_delete(state.emu);
        state.emu = nullptr;
    }
    
    // Reset seek request
    state.seek_request.store(-1);
    
    // Load file with UTF-8 path support
    std::vector<uint8_t> file_data;
    if (!read_file_utf8(path, file_data)) {
        strncpy(state.error_msg, "Failed to open file", sizeof(state.error_msg) - 1);
        state.error_msg[sizeof(state.error_msg) - 1] = '\0';
        return;
    }
    
    // Load from memory data
    gme_err_t err = gme_open_data(file_data.data(), static_cast<long>(file_data.size()), 
                                   &state.emu, state.sample_rate);
    if (err) {
        strncpy(state.error_msg, err, sizeof(state.error_msg) - 1);
        state.error_msg[sizeof(state.error_msg) - 1] = '\0';
        return;
    }
    
    // Get track info
    state.track_count = gme_track_count(state.emu);
    state.current_track = 0;
    state.error_msg[0] = '\0';
    strncpy(state.loaded_file, path, sizeof(state.loaded_file) - 1);
    state.loaded_file[sizeof(state.loaded_file) - 1] = '\0';
    
    // Initialize visualizer with new emulator
    state.visualizer.init(state.emu, state.sample_rate);
    
    // Reset piano visualizer and preprocess
    state.piano.reset();
    state.playback_time.store(0.0f);
    
    // Apply current settings
    gme_set_tempo(state.emu, state.tempo);
    gme_mute_voices(state.emu, state.visualizer.getMuteMask());
}

// Called after load to preprocess piano data (call without holding audio_mutex)
void postload_preprocess() {
    preprocess_piano_track();
}

// Load NES ROM file
void load_nes_rom(const char* path) {
    if (state.nes_emu.loadROM(path)) {
        state.nes_rom_loaded = true;
        current_mode = AppMode::NES_EMULATOR;
        show_emulator = true;
        
        // Reset visualizers for emulator mode
        state.visualizer.reset();
        state.piano.reset();
    } else {
        strncpy(state.error_msg, "Failed to load NES ROM", sizeof(state.error_msg) - 1);
    }
}

// Draw NES Emulator window
void draw_emulator_window(bool* p_open) {
    ImGui::SetNextWindowSize(ImVec2(540, 540), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("NES Emulator", p_open, ImGuiWindowFlags_MenuBar)) {
        // Menu bar
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open ROM...", "Ctrl+R")) {
                    nfdu8filteritem_t filterItem[2];
                    filterItem[0].name = "NES ROM Files";
                    filterItem[0].spec = "nes";
                    filterItem[1].name = "All Files";
                    filterItem[1].spec = "*";
                    
                    nfdu8char_t* outPath = nullptr;
                    nfdresult_t result = NFD_OpenDialogU8(&outPath, filterItem, 2, nullptr);
                    
                    if (result == NFD_OKAY) {
                        load_nes_rom(outPath);
                        NFD_FreePathU8(outPath);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Close ROM")) {
                    state.nes_emu.pause();
                    state.nes_rom_loaded = false;
                    current_mode = AppMode::NSF_PLAYER;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Emulation")) {
                bool running = state.nes_emu.isRunning();
                if (ImGui::MenuItem(running ? "Pause" : "Resume", "P")) {
                    if (running) state.nes_emu.pause();
                    else state.nes_emu.resume();
                }
                if (ImGui::MenuItem("Reset", "F5")) {
                    state.nes_emu.reset();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::SliderFloat("Scale", &state.nes_screen_scale, 1.0f, 4.0f, "%.1fx");
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        
        if (state.nes_rom_loaded) {
            // Emulation controls
            ImGui::BeginGroup();
            {
                bool running = state.nes_emu.isRunning();
                
                if (ImGui::Button(running ? "Pause" : "Play", ImVec2(60, 25))) {
                    if (running) state.nes_emu.pause();
                    else state.nes_emu.resume();
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset", ImVec2(60, 25))) {
                    state.nes_emu.reset();
                }
                ImGui::SameLine();
                
                // Status
                if (running) {
                    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Running");
                } else {
                    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.3f, 1.0f), "Paused");
                }
            }
            ImGui::EndGroup();
            
            ImGui::Separator();
            
            // Display the NES screen
            // Center the screen in the available space
            ImVec2 content_size = ImGui::GetContentRegionAvail();
            float screen_width = AGNES_SCREEN_WIDTH * state.nes_screen_scale;
            float screen_height = AGNES_SCREEN_HEIGHT * state.nes_screen_scale;
            
            float offset_x = (content_size.x - screen_width) * 0.5f;
            if (offset_x > 0) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset_x);
            }
            
            // Draw the emulator screen
            state.nes_emu.drawScreen(state.nes_screen_scale);
            
            ImGui::Separator();
            
            // Controller info
            ImGui::Text("Controls: Arrow Keys = D-Pad, Z = A, X = B, Enter = Start, BackSpace = Select");
        } else {
            // No ROM loaded
            ImVec2 content_size = ImGui::GetContentRegionAvail();
            ImVec2 text_size = ImGui::CalcTextSize("Load a NES ROM to start");
            ImGui::SetCursorPos(ImVec2(
                (content_size.x - text_size.x) * 0.5f,
                (content_size.y - text_size.y) * 0.5f
            ));
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f), "Load a NES ROM to start");
        }
    }
    ImGui::End();
}

// Update NES controller input from keyboard
void update_nes_input() {
    // Reset input
    memset(&state.nes_input, 0, sizeof(state.nes_input));
    
    // D-Pad
    state.nes_input.up = key_states[SAPP_KEYCODE_UP];
    state.nes_input.down = key_states[SAPP_KEYCODE_DOWN];
    state.nes_input.left = key_states[SAPP_KEYCODE_LEFT];
    state.nes_input.right = key_states[SAPP_KEYCODE_RIGHT];
    
    // Buttons
    state.nes_input.a = key_states[SAPP_KEYCODE_Z];
    state.nes_input.b = key_states[SAPP_KEYCODE_X];
    state.nes_input.start = key_states[SAPP_KEYCODE_ENTER];
    state.nes_input.select = key_states[SAPP_KEYCODE_BACKSPACE];
    
    // Set input to emulator
    state.nes_emu.setInput(0, state.nes_input);
}

void init(void) {
    sg_desc _sg_desc{};
    _sg_desc.environment = sglue_environment();
    _sg_desc.logger.func = slog_func;
    sg_setup(_sg_desc);

    simgui_desc_t simgui_desc = { };
    simgui_desc.logger.func = slog_func;
    simgui_desc.no_default_font = true; // Disable default font to load custom font
    simgui_setup(&simgui_desc);

    // Load font with Chinese/Japanese support
    ImGuiIO& io = ImGui::GetIO();

    // Try to load system fonts that support CJK characters
    ImFont* font = nullptr;

#ifdef _WIN32
    // Try Windows system fonts that support Chinese/Japanese
    const char* font_paths[] = {
        "C:/Windows/Fonts/msyh.ttc",          // Microsoft YaHei (Chinese)
        "C:/Windows/Fonts/msgothic.ttc",      // MS Gothic (Japanese)
        "C:/Windows/Fonts/msyhbd.ttc",        // Microsoft YaHei Bold
        "C:/Windows/Fonts/simsun.ttc",        // SimSun (Chinese)
    };

    for (const char* font_path : font_paths) {
        // Check if file exists by trying to open it
        FILE* test_file = fopen(font_path, "rb");
        if (test_file) {
            fclose(test_file);
            // Load font with CJK glyph ranges (includes Chinese, Japanese, Korean)
            font = io.Fonts->AddFontFromFileTTF(font_path, 16.0f, nullptr,
                io.Fonts->GetGlyphRangesChineseFull());
            if (font) {
                break;
            }
        }
    }

    // If no system font found, add default font and try to merge CJK glyphs
    if (!font) {
        font = io.Fonts->AddFontDefault();
        if (font) {
            // Merge in Chinese/Japanese glyphs from system fonts
            ImFontConfig config;
            config.MergeMode = true;
            config.GlyphMinAdvanceX = 13.0f; // Use if you want to make the icon monospaced
            config.PixelSnapH = true;

            // Try to merge CJK characters from any available font
            for (const char* font_path : font_paths) {
                FILE* test_file = fopen(font_path, "rb");
                if (test_file) {
                    fclose(test_file);
                    io.Fonts->AddFontFromFileTTF(font_path, 16.0f, &config,
                        io.Fonts->GetGlyphRangesChineseFull());
                    break;
                }
            }
        }
    }
#else
    // On Linux/Mac, try common font paths
    const char* font_paths[] = {
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/PingFang.ttc",  // macOS
        "/System/Library/Fonts/STHeiti Light.ttc",  // macOS Chinese
    };

    for (const char* font_path : font_paths) {
        FILE* test_file = fopen(font_path, "rb");
        if (test_file) {
            fclose(test_file);
            font = io.Fonts->AddFontFromFileTTF(font_path, 16.0f, nullptr,
                io.Fonts->GetGlyphRangesChineseFull());
            if (font) {
                break;
            }
        }
    }

    // Fallback to default font if no custom font found
    if (!font) {
        font = io.Fonts->AddFontDefault();
    }
#endif

    // Use ImGui default dark theme (blue style)
    ImGui::StyleColorsDark();

    state.pass_action.colors[0] = { .load_action=SG_LOADACTION_CLEAR, .clear_value={0.1f, 0.1f, 0.1f, 1.0f } };
    
    // Initialize sokol_audio with callback model
    saudio_desc audio_desc = {};
    audio_desc.sample_rate = state.sample_rate;
    audio_desc.num_channels = 2; // Stereo
    audio_desc.buffer_frames = 2048;  // ~46ms latency
    audio_desc.stream_userdata_cb = audio_stream_callback;
    audio_desc.user_data = nullptr;
    audio_desc.logger.func = slog_func;
    
    saudio_setup(&audio_desc);
    state.audio_initialized = saudio_isvalid();
    
    // Initialize Native File Dialog
    NFD_Init();
    
    // Initialize NES Emulator
    state.nes_emu.init(state.sample_rate);
}

void draw_player_window() {
    ImGui::SetNextWindowSize(ImVec2(500, 450), ImGuiCond_FirstUseEver);
    ImGui::Begin("NES Music Player", nullptr, ImGuiWindowFlags_MenuBar);
    
    // Menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open NSF...", "Ctrl+O")) {
                nfdu8filteritem_t filterItem[2];
                filterItem[0].name = "NES Sound Files";
                filterItem[0].spec = "nsf,nsfe";
                filterItem[1].name = "All Files";
                filterItem[1].spec = "*";
                
                nfdu8char_t* outPath = nullptr;
                nfdresult_t result = NFD_OpenDialogU8(&outPath, filterItem, 2, nullptr);
                
                if (result == NFD_OKAY) {
                    load_nsf_file(outPath);
                    postload_preprocess();
                    NFD_FreePathU8(outPath);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                sapp_request_quit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Emulator")) {
            if (ImGui::MenuItem("Open NES ROM...", "Ctrl+R")) {
                nfdu8filteritem_t filterItem[2];
                filterItem[0].name = "NES ROM Files";
                filterItem[0].spec = "nes";
                filterItem[1].name = "All Files";
                filterItem[1].spec = "*";
                
                nfdu8char_t* outPath = nullptr;
                nfdresult_t result = NFD_OpenDialogU8(&outPath, filterItem, 2, nullptr);
                
                if (result == NFD_OKAY) {
                    load_nes_rom(outPath);
                    NFD_FreePathU8(outPath);
                }
            }
            ImGui::MenuItem("Show Emulator Window", nullptr, &show_emulator);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Audio Visualizer", nullptr, &show_visualizer);
            ImGui::MenuItem("Piano Visualizer", nullptr, &show_piano);
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &show_demo_window);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    
    // File info section
    ImGui::Text("NES APU Audio Player");
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.7f, 1.0f), "Powered by Game_Music_Emu");
    ImGui::Separator();
    
    // File loading section
    ImGui::Text("File:");
    ImGui::SameLine();
    
    // Show filename only, not full path
    const char* filename = state.loaded_file;
    const char* last_slash = strrchr(state.loaded_file, '/');
    const char* last_backslash = strrchr(state.loaded_file, '\\');
    if (last_slash && last_slash > last_backslash) filename = last_slash + 1;
    else if (last_backslash) filename = last_backslash + 1;
    
    if (state.loaded_file[0] != '\0') {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", filename);
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(No file loaded)");
    }
    
    ImGui::SameLine(ImGui::GetWindowWidth() - 100);
    if (ImGui::Button("Open...", ImVec2(90, 0))) {
        nfdu8filteritem_t filterItem[2];
        filterItem[0].name = "NES Sound Files";
        filterItem[0].spec = "nsf,nsfe";
        filterItem[1].name = "All Files";
        filterItem[1].spec = "*";
        
        nfdu8char_t* outPath = nullptr;
        nfdresult_t result = NFD_OpenDialogU8(&outPath, filterItem, 2, nullptr);
        
        if (result == NFD_OKAY) {
            load_nsf_file(outPath);
            postload_preprocess();
            NFD_FreePathU8(outPath);
        }
    }
    
    // Error display
    if (state.error_msg[0] != '\0') {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s", state.error_msg);
    }
    
    ImGui::Separator();
    
    // Track info and controls (only if file loaded)
    if (state.emu) {
        // Track info
        track_info_t info;
        if (gme_track_info(state.emu, &info, state.current_track) == nullptr) {
            ImGui::BeginChild("TrackInfo", ImVec2(0, 80), true);
            
            if (info.game[0]) {
                ImGui::Text("Game: %s", info.game);
            }
            if (info.song[0]) {
                ImGui::Text("Song: %s", info.song);
            } else {
                ImGui::Text("Track: %d / %d", state.current_track + 1, state.track_count);
            }
            if (info.author[0]) {
                ImGui::Text("Author: %s", info.author);
            }
            if (info.copyright[0]) {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "© %s", info.copyright);
            }
            
            ImGui::EndChild();
        }
        
        // Track selection
        ImGui::Text("Track:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderInt("##track", &state.current_track, 0, state.track_count - 1, "Track %d")) {
            start_track_with_preprocess(state.current_track);
        }
        ImGui::SameLine();
        ImGui::Text("/ %d", state.track_count);
        
        ImGui::Separator();
        
        // Playback position and seek bar
        {
            long pos = gme_tell(state.emu);
            long length = 0;
            if (gme_track_info(state.emu, &info, state.current_track) == nullptr) {
                length = info.length > 0 ? info.length : 150000; // Default 2:30 if unknown
            }
            if (length <= 0) length = 150000; // Fallback
            
            // Format time strings
            int pos_sec = (pos / 1000) % 60;
            int pos_min = (pos / 1000) / 60;
            int len_sec = (length / 1000) % 60;
            int len_min = (length / 1000) / 60;
            
            // Time display: current / total
            char time_str[64];
            snprintf(time_str, sizeof(time_str), "%02d:%02d / %02d:%02d", 
                     pos_min, pos_sec, len_min, len_sec);
            
            // Calculate layout
            float time_width = ImGui::CalcTextSize(time_str).x;
            float available_width = ImGui::GetContentRegionAvail().x;
            float slider_width = available_width - time_width - 20;
            
            // Progress slider (interactive seek bar)
            float progress = static_cast<float>(pos) / static_cast<float>(length);
            progress = std::clamp(progress, 0.0f, 1.0f);
            
            ImGui::SetNextItemWidth(slider_width);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.20f, 0.20f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.25f, 0.25f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.50f, 0.70f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.60f, 0.80f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 12.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            
            if (ImGui::SliderFloat("##seek", &progress, 0.0f, 1.0f, "")) {
                // User is seeking - send request to audio thread
                long new_pos = static_cast<long>(progress * length);
                state.seek_request.store(new_pos);
            }
            
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(5);
            
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "%s", time_str);
            
            // Visual progress bar below the slider (filled portion)
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 bar_pos = ImGui::GetCursorScreenPos();
            bar_pos.y -= 22; // Position it over the slider
            float bar_height = 4.0f;
            float filled_width = progress * slider_width;
            
            // Draw filled portion with gradient
            ImU32 color_left = IM_COL32(80, 140, 220, 255);
            ImU32 color_right = IM_COL32(140, 200, 255, 255);
            draw_list->AddRectFilledMultiColor(
                ImVec2(bar_pos.x, bar_pos.y + 8),
                ImVec2(bar_pos.x + filled_width, bar_pos.y + 8 + bar_height),
                color_left, color_right, color_right, color_left
            );
            
            // Check if track ended
            if (state.is_playing.load() && gme_track_ended(state.emu)) {
                // Auto-advance to next track
                if (state.current_track < state.track_count - 1) {
                    state.current_track++;
                    start_track_with_preprocess(state.current_track);
                } else {
                    state.is_playing.store(false);
                }
            }
        }
        
        ImGui::Separator();
        
        // Playback controls
        ImGui::BeginGroup();
        {
            // Previous track
            if (ImGui::Button("|<", ImVec2(40, 30))) {
                if (state.current_track > 0) {
                    state.current_track--;
                    start_track_with_preprocess(state.current_track);
                }
            }
            ImGui::SameLine();
            
            // Play/Pause
            const char* play_label = state.is_playing.load() ? "||" : ">";
            if (ImGui::Button(play_label, ImVec2(50, 30))) {
                if (!state.is_playing.load()) {
                    safe_start_track(state.current_track);  // Same track, no re-preprocessing
                } else {
                    state.is_playing.store(false);
                }
            }
            ImGui::SameLine();
            
            // Stop
            if (ImGui::Button("[]", ImVec2(40, 30))) {
                state.is_playing.store(false);
                // Reset to beginning of track
                state.seek_request.store(0);
            }
            ImGui::SameLine();
            
            // Next track
            if (ImGui::Button(">|", ImVec2(40, 30))) {
                if (state.current_track < state.track_count - 1) {
                    state.current_track++;
                    start_track_with_preprocess(state.current_track);
                }
            }
        }
        ImGui::EndGroup();
        
        ImGui::Separator();
        
        // Audio controls
        ImGui::Text("Audio Settings");
        
        // Volume
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderFloat("Volume", &state.volume_db, -40.0f, 6.0f, "%.1f dB")) {
            // Volume is applied in audio callback
        }
        ImGui::SameLine();
        if (ImGui::Button("0 dB")) {
            state.volume_db = 0.0f;
        }
        
        // Tempo
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderFloat("Tempo", &state.tempo, 0.25f, 2.0f, "%.2fx")) {
            gme_set_tempo(state.emu, state.tempo);
        }
        ImGui::SameLine();
        if (ImGui::Button("1.0x")) {
            state.tempo = 1.0f;
            gme_set_tempo(state.emu, state.tempo);
        }
        
        // Voice info
        ImGui::Separator();
        ImGui::Text("NES APU Channels:");
        
        int voice_count = gme_voice_count(state.emu);
        const char** voice_names = gme_voice_names(state.emu);
        
        ImGui::Columns(voice_count, "voices", false);
        for (int i = 0; i < voice_count && i < 5; ++i) {
            NesChannel channel = static_cast<NesChannel>(i);
            bool muted = state.visualizer.isChannelMuted(channel);
            
            ImGui::PushStyleColor(ImGuiCol_CheckMark, ChannelColors[i]);
            char label[64];
            snprintf(label, sizeof(label), "%s##ch%d", voice_names[i], i);
            if (ImGui::Checkbox(label, &muted)) {
                state.visualizer.setChannelMute(channel, muted);
            }
            ImGui::PopStyleColor();
            
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
    } else {
        // No file loaded
        ImGui::Dummy(ImVec2(0, 20));
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f), "Load an NSF file to start playing NES music!");
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1.0f), "Supported formats: .nsf, .nsfe");
    }
    
    ImGui::Separator();
    
    // Status bar
    if (state.audio_initialized) {
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "Audio: Ready (%ld Hz)", state.sample_rate);
    } else {
        ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1.0f), "Audio: Not initialized");
    }
    
    ImGui::End();
}

void frame(void) {
    const int width = sapp_width();
    const int height = sapp_height();
    simgui_new_frame({ width, height, sapp_frame_duration(), sapp_dpi_scale() });

    // Run NES emulator frame if active
    if (current_mode == AppMode::NES_EMULATOR && state.nes_emu.isRunning()) {
        // Update input from keyboard (only if ImGui doesn't want keyboard)
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            update_nes_input();
        }
        
        // Run one frame of emulation
        state.nes_emu.runFrame();
    }

    // Main player window
    draw_player_window();
    
    // NES Emulator window
    if (show_emulator) {
        draw_emulator_window(&show_emulator);
    }
    
    // Visualizer window
    if (show_visualizer) {
        state.visualizer.drawVisualizerWindow(&show_visualizer);
    }
    
    // Piano visualizer window
    if (show_piano) {
        float current_time = (current_mode == AppMode::NES_EMULATOR) 
            ? static_cast<float>(state.nes_emu.getCpuCycles()) / 1789773.0f
            : state.playback_time.load();
        state.piano.drawPianoWindow(&show_piano, current_time);
    }
    
    // ImGui demo window
    if (show_demo_window) {
        ImGui::ShowDemoWindow(&show_demo_window);
    }

    sg_pass _sg_pass{};
    _sg_pass = { .action = state.pass_action, .swapchain = sglue_swapchain() };

    sg_begin_pass(&_sg_pass);
    simgui_render();
    sg_end_pass();
    sg_commit();
}

void cleanup(void) {
    // Stop audio playback
    state.is_playing.store(false);
    
    // Wait for audio thread to finish
    {
        std::lock_guard<std::mutex> lock(audio_mutex);
        // Cleanup Game_Music_Emu
        if (state.emu) {
            gme_delete(state.emu);
            state.emu = nullptr;
        }
    }
    
    // Cleanup sokol_audio
    if (state.audio_initialized) {
        saudio_shutdown();
    }
    
    // Cleanup Native File Dialog
    NFD_Quit();
    
    simgui_shutdown();
    sg_shutdown();
}

void input(const sapp_event* ev) {
    simgui_handle_event(ev);
    
    // Handle file drag and drop
    if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED) {
        const int num_files = sapp_get_num_dropped_files();
        if (num_files > 0) {
            const char* path = sapp_get_dropped_file_path(0);
            if (path && path[0] != '\0') {
                // Check file extension and load appropriately
                if (has_extension(path, "nsf") || has_extension(path, "nsfe")) {
                    load_nsf_file(path);
                    postload_preprocess();
                } else if (has_extension(path, "nes")) {
                    load_nes_rom(path);
                }
            }
        }
    }
    
    // Track key states for NES controller input
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) {
        if (ev->key_code < 512) key_states[ev->key_code] = true;
    } else if (ev->type == SAPP_EVENTTYPE_KEY_UP) {
        if (ev->key_code < 512) key_states[ev->key_code] = false;
    }
    
    // Keyboard shortcuts
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && !ImGui::GetIO().WantCaptureKeyboard) {
        switch (ev->key_code) {
            case SAPP_KEYCODE_SPACE:
                // Toggle play/pause
                if (state.emu) {
                    if (!state.is_playing.load()) {
                        safe_start_track(state.current_track);  // Same track, no re-preprocessing
                    } else {
                        state.is_playing.store(false);
                    }
                }
                break;
                
            case SAPP_KEYCODE_LEFT:
                // Previous track
                if (state.emu && state.current_track > 0) {
                    state.current_track--;
                    start_track_with_preprocess(state.current_track);
                }
                break;
                
            case SAPP_KEYCODE_RIGHT:
                // Next track
                if (state.emu && state.current_track < state.track_count - 1) {
                    state.current_track++;
                    start_track_with_preprocess(state.current_track);
                }
                break;
                
            default:
                break;
        }
        
        // Ctrl+O: Open NSF file
        if (ev->key_code == SAPP_KEYCODE_O && (ev->modifiers & SAPP_MODIFIER_CTRL)) {
            nfdu8filteritem_t filterItem[2];
            filterItem[0].name = "NES Sound Files";
            filterItem[0].spec = "nsf,nsfe";
            filterItem[1].name = "All Files";
            filterItem[1].spec = "*";
            
            nfdu8char_t* outPath = nullptr;
            nfdresult_t result = NFD_OpenDialogU8(&outPath, filterItem, 2, nullptr);
            
            if (result == NFD_OKAY) {
                load_nsf_file(outPath);
                postload_preprocess();
                NFD_FreePathU8(outPath);
            }
        }
        
        // Ctrl+R: Open NES ROM
        if (ev->key_code == SAPP_KEYCODE_R && (ev->modifiers & SAPP_MODIFIER_CTRL)) {
            nfdu8filteritem_t filterItem[2];
            filterItem[0].name = "NES ROM Files";
            filterItem[0].spec = "nes";
            filterItem[1].name = "All Files";
            filterItem[1].spec = "*";
            
            nfdu8char_t* outPath = nullptr;
            nfdresult_t result = NFD_OpenDialogU8(&outPath, filterItem, 2, nullptr);
            
            if (result == NFD_OKAY) {
                load_nes_rom(outPath);
                NFD_FreePathU8(outPath);
            }
        }
        
        // P: Toggle emulator pause (when in emulator mode)
        if (ev->key_code == SAPP_KEYCODE_P && current_mode == AppMode::NES_EMULATOR) {
            if (state.nes_emu.isRunning()) {
                state.nes_emu.pause();
            } else {
                state.nes_emu.resume();
            }
        }
        
        // F5: Reset emulator
        if (ev->key_code == SAPP_KEYCODE_F5 && current_mode == AppMode::NES_EMULATOR) {
            state.nes_emu.reset();
        }
    }
}

sapp_desc sokol_main(int argc, char* argv[]) {
    sapp_desc _sapp_desc{};
    _sapp_desc.init_cb = init;
    _sapp_desc.frame_cb = frame;
    _sapp_desc.cleanup_cb = cleanup;
    _sapp_desc.event_cb = input;
    _sapp_desc.width = 1280;
    _sapp_desc.height = 720;
    _sapp_desc.window_title = "NES Music Player - NSF Visualizer";
    _sapp_desc.icon.sokol_default = true;
    _sapp_desc.logger.func = slog_func;
    
    // Enable drag and drop support
    _sapp_desc.enable_dragndrop = true;
    _sapp_desc.max_dropped_files = 1;
    _sapp_desc.max_dropped_file_path_length = 4096;
    
    return _sapp_desc;
}
