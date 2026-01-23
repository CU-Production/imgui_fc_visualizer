#pragma once

#include "sokol_gfx.h"
#include "imgui.h"
#include <cstdint>
#include <array>
#include <string>
#include <unordered_map>

// Forward declaration
class NesEmulator;

// Constants for 2A03 chip visualization
constexpr int A2A03_MAX_LAYERS = 6;
constexpr int A2A03_MAX_NODES = 8192;

// Color palette for chip layers
struct A2A03Palette {
    float colors[A2A03_MAX_LAYERS][4];  // RGBA for each layer
    float background[4];                 // Background color
};

// Default palette (from v6502r)
inline const A2A03Palette A2A03_DEFAULT_PALETTE = {
    {
        {0.96f, 0.00f, 0.34f, 1.0f},  // Layer 0 - Pink/Red
        {1.00f, 0.92f, 0.23f, 1.0f},  // Layer 1 - Yellow
        {1.00f, 0.32f, 0.32f, 1.0f},  // Layer 2 - Red (unused?)
        {0.49f, 0.34f, 0.76f, 0.7f},  // Layer 3 - Purple
        {0.98f, 0.55f, 0.00f, 0.7f},  // Layer 4 - Orange
        {0.00f, 0.69f, 1.00f, 1.0f},  // Layer 5 - Blue
    },
    {0.1f, 0.1f, 0.15f, 1.0f}         // Background
};

// 2A03 CPU state for visualization mapping
struct A2A03CpuState {
    uint8_t a;      // Accumulator
    uint8_t x;      // X register
    uint8_t y;      // Y register
    uint8_t sp;     // Stack pointer
    uint8_t p;      // Status register
    uint16_t pc;    // Program counter
    uint16_t addr;  // Current address bus
    uint8_t data;   // Current data bus
    bool rw;        // Read/Write (true = read)
};

// 2A03 APU state for visualization mapping
struct A2A03ApuState {
    // Square 1
    uint8_t sq0_out;
    uint16_t sq0_period;
    uint8_t sq0_volume;
    bool sq0_enabled;
    
    // Square 2
    uint8_t sq1_out;
    uint16_t sq1_period;
    uint8_t sq1_volume;
    bool sq1_enabled;
    
    // Triangle
    uint8_t tri_out;
    uint16_t tri_period;
    bool tri_enabled;
    
    // Noise
    uint8_t noi_out;
    uint8_t noi_volume;
    bool noi_enabled;
    
    // DMC
    uint8_t pcm_out;
    bool pcm_enabled;
};

class A2A03Visualizer {
public:
    A2A03Visualizer();
    ~A2A03Visualizer();
    
    // Initialize graphics resources
    bool init();
    
    // Shutdown and cleanup
    void shutdown();
    
    // Check if initialized
    bool isInitialized() const { return initialized_; }
    
    // Update node states from emulator (register-level only)
    void updateFromEmulator(const NesEmulator* emu);
    
    // Update using perfect2a03 transistor-level simulation
    // Steps the simulation and updates all node states
    // If num_half_cycles is -1, uses the configured sim_cycles_per_frame_
    void stepSimulation(int num_half_cycles = -1);
    
    // Reset the transistor simulation
    void resetSimulation();
    
    // Check if transistor simulation is enabled
    bool isSimulationEnabled() const { return sim_enabled_; }
    void setSimulationEnabled(bool enabled) { sim_enabled_ = enabled; }
    
    // Manual state update (for custom mapping)
    void updateCpuState(const A2A03CpuState& cpu_state);
    void updateApuState(const A2A03ApuState& apu_state);
    
    // Render the chip visualization to a texture
    void render(float width, float height);
    
    // Draw ImGui window with chip visualization
    void drawWindow(bool* p_open);
    
    // View controls
    void setOffset(float x, float y);
    void addOffset(float dx, float dy);
    float getOffsetX() const { return offset_x_; }
    float getOffsetY() const { return offset_y_; }
    
    void setScale(float scale);
    void addScale(float delta);
    float getScale() const { return scale_; }
    float getMinScale() const { return 1.0f; }
    float getMaxScale() const { return 100.0f; }
    
    // Layer visibility
    void setLayerVisible(int layer, bool visible);
    bool getLayerVisible(int layer) const;
    void toggleLayerVisible(int layer);
    
    // Node highlighting
    void highlightNode(int node_index);
    void clearHighlight();
    
    // Palette
    void setPalette(const A2A03Palette& palette);
    void setAdditiveBlend(bool additive) { use_additive_blend_ = additive; }
    
    // Get node info
    const char* getNodeName(int node_index) const;
    int findNodeByName(const char* name) const;
    
private:
    bool initialized_ = false;
    
    // Sokol graphics resources
    sg_buffer layer_buffers_[A2A03_MAX_LAYERS];
    int layer_vertex_counts_[A2A03_MAX_LAYERS];
    sg_pipeline pipeline_alpha_;
    sg_pipeline pipeline_add_;
    sg_image node_texture_;
    sg_view node_texture_view_;
    sg_sampler node_sampler_;
    sg_shader shader_alpha_;
    sg_shader shader_add_;
    
    // For rendering to texture (ImGui integration)
    sg_image render_target_;
    sg_view render_target_view_;        // For ImGui display
    sg_view color_attachment_view_;     // For render pass
    sg_sampler render_sampler_;
    int render_width_ = 0;
    int render_height_ = 0;
    
    // Node state buffer (brightness values)
    uint8_t node_states_[A2A03_MAX_NODES];
    
    // View state
    float offset_x_ = 0.0f;
    float offset_y_ = 0.0f;
    float scale_ = 9.0f;
    float aspect_ = 1.0f;
    
    // Chip geometry bounds
    uint16_t seg_min_x_ = 0;
    uint16_t seg_min_y_ = 0;
    uint16_t seg_max_x_ = 0;
    uint16_t seg_max_y_ = 0;
    
    // Layer visibility
    bool layer_visible_[A2A03_MAX_LAYERS];
    
    // Palette
    A2A03Palette palette_;
    bool use_additive_blend_ = false;
    
    // Node name lookup
    std::unordered_map<std::string, int> node_name_to_index_;
    
    // Node indices for CPU registers (cached for fast access)
    int node_a_[8] = {-1};      // a0-a7
    int node_x_[8] = {-1};      // x0-x7
    int node_y_[8] = {-1};      // y0-y7
    int node_sp_[8] = {-1};     // s0-s7
    int node_p_[8] = {-1};      // p0-p7
    int node_pcl_[8] = {-1};    // pcl0-pcl7
    int node_pch_[8] = {-1};    // pch0-pch7
    int node_db_[8] = {-1};     // db0-db7
    int node_ab_[16] = {-1};    // ab0-ab15
    
    // Node indices for APU (cached)
    int node_sq0_out_[4] = {-1};
    int node_sq1_out_[4] = {-1};
    int node_tri_out_[4] = {-1};
    int node_noi_out_[4] = {-1};
    int node_pcm_out_[7] = {-1};
    
    // Perfect2a03 transistor-level simulation
    void* sim_state_ = nullptr;     // perfect2a03 state (opaque pointer)
    bool sim_enabled_ = true;       // Enable transistor simulation
    int sim_cycles_per_frame_ = 100; // Half-cycles to simulate per frame
    
    // Helper methods
    void initNodeLookup();
    void cacheNodeIndices();
    void setNodeBits(const int* nodes, int count, uint32_t value);
    void setPerfect2a03NodeBits(const int* nodes, int count, uint32_t value);
    void updateRenderTarget(int width, int height);
    void renderChip();
    void initSimulation();
    void shutdownSimulation();
    void updateNodeStatesFromSimulation();
};
