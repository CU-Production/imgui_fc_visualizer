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

static bool show_test_window = true;
static bool show_another_window = false;

// application state
static struct {
    sg_pass_action pass_action;
    
    // Game_Music_Emu state
    Music_Emu* emu = nullptr;
    bool is_playing = false;
    int current_track = 0;
    int track_count = 0;
    char loaded_file[256] = "3rd_party/Game_Music_Emu/test.nsf";
    char error_msg[512] = "";
    
    // Audio state
    bool audio_initialized = false;
    const long sample_rate = 44100;
} state;

// Audio stream callback - called from audio thread
void audio_stream_callback(float* buffer, int num_frames, int num_channels, void* user_data) {
    if (!state.emu || !state.is_playing) {
        // Fill with silence
        std::fill(buffer, buffer + num_frames * num_channels, 0.0f);
        return;
    }
    
    // Game_Music_Emu generates 16-bit signed samples (stereo)
    // We need to convert to 32-bit float samples
    const int num_samples = num_frames * num_channels;
    static std::vector<short> temp_buffer;
    temp_buffer.resize(num_samples);
    
    // Generate samples from Game_Music_Emu
    gme_err_t err = gme_play(state.emu, num_samples, temp_buffer.data());
    if (err) {
        // On error, fill with silence
        std::fill(buffer, buffer + num_samples, 0.0f);
        return;
    }
    
    // Convert 16-bit signed integer to 32-bit float (-1.0 to 1.0)
    for (int i = 0; i < num_samples; i++) {
        buffer[i] = temp_buffer[i] / 32768.0f;
    }
}

void init(void) {
    sg_desc _sg_desc{};
    _sg_desc.environment = sglue_environment();
    _sg_desc.logger.func = slog_func;
    sg_setup(_sg_desc);

    simgui_desc_t simgui_desc = { };
    simgui_desc.logger.func = slog_func;
    simgui_setup(&simgui_desc);

    state.pass_action.colors[0] = { .load_action=SG_LOADACTION_CLEAR, .clear_value={0.0f, 0.0f, 0.0f, 1.0f } };
    
    // Initialize sokol_audio with callback model
    saudio_desc audio_desc = {};
    audio_desc.sample_rate = state.sample_rate;
    audio_desc.num_channels = 2; // Stereo
    audio_desc.buffer_frames = 2048; // Low latency buffer
    audio_desc.stream_userdata_cb = audio_stream_callback;
    audio_desc.user_data = nullptr;
    audio_desc.logger.func = slog_func;
    
    saudio_setup(&audio_desc);
    state.audio_initialized = saudio_isvalid();
}

void frame(void) {
    const int width = sapp_width();
    const int height = sapp_height();
    simgui_new_frame({ width, height, sapp_frame_duration(), sapp_dpi_scale() });

    // NSF Player Window
    ImGui::Begin("NES Music DAW - Game_Music_Emu Test");
    
    ImGui::Text("Game_Music_Emu Integration Test");
    ImGui::Separator();
    
    // File loading
    ImGui::InputText("NSF File Path", state.loaded_file, sizeof(state.loaded_file));
    ImGui::SameLine();
    if (ImGui::Button("Load NSF")) {
        if (state.emu) {
            gme_delete(state.emu);
            state.emu = nullptr;
        }
        
        gme_err_t err = gme_open_file(state.loaded_file, &state.emu, state.sample_rate);
        if (err) {
            strncpy(state.error_msg, err, sizeof(state.error_msg) - 1);
            state.error_msg[sizeof(state.error_msg) - 1] = '\0';
        } else {
            state.track_count = gme_track_count(state.emu);
            state.current_track = 0;
            state.error_msg[0] = '\0';
            ImGui::Text("Loaded successfully! Tracks: %d", state.track_count);
        }
    }
    
    if (state.error_msg[0] != '\0') {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: %s", state.error_msg);
    }
    
    if (state.emu) {
        ImGui::Separator();
        ImGui::Text("Tracks: %d", state.track_count);
        
        if (ImGui::SliderInt("Track", &state.current_track, 0, state.track_count - 1)) {
            gme_start_track(state.emu, state.current_track);
            state.is_playing = true;
        }
        
        // Track info
        track_info_t info;
        if (gme_track_info(state.emu, &info, state.current_track) == nullptr) {
            ImGui::Text("Game: %s", info.game);
            ImGui::Text("Song: %s", info.song);
            ImGui::Text("Author: %s", info.author);
            if (info.length > 0) {
                ImGui::Text("Length: %ld ms", info.length);
            }
        }
        
        ImGui::Separator();
        
        // Playback controls
        if (ImGui::Button(state.is_playing ? "Stop" : "Play")) {
            if (!state.is_playing) {
                gme_err_t err = gme_start_track(state.emu, state.current_track);
                if (err) {
                    strncpy(state.error_msg, err, sizeof(state.error_msg) - 1);
                } else {
                    state.is_playing = true;
                }
            } else {
                state.is_playing = false;
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Pause")) {
            state.is_playing = false;
        }
        
        if (state.is_playing) {
            long pos = gme_tell(state.emu);
            long length = 0;
            track_info_t info;
            if (gme_track_info(state.emu, &info, state.current_track) == nullptr) {
                length = info.length > 0 ? info.length : 0;
            }
            
            ImGui::Text("Position: %ld ms", pos);
            if (length > 0) {
                float progress = (float)pos / (float)length;
                ImGui::ProgressBar(progress, ImVec2(0.0f, 0.0f));
            }
            
            if (gme_track_ended(state.emu)) {
                state.is_playing = false;
            }
        }
        
        // Audio status
        if (state.audio_initialized) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Audio: Ready");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Audio: Not initialized");
        }
    }
    
    ImGui::End();
    
    // 1. Show a simple window
    // Tip: if we don't call ImGui::Begin()/ImGui::End() the widgets appears in a window automatically called "Debug"
    static float f = 0.0f;
    ImGui::Text("Hello, world!");
    ImGui::SliderFloat("float", &f, 0.0f, 1.0f);
    ImGui::ColorEdit3("clear color", &state.pass_action.colors[0].clear_value.r);
    if (ImGui::Button("Test Window")) show_test_window ^= 1;
    if (ImGui::Button("Another Window")) show_another_window ^= 1;
    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
    ImGui::Text("w: %d, h: %d, dpi_scale: %.1f", sapp_width(), sapp_height(), sapp_dpi_scale());
    if (ImGui::Button(sapp_is_fullscreen() ? "Switch to windowed" : "Switch to fullscreen")) {
        sapp_toggle_fullscreen();
    }

    // 2. Show another simple window, this time using an explicit Begin/End pair
    if (show_another_window) {
        ImGui::SetNextWindowSize(ImVec2(200,100), ImGuiCond_FirstUseEver);
        ImGui::Begin("Another Window", &show_another_window);
        ImGui::Text("Hello");
        ImGui::End();
    }

    // 3. Show the ImGui test window. Most of the sample code is in ImGui::ShowDemoWindow()
    if (show_test_window) {
        ImGui::SetNextWindowPos(ImVec2(460, 20), ImGuiCond_FirstUseEver);
        ImGui::ShowDemoWindow();
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
    
    simgui_shutdown();
    sg_shutdown();
}

void input(const sapp_event* ev) {
    simgui_handle_event(ev);
}

sapp_desc sokol_main(int argc, char* argv[]) {
    sapp_desc _sapp_desc{};
    _sapp_desc.init_cb = init;
    _sapp_desc.frame_cb = frame;
    _sapp_desc.cleanup_cb = cleanup;
    _sapp_desc.event_cb = input;
    _sapp_desc.width = 1280;
    _sapp_desc.height = 720;
    _sapp_desc.window_title = "NES Music DAW";
    _sapp_desc.icon.sokol_default = true;
    _sapp_desc.logger.func = slog_func;
    return _sapp_desc;
}