#define SOKOL_IMPL
#define SOKOL_GLCORE
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_glue.h"
#include "sokol_audio.h"
#include <vector>
#include <cstring>
#include <algorithm>

#define SOKOL_IMGUI_IMPL
#include "imgui.h"
#include "util/sokol_imgui.h"

// Game_Music_Emu for NSF file support
#include "gme/gme.h"

// Native File Dialog for file selection
#include "nfd.h"

// Audio Visualizer
#include "AudioVisualizer.h"

static bool show_demo_window = false;
static bool show_visualizer = true;

// application state
static struct {
    sg_pass_action pass_action;
    
    // Game_Music_Emu state
    Music_Emu* emu = nullptr;
    bool is_playing = false;
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
    
    // Audio visualizer
    AudioVisualizer visualizer;
    
    // Audio buffer for visualization (double buffered)
    std::vector<short> viz_buffer;
    int viz_buffer_pos = 0;
} state;

// Audio stream callback - called from audio thread
void audio_stream_callback(float* buffer, int num_frames, int num_channels, void* user_data) {
    if (!state.emu || !state.is_playing) {
        // Fill with silence
        std::fill(buffer, buffer + num_frames * num_channels, 0.0f);
        return;
    }
    
    // Game_Music_Emu generates 16-bit signed samples (stereo)
    const int num_samples = num_frames * num_channels;
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
    
    // Convert 16-bit signed integer to 32-bit float (-1.0 to 1.0)
    // Apply volume control
    float volume_linear = std::pow(10.0f, state.volume_db / 20.0f);
    for (int i = 0; i < num_samples; i++) {
        buffer[i] = (temp_buffer[i] / 32768.0f) * volume_linear;
    }
}

void load_nsf_file(const char* path) {
    // Clean up previous emulator
    if (state.emu) {
        gme_delete(state.emu);
        state.emu = nullptr;
    }
    state.is_playing = false;
    
    // Load new file
    gme_err_t err = gme_open_file(path, &state.emu, state.sample_rate);
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
    
    // Apply current settings
    gme_set_tempo(state.emu, state.tempo);
    gme_mute_voices(state.emu, state.visualizer.getMuteMask());
}

void init(void) {
    sg_desc _sg_desc{};
    _sg_desc.environment = sglue_environment();
    _sg_desc.logger.func = slog_func;
    sg_setup(_sg_desc);

    simgui_desc_t simgui_desc = { };
    simgui_desc.logger.func = slog_func;
    simgui_setup(&simgui_desc);

    // Dark theme with NES-inspired colors
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.ScrollbarRounding = 2.0f;
    
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.95f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.10f, 0.18f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.15f, 0.30f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.22f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.25f, 0.35f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.30f, 0.25f, 0.45f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.35f, 0.60f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.50f, 0.40f, 0.70f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.70f, 0.50f, 0.90f, 1.0f);

    state.pass_action.colors[0] = { .load_action=SG_LOADACTION_CLEAR, .clear_value={0.05f, 0.05f, 0.08f, 1.0f } };
    
    // Initialize sokol_audio with callback model
    saudio_desc audio_desc = {};
    audio_desc.sample_rate = state.sample_rate;
    audio_desc.num_channels = 2; // Stereo
    audio_desc.buffer_frames = 2048;
    audio_desc.stream_userdata_cb = audio_stream_callback;
    audio_desc.user_data = nullptr;
    audio_desc.logger.func = slog_func;
    
    saudio_setup(&audio_desc);
    state.audio_initialized = saudio_isvalid();
    
    // Initialize Native File Dialog
    NFD_Init();
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
                    NFD_FreePathU8(outPath);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                sapp_request_quit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Audio Visualizer", nullptr, &show_visualizer);
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
            gme_start_track(state.emu, state.current_track);
            state.is_playing = true;
        }
        ImGui::SameLine();
        ImGui::Text("/ %d", state.track_count);
        
        // Playback position
        if (state.is_playing) {
            long pos = gme_tell(state.emu);
            long length = 0;
            if (gme_track_info(state.emu, &info, state.current_track) == nullptr) {
                length = info.length > 0 ? info.length : 0;
            }
            
            int pos_sec = pos / 1000;
            int pos_min = pos_sec / 60;
            pos_sec %= 60;
            
            if (length > 0) {
                int len_sec = length / 1000;
                int len_min = len_sec / 60;
                len_sec %= 60;
                ImGui::Text("Time: %02d:%02d / %02d:%02d", pos_min, pos_sec, len_min, len_sec);
                
                float progress = static_cast<float>(pos) / static_cast<float>(length);
                ImGui::ProgressBar(progress, ImVec2(-1, 6), "");
            } else {
                ImGui::Text("Time: %02d:%02d", pos_min, pos_sec);
            }
            
            // Check if track ended
            if (gme_track_ended(state.emu)) {
                // Auto-advance to next track
                if (state.current_track < state.track_count - 1) {
                    state.current_track++;
                    gme_start_track(state.emu, state.current_track);
                } else {
                    state.is_playing = false;
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
                    gme_start_track(state.emu, state.current_track);
                    state.is_playing = true;
                }
            }
            ImGui::SameLine();
            
            // Play/Pause
            const char* play_label = state.is_playing ? "||" : ">";
            if (ImGui::Button(play_label, ImVec2(50, 30))) {
                if (!state.is_playing) {
                    gme_start_track(state.emu, state.current_track);
                    state.is_playing = true;
                } else {
                    state.is_playing = false;
                }
            }
            ImGui::SameLine();
            
            // Stop
            if (ImGui::Button("[]", ImVec2(40, 30))) {
                state.is_playing = false;
                gme_start_track(state.emu, state.current_track);
            }
            ImGui::SameLine();
            
            // Next track
            if (ImGui::Button(">|", ImVec2(40, 30))) {
                if (state.current_track < state.track_count - 1) {
                    state.current_track++;
                    gme_start_track(state.emu, state.current_track);
                    state.is_playing = true;
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

    // Main player window
    draw_player_window();
    
    // Visualizer window
    if (show_visualizer) {
        state.visualizer.drawVisualizerWindow(&show_visualizer);
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
    state.is_playing = false;
    
    // Cleanup Game_Music_Emu
    if (state.emu) {
        gme_delete(state.emu);
        state.emu = nullptr;
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
    
    // Keyboard shortcuts
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && !ImGui::GetIO().WantCaptureKeyboard) {
        switch (ev->key_code) {
            case SAPP_KEYCODE_SPACE:
                // Toggle play/pause
                if (state.emu) {
                    if (!state.is_playing) {
                        gme_start_track(state.emu, state.current_track);
                        state.is_playing = true;
                    } else {
                        state.is_playing = false;
                    }
                }
                break;
                
            case SAPP_KEYCODE_LEFT:
                // Previous track
                if (state.emu && state.current_track > 0) {
                    state.current_track--;
                    gme_start_track(state.emu, state.current_track);
                    state.is_playing = true;
                }
                break;
                
            case SAPP_KEYCODE_RIGHT:
                // Next track
                if (state.emu && state.current_track < state.track_count - 1) {
                    state.current_track++;
                    gme_start_track(state.emu, state.current_track);
                    state.is_playing = true;
                }
                break;
                
            default:
                break;
        }
        
        // Ctrl+O: Open file
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
                NFD_FreePathU8(outPath);
            }
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
    return _sapp_desc;
}
