//------------------------------------------------------------------------------
// A2A03Visualizer.cpp
// 2A03 chip visualization for NES emulator
// Based on v6502r by floooh (https://github.com/floooh/v6502r)
//------------------------------------------------------------------------------

// sokol_imgui.h needs these to be defined before inclusion
#define SOKOL_GLCORE
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "util/sokol_imgui.h"

#include "A2A03Visualizer.h"
#include "NesEmulator.h"
#include <cstring>
#include <cmath>

// Include 2A03 chip data (C files, need extern "C")
extern "C" {
#include "v6502r/src/2a03/nodenames.h"
#include "v6502r/src/2a03/nodegroups.h"
#include "v6502r/src/2a03/segdefs.h"
}

// Node state values
static const uint8_t NODE_INACTIVE = 100;
static const uint8_t NODE_ACTIVE = 190;
static const uint8_t NODE_HIGHLIGHTED = 255;

// Shader source code (embedded)
// Vertex shader
static const char* vs_source = R"(
#version 330
uniform vec4 color0;
uniform vec2 half_size;
uniform vec2 offset;
uniform vec2 scale;

uniform sampler2D palette_tex;

layout(location=0) in vec2 pos;
layout(location=1) in ivec2 uv;

out vec4 color;

void main() {
    vec2 p = (pos - half_size) + offset;
    p *= scale;
    gl_Position = vec4(p, 0.5, 1.0);
    float r = texelFetch(palette_tex, ivec2(uv.x & 255, uv.x >> 8), 0).r;
    color = vec4(color0.xyz * r, color0.w);
}
)";

// Fragment shader (alpha blend)
static const char* fs_alpha_source = R"(
#version 330
in vec4 color;
out vec4 frag_color;

void main() {
    frag_color = color;
}
)";

// Fragment shader (additive blend)
static const char* fs_add_source = R"(
#version 330
in vec4 color;
out vec4 frag_color;

void main() {
    frag_color = vec4(color.rgb * color.a, 1.0);
}
)";

A2A03Visualizer::A2A03Visualizer() {
    // Initialize all to default values
    memset(layer_buffers_, 0, sizeof(layer_buffers_));
    memset(layer_vertex_counts_, 0, sizeof(layer_vertex_counts_));
    memset(node_states_, NODE_INACTIVE, sizeof(node_states_));
    
    for (int i = 0; i < A2A03_MAX_LAYERS; ++i) {
        layer_visible_[i] = true;
    }
    
    palette_ = A2A03_DEFAULT_PALETTE;
    
    // Set geometry bounds from segdefs
    seg_min_x_ = seg_min_x;
    seg_min_y_ = seg_min_y;
    seg_max_x_ = seg_max_x;
    seg_max_y_ = seg_max_y;
}

A2A03Visualizer::~A2A03Visualizer() {
    if (initialized_) {
        shutdown();
    }
}

bool A2A03Visualizer::init() {
    if (initialized_) return true;
    
    // Create vertex buffers for each layer
    struct LayerData {
        uint16_t* data;
        size_t count;
    };
    
    LayerData layers[A2A03_MAX_LAYERS] = {
        { seg_vertices_0, sizeof(seg_vertices_0) / sizeof(uint16_t) },
        { seg_vertices_1, sizeof(seg_vertices_1) / sizeof(uint16_t) },
        { seg_vertices_2, sizeof(seg_vertices_2) / sizeof(uint16_t) },
        { seg_vertices_3, sizeof(seg_vertices_3) / sizeof(uint16_t) },
        { seg_vertices_4, sizeof(seg_vertices_4) / sizeof(uint16_t) },
        { seg_vertices_5, sizeof(seg_vertices_5) / sizeof(uint16_t) },
    };
    
    for (int i = 0; i < A2A03_MAX_LAYERS; ++i) {
        if (layers[i].data && layers[i].count > 0) {
            sg_buffer_desc desc = {};
            desc.data.ptr = layers[i].data;
            desc.data.size = layers[i].count * sizeof(uint16_t);
            desc.label = "a2a03-layer-vb";
            
            layer_buffers_[i] = sg_make_buffer(&desc);
            layer_vertex_counts_[i] = static_cast<int>(layers[i].count / 4);  // 4 uint16 per vertex (x,y,u,v)
        }
    }
    
    // Create shaders using new Sokol API
    sg_shader_desc shader_desc_alpha = {};
    shader_desc_alpha.vertex_func.source = vs_source;
    shader_desc_alpha.fragment_func.source = fs_alpha_source;
    
    // Vertex attributes
    shader_desc_alpha.attrs[0].glsl_name = "pos";
    shader_desc_alpha.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    shader_desc_alpha.attrs[1].glsl_name = "uv";
    shader_desc_alpha.attrs[1].base_type = SG_SHADERATTRBASETYPE_SINT;
    
    // Uniform block (vertex shader stage)
    shader_desc_alpha.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    shader_desc_alpha.uniform_blocks[0].size = sizeof(float) * 10;  // color0(4) + half_size(2) + offset(2) + scale(2)
    shader_desc_alpha.uniform_blocks[0].glsl_uniforms[0].glsl_name = "color0";
    shader_desc_alpha.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_FLOAT4;
    shader_desc_alpha.uniform_blocks[0].glsl_uniforms[1].glsl_name = "half_size";
    shader_desc_alpha.uniform_blocks[0].glsl_uniforms[1].type = SG_UNIFORMTYPE_FLOAT2;
    shader_desc_alpha.uniform_blocks[0].glsl_uniforms[2].glsl_name = "offset";
    shader_desc_alpha.uniform_blocks[0].glsl_uniforms[2].type = SG_UNIFORMTYPE_FLOAT2;
    shader_desc_alpha.uniform_blocks[0].glsl_uniforms[3].glsl_name = "scale";
    shader_desc_alpha.uniform_blocks[0].glsl_uniforms[3].type = SG_UNIFORMTYPE_FLOAT2;
    
    // Texture view (vertex shader stage, since we sample in VS)
    shader_desc_alpha.views[0].texture.stage = SG_SHADERSTAGE_VERTEX;
    shader_desc_alpha.views[0].texture.image_type = SG_IMAGETYPE_2D;
    shader_desc_alpha.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    
    // Sampler (vertex shader stage)
    shader_desc_alpha.samplers[0].stage = SG_SHADERSTAGE_VERTEX;
    shader_desc_alpha.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    
    // Texture-sampler pair
    shader_desc_alpha.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_VERTEX;
    shader_desc_alpha.texture_sampler_pairs[0].view_slot = 0;
    shader_desc_alpha.texture_sampler_pairs[0].sampler_slot = 0;
    shader_desc_alpha.texture_sampler_pairs[0].glsl_name = "palette_tex";
    
    shader_desc_alpha.label = "a2a03-shader-alpha";
    shader_alpha_ = sg_make_shader(&shader_desc_alpha);
    
    sg_shader_desc shader_desc_add = shader_desc_alpha;
    shader_desc_add.fragment_func.source = fs_add_source;
    shader_desc_add.label = "a2a03-shader-add";
    shader_add_ = sg_make_shader(&shader_desc_add);
    
    // Create pipelines (no depth buffer for 2D visualization)
    sg_pipeline_desc pip_desc = {};
    pip_desc.layout.attrs[0].format = SG_VERTEXFORMAT_USHORT2N;  // pos (normalized)
    pip_desc.layout.attrs[1].format = SG_VERTEXFORMAT_SHORT2;    // uv (node index)
    pip_desc.shader = shader_alpha_;
    pip_desc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    pip_desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
    pip_desc.colors[0].blend.enabled = true;
    pip_desc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pip_desc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pip_desc.depth.pixel_format = SG_PIXELFORMAT_NONE;  // No depth buffer
    pip_desc.label = "a2a03-pipeline-alpha";
    pipeline_alpha_ = sg_make_pipeline(&pip_desc);
    
    pip_desc.shader = shader_add_;
    pip_desc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
    pip_desc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
    pip_desc.label = "a2a03-pipeline-add";
    pipeline_add_ = sg_make_pipeline(&pip_desc);
    
    // Create node state texture (256 x 32 = 8192 nodes)
    sg_image_desc img_desc = {};
    img_desc.width = 256;
    img_desc.height = A2A03_MAX_NODES / 256;
    img_desc.pixel_format = SG_PIXELFORMAT_R8;
    img_desc.usage.stream_update = true;
    img_desc.label = "a2a03-node-texture";
    node_texture_ = sg_make_image(&img_desc);
    
    // Create texture view
    sg_view_desc view_desc = {};
    view_desc.texture.image = node_texture_;
    view_desc.label = "a2a03-node-texture-view";
    node_texture_view_ = sg_make_view(&view_desc);
    
    // Create sampler
    sg_sampler_desc smp_desc = {};
    smp_desc.min_filter = SG_FILTER_NEAREST;
    smp_desc.mag_filter = SG_FILTER_NEAREST;
    smp_desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    smp_desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    smp_desc.label = "a2a03-node-sampler";
    node_sampler_ = sg_make_sampler(&smp_desc);
    
    // Initialize node lookup
    initNodeLookup();
    cacheNodeIndices();
    
    initialized_ = true;
    return true;
}

void A2A03Visualizer::shutdown() {
    if (!initialized_) return;
    
    for (int i = 0; i < A2A03_MAX_LAYERS; ++i) {
        if (layer_buffers_[i].id != SG_INVALID_ID) {
            sg_destroy_buffer(layer_buffers_[i]);
        }
    }
    
    if (pipeline_alpha_.id != SG_INVALID_ID) sg_destroy_pipeline(pipeline_alpha_);
    if (pipeline_add_.id != SG_INVALID_ID) sg_destroy_pipeline(pipeline_add_);
    if (shader_alpha_.id != SG_INVALID_ID) sg_destroy_shader(shader_alpha_);
    if (shader_add_.id != SG_INVALID_ID) sg_destroy_shader(shader_add_);
    if (node_texture_view_.id != SG_INVALID_ID) sg_destroy_view(node_texture_view_);
    if (node_texture_.id != SG_INVALID_ID) sg_destroy_image(node_texture_);
    if (node_sampler_.id != SG_INVALID_ID) sg_destroy_sampler(node_sampler_);
    
    if (render_target_view_.id != SG_INVALID_ID) sg_destroy_view(render_target_view_);
    if (color_attachment_view_.id != SG_INVALID_ID) sg_destroy_view(color_attachment_view_);
    if (render_target_.id != SG_INVALID_ID) sg_destroy_image(render_target_);
    if (render_sampler_.id != SG_INVALID_ID) sg_destroy_sampler(render_sampler_);
    
    initialized_ = false;
}

void A2A03Visualizer::initNodeLookup() {
    // Build name to index map
    for (int i = 0; i < num_node_names; ++i) {
        if (node_names[i] && node_names[i][0] != '\0') {
            node_name_to_index_[node_names[i]] = i;
        }
    }
}

void A2A03Visualizer::cacheNodeIndices() {
    // Cache CPU register node indices using nodegroups
    for (int i = 0; i < 8; ++i) {
        node_a_[i] = nodegroup_a[i];
        node_x_[i] = nodegroup_x[i];
        node_y_[i] = nodegroup_y[i];
        node_sp_[i] = nodegroup_sp[i];
        node_p_[i] = nodegroup_p[i];
        node_pcl_[i] = nodegroup_pcl[i];
        node_pch_[i] = nodegroup_pch[i];
        node_db_[i] = nodegroup_db[i];
    }
    
    for (int i = 0; i < 16; ++i) {
        node_ab_[i] = nodegroup_ab[i];
    }
    
    // Cache APU node indices
    for (int i = 0; i < 4; ++i) {
        node_sq0_out_[i] = nodegroup_sq0[i];
        node_sq1_out_[i] = nodegroup_sq1[i];
        node_tri_out_[i] = nodegroup_tri[i];
        node_noi_out_[i] = nodegroup_noi[i];
    }
    
    for (int i = 0; i < 7; ++i) {
        node_pcm_out_[i] = nodegroup_pcm[i];
    }
}

void A2A03Visualizer::setNodeBits(const int* nodes, int count, uint32_t value) {
    for (int i = 0; i < count; ++i) {
        if (nodes[i] >= 0 && nodes[i] < A2A03_MAX_NODES) {
            node_states_[nodes[i]] = (value & (1 << i)) ? NODE_ACTIVE : NODE_INACTIVE;
        }
    }
}

void A2A03Visualizer::updateFromEmulator(const NesEmulator* emu) {
    if (!emu || !initialized_) return;
    
    // Reset all nodes to inactive first
    memset(node_states_, NODE_INACTIVE, sizeof(node_states_));
    
    // Get CPU state from emulator
    NesEmulator::CpuState emu_cpu = emu->getCpuState();
    A2A03CpuState cpu_state = {};
    cpu_state.a = emu_cpu.a;
    cpu_state.x = emu_cpu.x;
    cpu_state.y = emu_cpu.y;
    cpu_state.sp = emu_cpu.sp;
    cpu_state.p = emu_cpu.p;
    cpu_state.pc = emu_cpu.pc;
    cpu_state.addr = emu_cpu.pc;  // Use PC as current address bus value
    cpu_state.data = 0;           // Data bus not easily accessible
    cpu_state.rw = true;          // Assume read
    
    // Update CPU register nodes
    setNodeBits(node_a_, 8, cpu_state.a);
    setNodeBits(node_x_, 8, cpu_state.x);
    setNodeBits(node_y_, 8, cpu_state.y);
    setNodeBits(node_sp_, 8, cpu_state.sp);
    setNodeBits(node_p_, 8, cpu_state.p);
    setNodeBits(node_pcl_, 8, cpu_state.pc & 0xFF);
    setNodeBits(node_pch_, 8, (cpu_state.pc >> 8) & 0xFF);
    setNodeBits(node_ab_, 16, cpu_state.addr);
    
    // Get APU state from emulator
    NesEmulator::ApuState emu_apu = emu->getApuState();
    
    // Update APU output nodes
    setNodeBits(node_sq0_out_, 4, emu_apu.sq0_out);
    setNodeBits(node_sq1_out_, 4, emu_apu.sq1_out);
    setNodeBits(node_tri_out_, 4, emu_apu.tri_out);
    setNodeBits(node_noi_out_, 4, emu_apu.noi_out);
    setNodeBits(node_pcm_out_, 7, emu_apu.pcm_out);
}

void A2A03Visualizer::updateCpuState(const A2A03CpuState& cpu) {
    if (!initialized_) return;
    
    // Update CPU register nodes
    setNodeBits(node_a_, 8, cpu.a);
    setNodeBits(node_x_, 8, cpu.x);
    setNodeBits(node_y_, 8, cpu.y);
    setNodeBits(node_sp_, 8, cpu.sp);
    setNodeBits(node_p_, 8, cpu.p);
    setNodeBits(node_pcl_, 8, cpu.pc & 0xFF);
    setNodeBits(node_pch_, 8, (cpu.pc >> 8) & 0xFF);
    setNodeBits(node_db_, 8, cpu.data);
    setNodeBits(node_ab_, 16, cpu.addr);
}

void A2A03Visualizer::updateApuState(const A2A03ApuState& apu) {
    if (!initialized_) return;
    
    // Update APU output nodes
    setNodeBits(node_sq0_out_, 4, apu.sq0_out);
    setNodeBits(node_sq1_out_, 4, apu.sq1_out);
    setNodeBits(node_tri_out_, 4, apu.tri_out);
    setNodeBits(node_noi_out_, 4, apu.noi_out);
    setNodeBits(node_pcm_out_, 7, apu.pcm_out);
}

void A2A03Visualizer::updateRenderTarget(int width, int height) {
    if (width == render_width_ && height == render_height_) return;
    
    // Destroy old resources
    if (render_target_view_.id != SG_INVALID_ID) sg_destroy_view(render_target_view_);
    if (color_attachment_view_.id != SG_INVALID_ID) sg_destroy_view(color_attachment_view_);
    if (render_target_.id != SG_INVALID_ID) sg_destroy_image(render_target_);
    if (render_sampler_.id != SG_INVALID_ID) sg_destroy_sampler(render_sampler_);
    
    render_width_ = width;
    render_height_ = height;
    
    // Create render target image
    sg_image_desc rt_desc = {};
    rt_desc.usage.color_attachment = true;
    rt_desc.width = width;
    rt_desc.height = height;
    rt_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    rt_desc.label = "a2a03-render-target";
    render_target_ = sg_make_image(&rt_desc);
    
    // Create texture view for ImGui display
    sg_view_desc rt_view_desc = {};
    rt_view_desc.texture.image = render_target_;
    rt_view_desc.label = "a2a03-render-target-view";
    render_target_view_ = sg_make_view(&rt_view_desc);
    
    // Create color attachment view for render pass
    sg_view_desc color_att_view_desc = {};
    color_att_view_desc.color_attachment.image = render_target_;
    color_att_view_desc.label = "a2a03-color-attachment-view";
    color_attachment_view_ = sg_make_view(&color_att_view_desc);
    
    // Create sampler for ImGui
    sg_sampler_desc smp_desc = {};
    smp_desc.min_filter = SG_FILTER_LINEAR;
    smp_desc.mag_filter = SG_FILTER_LINEAR;
    smp_desc.label = "a2a03-render-sampler";
    render_sampler_ = sg_make_sampler(&smp_desc);
}

void A2A03Visualizer::renderChip() {
    if (!initialized_) return;
    
    // Update node state texture
    sg_image_data img_data = {};
    img_data.mip_levels[0].ptr = node_states_;
    img_data.mip_levels[0].size = sizeof(node_states_);
    sg_update_image(node_texture_, &img_data);
    
    // Apply pipeline
    if (use_additive_blend_) {
        sg_apply_pipeline(pipeline_add_);
    } else {
        sg_apply_pipeline(pipeline_alpha_);
    }
    
    // Uniform data
    struct {
        float color0[4];
        float half_size[2];
        float offset[2];
        float scale[2];
    } vs_params;
    
    vs_params.half_size[0] = (seg_max_x_ >> 1) / 65535.0f;
    vs_params.half_size[1] = (seg_max_y_ >> 1) / 65535.0f;
    vs_params.offset[0] = offset_x_;
    vs_params.offset[1] = offset_y_;
    vs_params.scale[0] = scale_ * aspect_;
    vs_params.scale[1] = scale_;
    
    // Draw each layer
    for (int i = 0; i < A2A03_MAX_LAYERS; ++i) {
        if (!layer_visible_[i] || layer_buffers_[i].id == SG_INVALID_ID) continue;
        
        // Set layer color
        vs_params.color0[0] = palette_.colors[i][0];
        vs_params.color0[1] = palette_.colors[i][1];
        vs_params.color0[2] = palette_.colors[i][2];
        vs_params.color0[3] = palette_.colors[i][3];
        
        // Bind resources
        sg_bindings bindings = {};
        bindings.vertex_buffers[0] = layer_buffers_[i];
        bindings.views[0] = node_texture_view_;
        bindings.samplers[0] = node_sampler_;
        sg_apply_bindings(&bindings);
        
        // Apply uniforms
        sg_range ub_range = { &vs_params, sizeof(vs_params) };
        sg_apply_uniforms(0, &ub_range);
        
        // Draw
        sg_draw(0, layer_vertex_counts_[i], 1);
    }
}

void A2A03Visualizer::render(float width, float height) {
    if (!initialized_) return;
    
    int w = static_cast<int>(width);
    int h = static_cast<int>(height);
    if (w <= 0 || h <= 0) return;
    
    // Update aspect ratio
    aspect_ = height / width;
    
    // Ensure render target is correct size
    updateRenderTarget(w, h);
    
    // Begin render pass to texture (no depth buffer needed for 2D visualization)
    sg_pass pass = {};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value.r = palette_.background[0];
    pass.action.colors[0].clear_value.g = palette_.background[1];
    pass.action.colors[0].clear_value.b = palette_.background[2];
    pass.action.colors[0].clear_value.a = palette_.background[3];
    pass.attachments.colors[0] = color_attachment_view_;
    sg_begin_pass(&pass);
    
    // Render chip
    renderChip();
    
    sg_end_pass();
}

void A2A03Visualizer::drawWindow(bool* p_open) {
    if (!initialized_) return;
    
    ImGui::SetNextWindowSize(ImVec2(600, 600), ImGuiCond_FirstUseEver);
    
    if (!ImGui::Begin("2A03 Chip Visualizer", p_open, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }
    
    // Menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset View")) {
                offset_x_ = 0.0f;
                offset_y_ = 0.0f;
                scale_ = 9.0f;
            }
            ImGui::Separator();
            for (int i = 0; i < A2A03_MAX_LAYERS; ++i) {
                char label[32];
                snprintf(label, sizeof(label), "Layer %d", i);
                if (ImGui::MenuItem(label, nullptr, layer_visible_[i])) {
                    layer_visible_[i] = !layer_visible_[i];
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Style")) {
            if (ImGui::MenuItem("Alpha Blend", nullptr, !use_additive_blend_)) {
                use_additive_blend_ = false;
            }
            if (ImGui::MenuItem("Additive Blend", nullptr, use_additive_blend_)) {
                use_additive_blend_ = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    
    // Controls
    ImGui::SliderFloat("Zoom", &scale_, getMinScale(), getMaxScale(), "%.1f");
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        offset_x_ = 0.0f;
        offset_y_ = 0.0f;
        scale_ = 9.0f;
    }
    
    // Get available space for chip view
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 100) avail.x = 100;
    if (avail.y < 100) avail.y = 100;
    
    // Render chip to texture
    render(avail.x, avail.y);
    
    // Display texture in ImGui
    if (render_target_view_.id != SG_INVALID_ID) {
        ImVec2 uv0(0, 0);
        ImVec2 uv1(1, 1);
        
        // Get cursor position for mouse interaction
        ImVec2 pos = ImGui::GetCursorScreenPos();
        
        // Create ImTextureID from view and sampler
        ImTextureID tex_id = simgui_imtextureid_with_sampler(render_target_view_, render_sampler_);
        
        // Draw the chip image
        ImGui::Image(tex_id, avail, uv0, uv1);
        
        // Handle mouse interaction
        if (ImGui::IsItemHovered()) {
            // Zoom with mouse wheel
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                addScale(wheel * scale_ * 0.1f);
            }
            
            // Pan with middle mouse button or left mouse + drag
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || 
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                float pan_scale = 1.0f / (scale_ * 500.0f);
                offset_x_ += delta.x * pan_scale;
                offset_y_ -= delta.y * pan_scale;
            }
        }
    }
    
    ImGui::End();
}

void A2A03Visualizer::setOffset(float x, float y) {
    offset_x_ = x;
    offset_y_ = y;
}

void A2A03Visualizer::addOffset(float dx, float dy) {
    offset_x_ += dx;
    offset_y_ += dy;
}

void A2A03Visualizer::setScale(float scale) {
    scale_ = scale;
    if (scale_ < getMinScale()) scale_ = getMinScale();
    if (scale_ > getMaxScale()) scale_ = getMaxScale();
}

void A2A03Visualizer::addScale(float delta) {
    setScale(scale_ + delta);
}

void A2A03Visualizer::setLayerVisible(int layer, bool visible) {
    if (layer >= 0 && layer < A2A03_MAX_LAYERS) {
        layer_visible_[layer] = visible;
    }
}

bool A2A03Visualizer::getLayerVisible(int layer) const {
    if (layer >= 0 && layer < A2A03_MAX_LAYERS) {
        return layer_visible_[layer];
    }
    return false;
}

void A2A03Visualizer::toggleLayerVisible(int layer) {
    if (layer >= 0 && layer < A2A03_MAX_LAYERS) {
        layer_visible_[layer] = !layer_visible_[layer];
    }
}

void A2A03Visualizer::highlightNode(int node_index) {
    if (node_index >= 0 && node_index < A2A03_MAX_NODES) {
        node_states_[node_index] = NODE_HIGHLIGHTED;
    }
}

void A2A03Visualizer::clearHighlight() {
    // Reset highlighted nodes back to their logical state
    // For now, just set all to inactive
    memset(node_states_, NODE_INACTIVE, sizeof(node_states_));
}

void A2A03Visualizer::setPalette(const A2A03Palette& palette) {
    palette_ = palette;
}

const char* A2A03Visualizer::getNodeName(int node_index) const {
    if (node_index >= 0 && node_index < num_node_names) {
        return node_names[node_index];
    }
    return nullptr;
}

int A2A03Visualizer::findNodeByName(const char* name) const {
    auto it = node_name_to_index_.find(name);
    if (it != node_name_to_index_.end()) {
        return it->second;
    }
    return -1;
}
