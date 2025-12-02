#define SOKOL_IMPL
#define SOKOL_GLCORE
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_glue.h"
#include <vector>
#include <cstring>

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
} state;

void init(void) {
    sg_desc _sg_desc{};
    _sg_desc.environment = sglue_environment();
    _sg_desc.logger.func = slog_func;
    sg_setup(_sg_desc);

    simgui_desc_t simgui_desc = { };
    simgui_desc.logger.func = slog_func;
    simgui_setup(&simgui_desc);

    state.pass_action.colors[0] = { .load_action=SG_LOADACTION_CLEAR, .clear_value={0.0f, 0.0f, 0.0f, 1.0f } };
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
        
        const long sample_rate = 44100;
        gme_err_t err = gme_open_file(state.loaded_file, &state.emu, sample_rate);
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
        
        if (state.is_playing) {
            long pos = gme_tell(state.emu);
            ImGui::Text("Position: %ld ms", pos);
            
            if (gme_track_ended(state.emu)) {
                state.is_playing = false;
            }
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
    // Cleanup Game_Music_Emu
    if (state.emu) {
        gme_delete(state.emu);
        state.emu = nullptr;
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