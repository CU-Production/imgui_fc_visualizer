#define SOKOL_IMPL
#define SOKOL_WGPU
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_glue.h"
#include "sokol_audio.h"

#include "imgui.h"
#define SOKOL_IMGUI_IMPL
#include "util/sokol_imgui.h"

// TinySoundFont for MIDI playback
#define TSF_IMPLEMENTATION
#include "TinySoundFont/tsf.h"

// TinyMidiLoader for MIDI file parsing
#define TML_IMPLEMENTATION
#include "TinySoundFont/tml.h"
