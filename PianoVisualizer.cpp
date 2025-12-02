#include "PianoVisualizer.h"
#include <algorithm>
#include <cstring>
#include <complex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

PianoVisualizer::PianoVisualizer() {
    reset();
    fft_buffer_.resize(FFT_SIZE, 0.0f);
}

void PianoVisualizer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        current_notes_[i] = {i, 0, 0.0f, false};
        prev_midi_notes_[i] = -1;
        note_start_times_[i] = 0.0f;
    }
    
    piano_roll_notes_.clear();
}

int PianoVisualizer::frequencyToMidi(float frequency) {
    if (frequency <= 0) return -1;
    // MIDI note = 69 + 12 * log2(freq / 440)
    float midi = 69.0f + 12.0f * std::log2(frequency / 440.0f);
    int note = static_cast<int>(std::round(midi));
    if (note < 0 || note > 127) return -1;
    return note;
}

float PianoVisualizer::midiToFrequency(int midi_note) {
    return 440.0f * std::pow(2.0f, (midi_note - 69) / 12.0f);
}

bool PianoVisualizer::isBlackKey(int midi_note) {
    int note = midi_note % 12;
    // C=0, C#=1, D=2, D#=3, E=4, F=5, F#=6, G=7, G#=8, A=9, A#=10, B=11
    return note == 1 || note == 3 || note == 6 || note == 8 || note == 10;
}

int PianoVisualizer::getWhiteKeyIndex(int midi_note) {
    int octave = midi_note / 12;
    int note = midi_note % 12;
    // Count white keys before this note in the octave
    static const int white_key_offsets[] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    return octave * 7 + white_key_offsets[note];
}

int PianoVisualizer::getOctave(int midi_note) {
    return midi_note / 12 - 1;  // MIDI octave convention
}

int PianoVisualizer::getNoteInOctave(int midi_note) {
    return midi_note % 12;
}

void PianoVisualizer::processNoteChange(int channel, int new_midi_note, float velocity, float current_time) {
    int prev_note = prev_midi_notes_[channel];
    
    // End previous note if it was playing
    if (prev_note >= 0 && prev_note != new_midi_note) {
        // Find the active note and set its duration
        for (auto& note : piano_roll_notes_) {
            if (note.channel == channel && note.midi_note == prev_note && note.active) {
                note.active = false;
                note.duration = current_time - note.start_time;
                break;
            }
        }
    }
    
    // Start new note
    if (new_midi_note >= 0 && velocity > 0.05f && new_midi_note != prev_note) {
        PianoRollNote new_note;
        new_note.channel = channel;
        new_note.midi_note = new_midi_note;
        new_note.velocity = velocity;
        new_note.start_time = current_time;
        new_note.duration = 0;
        new_note.active = true;
        
        piano_roll_notes_.push_back(new_note);
        note_start_times_[channel] = current_time;
        
        // Limit the number of notes
        while (piano_roll_notes_.size() > MAX_ROLL_NOTES) {
            piano_roll_notes_.pop_front();
        }
    }
    
    prev_midi_notes_[channel] = (velocity > 0.05f) ? new_midi_note : -1;
    
    // Update current note state
    current_notes_[channel].midi_note = new_midi_note;
    current_notes_[channel].velocity = velocity;
    current_notes_[channel].active = (new_midi_note >= 0 && velocity > 0.05f);
}

void PianoVisualizer::updateFromFrequencies(const float* frequencies, const float* amplitudes, float current_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        float freq = frequencies[ch];
        float amp = amplitudes[ch];
        
        int midi_note = frequencyToMidi(freq);
        processNoteChange(ch, midi_note, amp, current_time);
    }
}

void PianoVisualizer::updateFromAPU(const int* periods, const int* lengths, const int* amplitudes, float current_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        int period = periods[ch];
        int length = lengths[ch];
        int amp = std::abs(amplitudes[ch]);  // Amplitude can be negative
        
        // Calculate frequency from period
        // NES APU frequency formula:
        // Square/Triangle: freq = CPU_CLOCK / (16 * (period + 1))
        float freq = 0;
        int midi_note = -1;
        float velocity = 0;
        
        if (ch == 3) {
            // Noise channel - map to low notes as rhythm indicator
            // Use a fixed low note range (C2-C3) based on noise "pitch" setting
            int noise_idx = period & 0x0F;
            if (length > 0 && amp > 0) {
                // Map noise period (0-15) to notes C2 (36) to C3 (48)
                midi_note = 36 + (15 - noise_idx);  // Lower period = higher "pitch"
                velocity = std::min(1.0f, static_cast<float>(amp) / 15.0f);
            }
        } else if (ch == 4) {
            // DMC channel - show as a low bass note when active
            if (length > 0 && amp > 0) {
                midi_note = 28;  // E1 - very low note for DMC
                velocity = std::min(1.0f, static_cast<float>(amp) / 127.0f);
            }
        } else {
            // Square1, Square2, Triangle
            // Skip if channel is silent
            if (length == 0 || amp == 0 || period < 8) {
                // Note off (period < 8 produces ultrasonic frequencies)
                processNoteChange(ch, -1, 0.0f, current_time);
                continue;
            }
            
            freq = NES_CPU_CLOCK / (16.0f * (period + 1));
            midi_note = frequencyToMidi(freq);
            
            // Normalize amplitude
            // Square channels: 0-15, Triangle: 0-15 (but uses different scale)
            float max_amp = (ch == 2) ? 15.0f : 15.0f;
            velocity = std::min(1.0f, static_cast<float>(amp) / max_amp);
        }
        
        // Only show notes within reasonable MIDI range
        if (midi_note >= 0 && midi_note <= 127 && velocity > 0.01f) {
            processNoteChange(ch, midi_note, velocity, current_time);
        } else {
            processNoteChange(ch, -1, 0.0f, current_time);
        }
    }
}

float PianoVisualizer::detectFrequency(const float* samples, int count, long sample_rate) {
    if (count < 64) return 0;
    
    // Simple autocorrelation-based pitch detection
    int max_lag = std::min(count / 2, static_cast<int>(sample_rate / 50));  // Min ~50 Hz
    int min_lag = static_cast<int>(sample_rate / 2000);  // Max ~2000 Hz
    
    float best_correlation = 0;
    int best_lag = 0;
    
    for (int lag = min_lag; lag < max_lag; ++lag) {
        float correlation = 0;
        float energy1 = 0;
        float energy2 = 0;
        
        int compare_count = count - lag;
        for (int i = 0; i < compare_count; ++i) {
            correlation += samples[i] * samples[i + lag];
            energy1 += samples[i] * samples[i];
            energy2 += samples[i + lag] * samples[i + lag];
        }
        
        if (energy1 > 0 && energy2 > 0) {
            correlation /= std::sqrt(energy1 * energy2);
            if (correlation > best_correlation) {
                best_correlation = correlation;
                best_lag = lag;
            }
        }
    }
    
    if (best_correlation > 0.5f && best_lag > 0) {
        return static_cast<float>(sample_rate) / best_lag;
    }
    
    return 0;
}

void PianoVisualizer::updateFromAudio(const short* samples, int sample_count, long sample_rate, float current_time) {
    if (!samples || sample_count < 128) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Convert to float and mono
    int mono_count = sample_count / 2;
    std::vector<float> mono_samples(mono_count);
    
    for (int i = 0; i < mono_count; ++i) {
        float left = samples[i * 2] / 32768.0f;
        float right = samples[i * 2 + 1] / 32768.0f;
        mono_samples[i] = (left + right) * 0.5f;
    }
    
    // Calculate RMS for amplitude
    float rms = 0;
    for (float s : mono_samples) {
        rms += s * s;
    }
    rms = std::sqrt(rms / mono_samples.size());
    
    // Detect dominant frequency
    float freq = detectFrequency(mono_samples.data(), mono_count, sample_rate);
    int midi_note = frequencyToMidi(freq);
    
    // For now, assign to triangle channel (most melodic)
    // In a more sophisticated implementation, we'd analyze multiple frequency peaks
    processNoteChange(2, midi_note, rms * 3.0f, current_time);  // Triangle
    
    // Simple estimation for other channels based on frequency bands
    // This is a rough approximation
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        if (ch != 2) {  // Skip triangle, already processed
            current_notes_[ch].velocity *= 0.9f;  // Decay
            if (current_notes_[ch].velocity < 0.05f) {
                current_notes_[ch].active = false;
            }
        }
    }
}

void PianoVisualizer::drawKey(ImDrawList* draw_list, ImVec2 pos, float width, float height,
                               int midi_note, bool is_black, int pressed_channel, float velocity) {
    ImU32 key_color;
    ImU32 border_color = IM_COL32(40, 40, 40, 255);
    
    if (pressed_channel >= 0 && velocity > 0.05f) {
        // Key is pressed - use channel color
        key_color = PianoChannelColors[pressed_channel];
        // Modulate brightness by velocity
        int r = (key_color & 0xFF);
        int g = (key_color >> 8) & 0xFF;
        int b = (key_color >> 16) & 0xFF;
        float bright = 0.5f + 0.5f * velocity;
        key_color = IM_COL32(
            static_cast<int>(r * bright),
            static_cast<int>(g * bright),
            static_cast<int>(b * bright),
            220
        );
    } else {
        key_color = is_black ? IM_COL32(30, 30, 35, 255) : IM_COL32(250, 250, 250, 255);
    }
    
    draw_list->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), key_color, 2.0f);
    draw_list->AddRect(pos, ImVec2(pos.x + width, pos.y + height), border_color, 2.0f);
}

void PianoVisualizer::drawPianoKeyboard(const char* label, float width, float height) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    
    // Calculate key dimensions
    int start_note = octave_low_ * 12 + 12;  // C of start octave (add 12 for MIDI convention)
    int end_note = octave_high_ * 12 + 12;
    
    // Count white keys
    int white_key_count = 0;
    for (int note = start_note; note <= end_note; ++note) {
        if (!isBlackKey(note)) white_key_count++;
    }
    
    float white_key_width = width / white_key_count;
    float white_key_height = height;
    float black_key_width = white_key_width * 0.65f;
    float black_key_height = height * 0.6f;
    
    // Build map of which channel is pressing which note
    std::array<int, 128> note_channel;
    std::array<float, 128> note_velocity;
    note_channel.fill(-1);
    note_velocity.fill(0.0f);
    
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        if (current_notes_[ch].active && current_notes_[ch].midi_note >= 0 && 
            current_notes_[ch].midi_note < 128) {
            int note = current_notes_[ch].midi_note;
            // Higher priority to later channels or higher velocity
            if (note_channel[note] < 0 || current_notes_[ch].velocity > note_velocity[note]) {
                note_channel[note] = ch;
                note_velocity[note] = current_notes_[ch].velocity;
            }
        }
    }
    
    // Draw white keys first
    int white_key_idx = 0;
    for (int note = start_note; note <= end_note; ++note) {
        if (!isBlackKey(note)) {
            ImVec2 key_pos(canvas_pos.x + white_key_idx * white_key_width, canvas_pos.y);
            drawKey(draw_list, key_pos, white_key_width - 1, white_key_height,
                   note, false, note_channel[note], note_velocity[note]);
            white_key_idx++;
        }
    }
    
    // Draw black keys on top
    white_key_idx = 0;
    for (int note = start_note; note <= end_note; ++note) {
        if (!isBlackKey(note)) {
            // Check if next note is black
            if (note + 1 <= end_note && isBlackKey(note + 1)) {
                float black_x = canvas_pos.x + white_key_idx * white_key_width + 
                               white_key_width - black_key_width / 2;
                ImVec2 key_pos(black_x, canvas_pos.y);
                drawKey(draw_list, key_pos, black_key_width, black_key_height,
                       note + 1, true, note_channel[note + 1], note_velocity[note + 1]);
            }
            white_key_idx++;
        }
    }
    
    // Draw octave labels
    white_key_idx = 0;
    for (int note = start_note; note <= end_note; ++note) {
        if (!isBlackKey(note) && getNoteInOctave(note) == 0) {  // C note
            float label_x = canvas_pos.x + white_key_idx * white_key_width + 2;
            float label_y = canvas_pos.y + white_key_height - 14;
            char octave_label[8];
            snprintf(octave_label, sizeof(octave_label), "C%d", getOctave(note));
            draw_list->AddText(ImVec2(label_x, label_y), IM_COL32(100, 100, 100, 255), octave_label);
        }
        if (!isBlackKey(note)) white_key_idx++;
    }
    
    ImGui::Dummy(ImVec2(width, height));
}

void PianoVisualizer::drawPianoRoll(const char* label, float width, float height, float current_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    
    // Background
    draw_list->AddRectFilled(canvas_pos, 
                            ImVec2(canvas_pos.x + width, canvas_pos.y + height),
                            IM_COL32(25, 25, 30, 255));
    
    // Calculate note range
    int start_note = octave_low_ * 12 + 12;
    int end_note = octave_high_ * 12 + 12;
    int note_range = end_note - start_note + 1;
    
    float note_height = height / note_range;
    float time_start = current_time - piano_roll_seconds_;
    float pixels_per_second = width / piano_roll_seconds_;
    
    // Draw horizontal grid lines (for each note)
    for (int note = start_note; note <= end_note; ++note) {
        float y = canvas_pos.y + (end_note - note) * note_height;
        ImU32 line_color = isBlackKey(note) ? IM_COL32(35, 35, 40, 255) : IM_COL32(45, 45, 55, 255);
        draw_list->AddLine(ImVec2(canvas_pos.x, y), 
                          ImVec2(canvas_pos.x + width, y), line_color);
        
        // Highlight C notes
        if (getNoteInOctave(note) == 0) {
            draw_list->AddLine(ImVec2(canvas_pos.x, y), 
                              ImVec2(canvas_pos.x + width, y), 
                              IM_COL32(60, 60, 70, 255), 2.0f);
        }
    }
    
    // Draw vertical grid lines (time markers)
    float time_grid = 0.5f;  // Every 0.5 seconds
    float grid_start = std::floor(time_start / time_grid) * time_grid;
    for (float t = grid_start; t <= current_time; t += time_grid) {
        float x = canvas_pos.x + (t - time_start) * pixels_per_second;
        if (x >= canvas_pos.x && x <= canvas_pos.x + width) {
            draw_list->AddLine(ImVec2(x, canvas_pos.y), 
                              ImVec2(x, canvas_pos.y + height), 
                              IM_COL32(50, 50, 60, 255));
        }
    }
    
    // Draw notes
    for (const auto& note : piano_roll_notes_) {
        if (note.midi_note < start_note || note.midi_note > end_note) continue;
        
        float note_end_time = note.active ? current_time : (note.start_time + note.duration);
        
        // Skip notes completely outside the visible range
        if (note_end_time < time_start || note.start_time > current_time) continue;
        
        float x1 = canvas_pos.x + (note.start_time - time_start) * pixels_per_second;
        float x2 = canvas_pos.x + (note_end_time - time_start) * pixels_per_second;
        
        // Clamp to visible area
        x1 = std::max(x1, canvas_pos.x);
        x2 = std::min(x2, canvas_pos.x + width);
        
        if (x2 <= x1) continue;
        
        float y = canvas_pos.y + (end_note - note.midi_note) * note_height;
        
        ImU32 note_color = PianoChannelColors[note.channel];
        
        // Draw note rectangle
        draw_list->AddRectFilled(
            ImVec2(x1, y + 1),
            ImVec2(x2, y + note_height - 1),
            note_color, 2.0f
        );
        
        // Draw note border
        draw_list->AddRect(
            ImVec2(x1, y + 1),
            ImVec2(x2, y + note_height - 1),
            IM_COL32(255, 255, 255, 100), 2.0f
        );
    }
    
    // Draw playhead
    float playhead_x = canvas_pos.x + width;
    draw_list->AddLine(
        ImVec2(playhead_x, canvas_pos.y),
        ImVec2(playhead_x, canvas_pos.y + height),
        IM_COL32(255, 255, 255, 200), 2.0f
    );
    
    // Border
    draw_list->AddRect(canvas_pos, 
                      ImVec2(canvas_pos.x + width, canvas_pos.y + height),
                      IM_COL32(80, 80, 100, 255));
    
    ImGui::Dummy(ImVec2(width, height));
}

void PianoVisualizer::drawPianoWindow(bool* p_open, float current_time) {
    ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_FirstUseEver);
    
    if (!ImGui::Begin("Piano Visualizer", p_open)) {
        ImGui::End();
        return;
    }
    
    float available_width = ImGui::GetContentRegionAvail().x;
    
    // Legend
    ImGui::Text("Channels:");
    ImGui::SameLine();
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        ImVec4 color = ImGui::ColorConvertU32ToFloat4(PianoChannelColors[i]);
        ImGui::SameLine();
        ImGui::ColorButton(PianoChannelNames[i], color, ImGuiColorEditFlags_NoTooltip, ImVec2(20, 14));
        ImGui::SameLine();
        ImGui::Text("%s", PianoChannelNames[i]);
    }
    
    ImGui::Separator();
    
    // Settings
    ImGui::SliderFloat("Roll Speed (sec)", &piano_roll_seconds_, 1.0f, 10.0f);
    ImGui::SliderInt("Octave Low", &octave_low_, 0, 6);
    ImGui::SliderInt("Octave High", &octave_high_, octave_low_ + 1, 8);
    
    ImGui::Separator();
    
    // Piano roll (top section)
    ImGui::Text("Piano Roll");
    float roll_height = ImGui::GetContentRegionAvail().y - 100;
    drawPianoRoll("##roll", available_width, roll_height, current_time);
    
    // Piano keyboard (bottom section)
    ImGui::Text("Keyboard");
    drawPianoKeyboard("##keyboard", available_width, 80);
    
    ImGui::End();
}

