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
// Perfect2a03 transistor-level simulator
#include "perfect2a03.h"
}

// Include sokol-shdc generated shader header
#include "A2A03Visualizer.glsl.h"

// Node state values
static const uint8_t NODE_INACTIVE = 100;
static const uint8_t NODE_ACTIVE = 190;
static const uint8_t NODE_HIGHLIGHTED = 255;

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
    
    // Create shaders using sokol-shdc generated descriptors
    const sg_shader_desc* shader_desc_alpha = shd_alpha_shader_desc(sg_query_backend());
    if (!shader_desc_alpha) {
        return false;  // Unsupported backend
    }
    shader_alpha_ = sg_make_shader(shader_desc_alpha);
    
    const sg_shader_desc* shader_desc_add = shd_add_shader_desc(sg_query_backend());
    if (!shader_desc_add) {
        return false;  // Unsupported backend
    }
    shader_add_ = sg_make_shader(shader_desc_add);
    
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
    
    // Initialize perfect2a03 transistor simulation
    initSimulation();
    
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
    
    // Shutdown perfect2a03 simulation
    shutdownSimulation();
    
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

void A2A03Visualizer::setPerfect2a03NodeBits(const int* nodes, int count, uint32_t value) {
    if (!sim_state_) return;
    
    for (int i = 0; i < count; ++i) {
        if (nodes[i] >= 0) {
            bool high = (value & (1 << i)) != 0;
            cpu_write_node(sim_state_, nodes[i], high);
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
    
    // Update CPU register nodes (for visualization)
    setNodeBits(node_a_, 8, cpu_state.a);
    setNodeBits(node_x_, 8, cpu_state.x);
    setNodeBits(node_y_, 8, cpu_state.y);
    setNodeBits(node_sp_, 8, cpu_state.sp);
    setNodeBits(node_p_, 8, cpu_state.p);
    setNodeBits(node_pcl_, 8, cpu_state.pc & 0xFF);
    setNodeBits(node_pch_, 8, (cpu_state.pc >> 8) & 0xFF);
    setNodeBits(node_ab_, 16, cpu_state.addr);
    
    // If perfect2a03 simulation is enabled, sync registers and execute instructions
    // Only do this if the emulator is running and has a ROM loaded
    if (sim_state_ && sim_enabled_ && emu->isRunning() && emu->isLoaded()) {
        // 1. Update perfect2a03 registers from emulator state
        setPerfect2a03NodeBits(node_a_, 8, cpu_state.a);
        setPerfect2a03NodeBits(node_x_, 8, cpu_state.x);
        setPerfect2a03NodeBits(node_y_, 8, cpu_state.y);
        setPerfect2a03NodeBits(node_sp_, 8, cpu_state.sp);
        
        // P register: note that p5 is skipped (node_p_[5] should be -1 or 0)
        // From perfect2a03.c: p0,p1,p2,p3,p4,0,p6,p7
        for (int i = 0; i < 8; ++i) {
            if (i == 5) continue;  // Skip p5 (it's always 0 in perfect2a03)
            if (node_p_[i] >= 0) {
                bool high = (cpu_state.p & (1 << i)) != 0;
                cpu_write_node(sim_state_, node_p_[i], high);
            }
        }
        
        setPerfect2a03NodeBits(node_pcl_, 8, cpu_state.pc & 0xFF);
        setPerfect2a03NodeBits(node_pch_, 8, (cpu_state.pc >> 8) & 0xFF);
        setPerfect2a03NodeBits(node_ab_, 16, cpu_state.addr);
        
        // 2. Get instruction bytes from agnes (fetch up to 3 instructions, max 9 bytes)
        extern uint8_t cpu_memory[65536];  // perfect2a03's global memory array
        
        uint16_t pc = cpu_state.pc;
        const int max_bytes = 9;  // 3 instructions * 3 bytes max
        
        // Fetch instruction bytes from agnes and place them in cpu_memory
        for (int i = 0; i < max_bytes; ++i) {
            uint16_t addr = pc + i;
            if (addr <= 0xFFFF) {
                cpu_memory[addr] = emu->readRomByte(addr);
            } else {
                cpu_memory[addr] = 0;
            }
        }
        
        // 3. Execute a few half-cycles to let perfect2a03 process the instructions
        // Execute enough cycles for at least one instruction (typically 2-7 cycles = 4-14 half-cycles)
        // Let's execute about 20 half-cycles to cover a few instructions
        const int half_cycles_to_execute = 20;
        for (int i = 0; i < half_cycles_to_execute; ++i) {
            cpu_step(sim_state_);
        }
        
        // 4. Update visualization from perfect2a03 simulation state
        updateNodeStatesFromSimulation();
    }
    
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
    
    // Uniform data (using sokol-shdc generated struct)
    block_vs_params_t vs_params = {};
    
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
        bindings.views[VIEW_palette_tex] = node_texture_view_;
        bindings.samplers[SMP_palette_tex_smp] = node_sampler_;
        sg_apply_bindings(&bindings);
        
        // Apply uniforms (using sokol-shdc generated uniform block slot)
        sg_range ub_range = { &vs_params, sizeof(vs_params) };
        sg_apply_uniforms(UB_block_vs_params, &ub_range);
        
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
        if (ImGui::BeginMenu("Simulation")) {
            if (ImGui::MenuItem("Enable Transistor Sim", nullptr, sim_enabled_)) {
                sim_enabled_ = !sim_enabled_;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Simulation")) {
                resetSimulation();
            }
            ImGui::Separator();
            ImGui::SliderInt("Cycles/Frame", &sim_cycles_per_frame_, 10, 1000);
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
    // Note: OpenGL texture origin is bottom-left, but ImGui expects top-left
    // So we flip the V coordinates to correct the vertical orientation
    if (render_target_view_.id != SG_INVALID_ID) {
        ImVec2 uv0(0, 1);  // Flip: top-left of image is at V=1
        ImVec2 uv1(1, 0);  // Flip: bottom-right of image is at V=0
        
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

//------------------------------------------------------------------------------
// Perfect2a03 Transistor-Level Simulation
//------------------------------------------------------------------------------

void A2A03Visualizer::initSimulation() {
    if (sim_state_) return;
    
    // Initialize perfect2a03 chip simulation
    sim_state_ = cpu_initAndResetChip();
    if (sim_state_) {
        // Run a few cycles to stabilize the chip after reset
        for (int i = 0; i < 20; ++i) {
            cpu_step(sim_state_);
        }
    }
}

void A2A03Visualizer::shutdownSimulation() {
    if (sim_state_) {
        cpu_destroyChip(sim_state_);
        sim_state_ = nullptr;
    }
}

void A2A03Visualizer::resetSimulation() {
    shutdownSimulation();
    initSimulation();
}

void A2A03Visualizer::stepSimulation(int num_half_cycles) {
    if (!sim_state_ || !sim_enabled_) return;
    
    // Use configured value if -1 passed
    int cycles = (num_half_cycles < 0) ? sim_cycles_per_frame_ : num_half_cycles;
    
    // Execute the specified number of half-cycles
    for (int i = 0; i < cycles; ++i) {
        cpu_step(sim_state_);
    }
    
    // Update visualization from simulation state
    updateNodeStatesFromSimulation();
}

void A2A03Visualizer::updateNodeStatesFromSimulation() {
    if (!sim_state_) return;
    
    // Read all node states from perfect2a03 into the visualization buffer
    // cpu_read_node_state_as_bytes writes active_value for HIGH nodes, 
    // inactive_value for LOW nodes
    cpu_read_node_state_as_bytes(
        sim_state_,
        NODE_ACTIVE,      // Value for active (HIGH) nodes
        NODE_INACTIVE,    // Value for inactive (LOW) nodes
        node_states_,
        A2A03_MAX_NODES
    );
}
