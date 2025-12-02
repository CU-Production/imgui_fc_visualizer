#include "AudioVisualizer.h"
#include <algorithm>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// SimpleFFT Implementation
// ============================================================================

void SimpleFFT::fft(std::vector<std::complex<float>>& data) {
    const size_t n = data.size();
    if (n <= 1) return;
    
    // Bit-reversal permutation
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    
    // Cooley-Tukey iterative FFT
    for (size_t len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
        std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; ++j) {
                std::complex<float> u = data[i + j];
                std::complex<float> v = data[i + j + len / 2] * w;
                data[i + j] = u + v;
                data[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

void SimpleFFT::computeMagnitude(const std::vector<std::complex<float>>& fftData,
                                  std::vector<float>& magnitudes, int numBins) {
    magnitudes.resize(numBins);
    const size_t fftSize = fftData.size();
    const size_t usefulBins = fftSize / 2;
    
    // Map FFT bins to display bins (logarithmic scale for better visualization)
    for (int i = 0; i < numBins; ++i) {
        // Logarithmic frequency mapping
        float t = static_cast<float>(i) / static_cast<float>(numBins);
        float logScale = std::pow(t, 2.0f); // Quadratic for more bass detail
        
        size_t startBin = static_cast<size_t>(logScale * usefulBins);
        size_t endBin = static_cast<size_t>(std::pow(static_cast<float>(i + 1) / numBins, 2.0f) * usefulBins);
        
        if (startBin >= usefulBins) startBin = usefulBins - 1;
        if (endBin >= usefulBins) endBin = usefulBins;
        if (endBin <= startBin) endBin = startBin + 1;
        
        float sum = 0.0f;
        for (size_t j = startBin; j < endBin; ++j) {
            float mag = std::abs(fftData[j]);
            sum += mag;
        }
        
        magnitudes[i] = sum / static_cast<float>(endBin - startBin);
    }
}

// ============================================================================
// AudioVisualizer Implementation
// ============================================================================

AudioVisualizer::AudioVisualizer()
    : emu_(nullptr)
    , sample_rate_(44100)
    , mute_mask_(0)
    , is_initialized_(false)
    , waveform_zoom_(1.0f)
    , spectrum_smoothing_(0.7f)
    , spectrum_history_pos_(0)
    , peak_decay_rate_(0.95f)
{
    // Initialize buffers
    waveform_buffer_.resize(WAVEFORM_SIZE, 0.0f);
    waveform_buffer_left_.resize(WAVEFORM_SIZE, 0.0f);
    waveform_buffer_right_.resize(WAVEFORM_SIZE, 0.0f);
    fft_input_.resize(FFT_SIZE, 0.0f);
    spectrum_data_.resize(SPECTRUM_BINS, 0.0f);
    spectrum_peaks_.resize(SPECTRUM_BINS, 0.0f);
    
    // Initialize spectrum history for waterfall
    spectrum_history_.resize(HISTORY_SIZE);
    for (auto& row : spectrum_history_) {
        row.resize(SPECTRUM_BINS, 0.0f);
    }
    
    // Initialize channel data
    channel_amplitudes_.fill(0.0f);
    channel_peaks_.fill(0.0f);
}

AudioVisualizer::~AudioVisualizer() {
    // Nothing special to clean up
}

bool AudioVisualizer::init(Music_Emu* emu, long sample_rate) {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    
    emu_ = emu;
    sample_rate_ = sample_rate;
    is_initialized_ = (emu != nullptr);
    
    // Call internal reset without locking (we already hold the lock)
    resetInternal();
    
    return is_initialized_;
}

void AudioVisualizer::reset() {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    resetInternal();
}

void AudioVisualizer::resetInternal() {
    // Note: This function assumes the caller already holds audio_mutex_
    
    // Clear all buffers
    std::fill(waveform_buffer_.begin(), waveform_buffer_.end(), 0.0f);
    std::fill(waveform_buffer_left_.begin(), waveform_buffer_left_.end(), 0.0f);
    std::fill(waveform_buffer_right_.begin(), waveform_buffer_right_.end(), 0.0f);
    std::fill(fft_input_.begin(), fft_input_.end(), 0.0f);
    std::fill(spectrum_data_.begin(), spectrum_data_.end(), 0.0f);
    std::fill(spectrum_peaks_.begin(), spectrum_peaks_.end(), 0.0f);
    
    for (auto& row : spectrum_history_) {
        std::fill(row.begin(), row.end(), 0.0f);
    }
    
    channel_amplitudes_.fill(0.0f);
    channel_peaks_.fill(0.0f);
    spectrum_history_pos_ = 0;
}

void AudioVisualizer::updateAudioData(const short* samples, int sample_count) {
    if (!samples || sample_count <= 0) return;
    
    std::lock_guard<std::mutex> lock(audio_mutex_);
    
    // Convert stereo samples to mono and update waveform buffer
    int mono_count = sample_count / 2;
    
    // Rolling window update for waveform
    int shift = std::min(mono_count, WAVEFORM_SIZE);
    
    if (shift < WAVEFORM_SIZE) {
        // Shift existing samples
        std::memmove(waveform_buffer_.data(), 
                     waveform_buffer_.data() + shift,
                     (WAVEFORM_SIZE - shift) * sizeof(float));
        std::memmove(waveform_buffer_left_.data(),
                     waveform_buffer_left_.data() + shift,
                     (WAVEFORM_SIZE - shift) * sizeof(float));
        std::memmove(waveform_buffer_right_.data(),
                     waveform_buffer_right_.data() + shift,
                     (WAVEFORM_SIZE - shift) * sizeof(float));
    }
    
    // Add new samples
    int start_idx = std::max(0, mono_count - shift);
    for (int i = 0; i < shift; ++i) {
        int src_idx = (start_idx + i) * 2;
        float left = samples[src_idx] / 32768.0f;
        float right = samples[src_idx + 1] / 32768.0f;
        float mono = (left + right) * 0.5f;
        
        int dst_idx = WAVEFORM_SIZE - shift + i;
        waveform_buffer_[dst_idx] = mono;
        waveform_buffer_left_[dst_idx] = left;
        waveform_buffer_right_[dst_idx] = right;
    }
    
    // Update FFT input buffer
    int fft_shift = std::min(mono_count, FFT_SIZE);
    if (fft_shift < FFT_SIZE) {
        std::memmove(fft_input_.data(),
                     fft_input_.data() + fft_shift,
                     (FFT_SIZE - fft_shift) * sizeof(float));
    }
    
    start_idx = std::max(0, mono_count - fft_shift);
    for (int i = 0; i < fft_shift; ++i) {
        int src_idx = (start_idx + i) * 2;
        float left = samples[src_idx] / 32768.0f;
        float right = samples[src_idx + 1] / 32768.0f;
        fft_input_[FFT_SIZE - fft_shift + i] = (left + right) * 0.5f;
    }
    
    // Process FFT and update spectrum
    processFFT();
    
    // Update channel amplitudes (rough estimation from overall signal)
    updateChannelAmplitudes(samples, sample_count);
}

void AudioVisualizer::processFFT() {
    // Apply Hann window and prepare complex data
    std::vector<std::complex<float>> fftData(FFT_SIZE);
    for (int i = 0; i < FFT_SIZE; ++i) {
        float window = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (FFT_SIZE - 1)));
        fftData[i] = std::complex<float>(fft_input_[i] * window, 0.0f);
    }
    
    // Perform FFT
    SimpleFFT::fft(fftData);
    
    // Compute magnitudes
    std::vector<float> newSpectrum;
    SimpleFFT::computeMagnitude(fftData, newSpectrum, SPECTRUM_BINS);
    
    // Convert to dB and normalize
    for (int i = 0; i < SPECTRUM_BINS; ++i) {
        // Add small value to avoid log(0)
        float db = 20.0f * std::log10(newSpectrum[i] + 1e-10f);
        // Normalize to 0-1 range (assuming -60dB to 0dB range)
        float normalized = (db + 60.0f) / 60.0f;
        normalized = std::clamp(normalized, 0.0f, 1.0f);
        
        // Smooth with previous values
        spectrum_data_[i] = spectrum_smoothing_ * spectrum_data_[i] + 
                           (1.0f - spectrum_smoothing_) * normalized;
        
        // Update peaks
        if (spectrum_data_[i] > spectrum_peaks_[i]) {
            spectrum_peaks_[i] = spectrum_data_[i];
        }
    }
    
    // Update history for waterfall display
    spectrum_history_[spectrum_history_pos_] = spectrum_data_;
    spectrum_history_pos_ = (spectrum_history_pos_ + 1) % HISTORY_SIZE;
}

void AudioVisualizer::updateChannelAmplitudes(const short* samples, int sample_count) {
    // Calculate overall RMS
    float rms = 0.0f;
    int mono_count = sample_count / 2;
    for (int i = 0; i < mono_count; ++i) {
        float left = samples[i * 2] / 32768.0f;
        float right = samples[i * 2 + 1] / 32768.0f;
        float mono = (left + right) * 0.5f;
        rms += mono * mono;
    }
    rms = std::sqrt(rms / std::max(1, mono_count));
    
    // Distribute amplitude across channels (estimation)
    // In reality, we'd need separate channel buffers from the APU
    // For now, we simulate based on frequency content
    for (int i = 0; i < static_cast<int>(NesChannel::Count); ++i) {
        // Decay existing amplitude
        channel_amplitudes_[i] *= 0.9f;
        
        // Add contribution based on overall amplitude
        // This is a rough approximation
        float contribution = rms * (1.0f - (mute_mask_ & (1 << i) ? 1.0f : 0.0f));
        channel_amplitudes_[i] = std::max(channel_amplitudes_[i], contribution);
        
        // Update peaks
        if (channel_amplitudes_[i] > channel_peaks_[i]) {
            channel_peaks_[i] = channel_amplitudes_[i];
        }
    }
}

void AudioVisualizer::decayPeaks(float delta_time) {
    float decay = std::pow(peak_decay_rate_, delta_time * 60.0f);
    
    for (auto& peak : spectrum_peaks_) {
        peak *= decay;
    }
    
    for (auto& peak : channel_peaks_) {
        peak *= decay;
    }
}

void AudioVisualizer::setChannelMute(NesChannel channel, bool mute) {
    int bit = 1 << static_cast<int>(channel);
    if (mute) {
        mute_mask_ |= bit;
    } else {
        mute_mask_ &= ~bit;
    }
    
    // Apply to emulator if available
    if (emu_) {
        gme_mute_voices(emu_, mute_mask_);
    }
}

bool AudioVisualizer::isChannelMuted(NesChannel channel) const {
    return (mute_mask_ & (1 << static_cast<int>(channel))) != 0;
}

ImU32 AudioVisualizer::vec4ToU32(const ImVec4& col) {
    return IM_COL32(
        static_cast<int>(col.x * 255),
        static_cast<int>(col.y * 255),
        static_cast<int>(col.z * 255),
        static_cast<int>(col.w * 255)
    );
}

ImU32 AudioVisualizer::getSpectrumColor(float normalized_value, float normalized_freq) {
    // Create a nice gradient from blue (low) to cyan to green to yellow to red (high)
    float h = (1.0f - normalized_value) * 0.7f; // Hue from red to blue
    float s = 0.8f + 0.2f * normalized_freq;
    float v = 0.3f + 0.7f * normalized_value;
    
    // HSV to RGB conversion
    float c = v * s;
    float x = c * (1.0f - std::abs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    float m = v - c;
    
    float r, g, b;
    int hi = static_cast<int>(h * 6.0f) % 6;
    switch (hi) {
        case 0: r = c; g = x; b = 0; break;
        case 1: r = x; g = c; b = 0; break;
        case 2: r = 0; g = c; b = x; break;
        case 3: r = 0; g = x; b = c; break;
        case 4: r = x; g = 0; b = c; break;
        default: r = c; g = 0; b = x; break;
    }
    
    return IM_COL32(
        static_cast<int>((r + m) * 255),
        static_cast<int>((g + m) * 255),
        static_cast<int>((b + m) * 255),
        255
    );
}

void AudioVisualizer::drawVisualizerWindow(bool* p_open) {
    ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);
    
    if (!ImGui::Begin("Audio Visualizer", p_open)) {
        ImGui::End();
        return;
    }
    
    // Decay peaks
    decayPeaks(ImGui::GetIO().DeltaTime);
    
    // Top section: Waveform and Spectrum side by side
    float available_width = ImGui::GetContentRegionAvail().x;
    float section_width = (available_width - 10) / 2;
    
    ImGui::BeginChild("Waveform Section", ImVec2(section_width, 180), true);
    ImGui::Text("Waveform");
    ImGui::Separator();
    drawWaveformScope("##waveform", section_width - 16, 140);
    ImGui::EndChild();
    
    ImGui::SameLine();
    
    ImGui::BeginChild("Spectrum Section", ImVec2(section_width, 180), true);
    ImGui::Text("Spectrum Analyzer");
    ImGui::Separator();
    drawSpectrumAnalyzer("##spectrum", section_width - 16, 140);
    ImGui::EndChild();
    
    // Middle section: Volume meters
    ImGui::BeginChild("Meters Section", ImVec2(available_width, 100), true);
    ImGui::Text("Channel Levels");
    ImGui::Separator();
    drawVolumeMeters(available_width - 16, 60);
    ImGui::EndChild();
    
    // Bottom section: Channel controls
    ImGui::BeginChild("Controls Section", ImVec2(available_width, 0), true);
    ImGui::Text("Channel Controls");
    ImGui::Separator();
    drawChannelInfo();
    ImGui::EndChild();
    
    ImGui::End();
}

void AudioVisualizer::drawWaveformScope(const char* label, float width, float height) {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size(width, height);
    
    // Background
    draw_list->AddRectFilled(canvas_pos,
                            ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                            IM_COL32(15, 15, 25, 255));
    
    // Grid lines
    float center_y = canvas_pos.y + canvas_size.y * 0.5f;
    draw_list->AddLine(ImVec2(canvas_pos.x, center_y),
                      ImVec2(canvas_pos.x + canvas_size.x, center_y),
                      IM_COL32(60, 60, 80, 255), 1.0f);
    
    // Draw 25% and 75% lines
    float quarter_y = canvas_size.y * 0.25f;
    draw_list->AddLine(ImVec2(canvas_pos.x, canvas_pos.y + quarter_y),
                      ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + quarter_y),
                      IM_COL32(40, 40, 60, 255), 1.0f);
    draw_list->AddLine(ImVec2(canvas_pos.x, canvas_pos.y + canvas_size.y - quarter_y),
                      ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y - quarter_y),
                      IM_COL32(40, 40, 60, 255), 1.0f);
    
    // Draw left channel (cyan)
    if (waveform_buffer_left_.size() > 1) {
        int sample_count = static_cast<int>(waveform_buffer_left_.size());
        float step_x = canvas_size.x / static_cast<float>(sample_count - 1);
        
        for (int i = 1; i < sample_count; ++i) {
            float x1 = canvas_pos.x + (i - 1) * step_x;
            float x2 = canvas_pos.x + i * step_x;
            float y1 = center_y - waveform_buffer_left_[i - 1] * canvas_size.y * 0.45f * waveform_zoom_;
            float y2 = center_y - waveform_buffer_left_[i] * canvas_size.y * 0.45f * waveform_zoom_;
            
            y1 = std::clamp(y1, canvas_pos.y, canvas_pos.y + canvas_size.y);
            y2 = std::clamp(y2, canvas_pos.y, canvas_pos.y + canvas_size.y);
            
            draw_list->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(100, 200, 255, 180), 1.0f);
        }
    }
    
    // Draw right channel (orange)
    if (waveform_buffer_right_.size() > 1) {
        int sample_count = static_cast<int>(waveform_buffer_right_.size());
        float step_x = canvas_size.x / static_cast<float>(sample_count - 1);
        
        for (int i = 1; i < sample_count; ++i) {
            float x1 = canvas_pos.x + (i - 1) * step_x;
            float x2 = canvas_pos.x + i * step_x;
            float y1 = center_y - waveform_buffer_right_[i - 1] * canvas_size.y * 0.45f * waveform_zoom_;
            float y2 = center_y - waveform_buffer_right_[i] * canvas_size.y * 0.45f * waveform_zoom_;
            
            y1 = std::clamp(y1, canvas_pos.y, canvas_pos.y + canvas_size.y);
            y2 = std::clamp(y2, canvas_pos.y, canvas_pos.y + canvas_size.y);
            
            draw_list->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(255, 180, 100, 180), 1.0f);
        }
    }
    
    // Border
    draw_list->AddRect(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                      IM_COL32(80, 80, 100, 255));
    
    ImGui::Dummy(canvas_size);
}

void AudioVisualizer::drawSpectrumAnalyzer(const char* label, float width, float height) {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size(width, height);
    
    // Background with gradient
    draw_list->AddRectFilledMultiColor(
        canvas_pos,
        ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
        IM_COL32(10, 10, 20, 255),
        IM_COL32(10, 10, 20, 255),
        IM_COL32(20, 15, 30, 255),
        IM_COL32(20, 15, 30, 255)
    );
    
    // Draw spectrum bars
    float bar_width = canvas_size.x / static_cast<float>(SPECTRUM_BINS);
    float bar_gap = 1.0f;
    
    for (int i = 0; i < SPECTRUM_BINS; ++i) {
        float x = canvas_pos.x + i * bar_width;
        float bar_height = spectrum_data_[i] * canvas_size.y;
        float peak_height = spectrum_peaks_[i] * canvas_size.y;
        
        // Bar gradient
        float normalized_freq = static_cast<float>(i) / SPECTRUM_BINS;
        ImU32 bar_color_top = getSpectrumColor(spectrum_data_[i], normalized_freq);
        ImU32 bar_color_bottom = getSpectrumColor(spectrum_data_[i] * 0.3f, normalized_freq);
        
        // Draw bar with gradient
        draw_list->AddRectFilledMultiColor(
            ImVec2(x + bar_gap, canvas_pos.y + canvas_size.y - bar_height),
            ImVec2(x + bar_width - bar_gap, canvas_pos.y + canvas_size.y),
            bar_color_top, bar_color_top,
            bar_color_bottom, bar_color_bottom
        );
        
        // Draw peak indicator
        if (peak_height > 2) {
            draw_list->AddRectFilled(
                ImVec2(x + bar_gap, canvas_pos.y + canvas_size.y - peak_height),
                ImVec2(x + bar_width - bar_gap, canvas_pos.y + canvas_size.y - peak_height + 2),
                IM_COL32(255, 255, 255, 200)
            );
        }
    }
    
    // Border
    draw_list->AddRect(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                      IM_COL32(80, 80, 100, 255));
    
    ImGui::Dummy(canvas_size);
}

void AudioVisualizer::drawVolumeMeters(float width, float height) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 start_pos = ImGui::GetCursorScreenPos();
    
    float meter_width = (width - 20) / static_cast<float>(NesChannel::Count);
    float meter_height = height - 20;
    
    for (int i = 0; i < static_cast<int>(NesChannel::Count); ++i) {
        float x = start_pos.x + i * (meter_width + 4);
        float y = start_pos.y;
        
        // Background
        draw_list->AddRectFilled(
            ImVec2(x, y),
            ImVec2(x + meter_width - 4, y + meter_height),
            IM_COL32(30, 30, 40, 255)
        );
        
        // Level bar
        float level = channel_amplitudes_[i];
        float bar_height = level * meter_height * 5.0f; // Scale up for visibility
        bar_height = std::min(bar_height, meter_height);
        
        ImVec4 color = ChannelColors[i];
        if (mute_mask_ & (1 << i)) {
            color.w = 0.3f; // Dim if muted
        }
        
        // Gradient bar
        draw_list->AddRectFilledMultiColor(
            ImVec2(x, y + meter_height - bar_height),
            ImVec2(x + meter_width - 4, y + meter_height),
            vec4ToU32(color),
            vec4ToU32(color),
            vec4ToU32(ImVec4(color.x * 0.5f, color.y * 0.5f, color.z * 0.5f, color.w)),
            vec4ToU32(ImVec4(color.x * 0.5f, color.y * 0.5f, color.z * 0.5f, color.w))
        );
        
        // Peak indicator
        float peak_y = y + meter_height - channel_peaks_[i] * meter_height * 5.0f;
        peak_y = std::max(peak_y, y);
        draw_list->AddLine(
            ImVec2(x, peak_y),
            ImVec2(x + meter_width - 4, peak_y),
            IM_COL32(255, 255, 255, 200),
            2.0f
        );
        
        // Border
        draw_list->AddRect(
            ImVec2(x, y),
            ImVec2(x + meter_width - 4, y + meter_height),
            IM_COL32(80, 80, 100, 255)
        );
    }
    
    // Channel labels below
    ImGui::Dummy(ImVec2(width, meter_height + 5));
    for (int i = 0; i < static_cast<int>(NesChannel::Count); ++i) {
        if (i > 0) ImGui::SameLine();
        float label_width = meter_width - 4;
        ImGui::PushItemWidth(label_width);
        ImGui::TextColored(ChannelColors[i], "%s", ChannelNames[i]);
        ImGui::PopItemWidth();
    }
}

void AudioVisualizer::drawChannelInfo() {
    // Display channel mute toggles
    ImGui::Columns(5, "channel_controls", false);
    
    for (int i = 0; i < static_cast<int>(NesChannel::Count); ++i) {
        NesChannel channel = static_cast<NesChannel>(i);
        bool muted = isChannelMuted(channel);
        
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ChannelColors[i]);
        
        char label[32];
        snprintf(label, sizeof(label), "%s##mute%d", ChannelNames[i], i);
        
        if (ImGui::Checkbox(label, &muted)) {
            setChannelMute(channel, muted);
        }
        
        // Show amplitude bar
        float amp = channel_amplitudes_[i];
        ImGui::ProgressBar(amp * 5.0f, ImVec2(-1, 8), "");
        
        ImGui::PopStyleColor();
        ImGui::NextColumn();
    }
    
    ImGui::Columns(1);
    
    ImGui::Separator();
    
    // Settings
    ImGui::Text("Settings");
    ImGui::SliderFloat("Waveform Zoom", &waveform_zoom_, 0.5f, 4.0f);
    ImGui::SliderFloat("Spectrum Smoothing", &spectrum_smoothing_, 0.0f, 0.95f);
    
    // Quick mute buttons
    ImGui::Separator();
    if (ImGui::Button("Mute All")) {
        mute_mask_ = 0x1F; // All 5 channels
        if (emu_) gme_mute_voices(emu_, mute_mask_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Unmute All")) {
        mute_mask_ = 0;
        if (emu_) gme_mute_voices(emu_, mute_mask_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Solo Square")) {
        mute_mask_ = 0x1C; // Mute Triangle, Noise, DMC
        if (emu_) gme_mute_voices(emu_, mute_mask_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Solo Triangle")) {
        mute_mask_ = 0x1B; // Mute others
        if (emu_) gme_mute_voices(emu_, mute_mask_);
    }
}
