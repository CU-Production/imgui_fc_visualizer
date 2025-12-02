#pragma once

#include "gme/gme.h"
#include "gme/Music_Emu.h"
#include "imgui.h"
#include <vector>
#include <array>
#include <mutex>
#include <cmath>
#include <complex>

// NES APU channel types
enum class NesChannel {
    Square1 = 0,
    Square2 = 1,
    Triangle = 2,
    Noise = 3,
    DMC = 4,
    Count = 5
};

// Channel names
inline const char* const ChannelNames[] = {
    "Square 1",
    "Square 2",
    "Triangle",
    "Noise",
    "DMC"
};

// Channel colors (RGBA)
inline const ImVec4 ChannelColors[] = {
    ImVec4(1.0f, 0.3f, 0.3f, 1.0f),  // Square 1 - Red
    ImVec4(1.0f, 0.6f, 0.2f, 1.0f),  // Square 2 - Orange
    ImVec4(0.3f, 0.7f, 1.0f, 1.0f),  // Triangle - Blue
    ImVec4(0.9f, 0.3f, 0.9f, 1.0f),  // Noise - Magenta
    ImVec4(0.9f, 0.9f, 0.3f, 1.0f)   // DMC - Yellow
};

// Simple FFT implementation for spectrum analysis
class SimpleFFT {
public:
    static void fft(std::vector<std::complex<float>>& data);
    static void computeMagnitude(const std::vector<std::complex<float>>& fftData, 
                                  std::vector<float>& magnitudes, int numBins);
};

// Audio visualizer class
class AudioVisualizer {
public:
    AudioVisualizer();
    ~AudioVisualizer();

    // Initialize visualizer
    bool init(Music_Emu* emu, long sample_rate);
    
    // Reset when loading new file
    void reset();

    // Update audio data (called in audio callback)
    void updateAudioData(const short* samples, int sample_count);

    // Draw the complete visualizer window
    void drawVisualizerWindow(bool* p_open = nullptr);

    // Individual drawing functions
    void drawWaveformScope(const char* label, float width, float height);
    void drawSpectrumAnalyzer(const char* label, float width, float height);
    void drawVolumeMeters(float width, float height);
    void drawChannelInfo();
    
    // Channel muting control
    void setChannelMute(NesChannel channel, bool mute);
    bool isChannelMuted(NesChannel channel) const;
    int getMuteMask() const { return mute_mask_; }
    
    // Settings
    void setWaveformZoom(float zoom) { waveform_zoom_ = zoom; }
    float getWaveformZoom() const { return waveform_zoom_; }
    
    void setSpectrumSmoothing(float smooth) { spectrum_smoothing_ = smooth; }
    float getSpectrumSmoothing() const { return spectrum_smoothing_; }

private:
    // Buffer sizes
    static constexpr int WAVEFORM_SIZE = 1024;    // Samples for waveform display
    static constexpr int FFT_SIZE = 2048;         // FFT size (must be power of 2)
    static constexpr int SPECTRUM_BINS = 64;      // Number of frequency bins to display
    static constexpr int HISTORY_SIZE = 128;      // History for waterfall display
    
    // Audio buffers
    std::vector<float> waveform_buffer_;          // Current waveform
    std::vector<float> waveform_buffer_left_;     // Left channel
    std::vector<float> waveform_buffer_right_;    // Right channel
    std::vector<float> fft_input_;                // FFT input buffer
    std::vector<float> spectrum_data_;            // Current spectrum
    std::vector<float> spectrum_peaks_;           // Peak hold for spectrum
    std::vector<std::vector<float>> spectrum_history_; // History for waterfall
    
    // Per-channel amplitude (estimated from mixed output)
    std::array<float, static_cast<size_t>(NesChannel::Count)> channel_amplitudes_;
    std::array<float, static_cast<size_t>(NesChannel::Count)> channel_peaks_;
    
    // Thread safety
    std::mutex audio_mutex_;
    
    // State
    Music_Emu* emu_;
    long sample_rate_;
    int mute_mask_;
    bool is_initialized_;
    
    // Visual settings
    float waveform_zoom_;
    float spectrum_smoothing_;
    int spectrum_history_pos_;
    
    // Timing for peak decay
    float peak_decay_rate_;
    
    // Helper functions
    void resetInternal();  // Internal reset without locking (caller must hold mutex)
    void processFFT();
    void updateChannelAmplitudes(const short* samples, int sample_count);
    void drawWaveformGraph(const std::vector<float>& samples, ImVec2 pos, ImVec2 size, ImVec4 color);
    void drawSpectrumBars(ImVec2 pos, ImVec2 size);
    void decayPeaks(float delta_time);
    
    // Color helpers
    ImU32 getSpectrumColor(float normalized_value, float normalized_freq);
    ImU32 vec4ToU32(const ImVec4& col);
};
