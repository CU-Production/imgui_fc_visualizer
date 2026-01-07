#include "PianoVisualizer.h"
#include "gme/gme.h"
#include "gme/Nes_Apu.h"
#include "gme/Nes_Vrc6_Apu.h"
#include <algorithm>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

PianoVisualizer::PianoVisualizer() {
    reset();
}

void PianoVisualizer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (int i = 0; i < PIANO_NUM_CHANNELS_MAX; ++i) {
        current_notes_[i] = {i, 0, 0.0f, false};
        preprocess_prev_notes_[i] = -1;
        preprocess_note_start_[i] = 0.0f;
        preprocess_note_velocity_[i] = 0.0f;
    }
    
    preprocessed_notes_.clear();
    has_preprocessed_data_ = false;
    track_duration_ = 0.0f;
}

int PianoVisualizer::frequencyToMidi(float frequency) {
    if (frequency <= 0) return -1;
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
    return note == 1 || note == 3 || note == 6 || note == 8 || note == 10;
}

int PianoVisualizer::getWhiteKeyIndex(int midi_note) {
    int octave = midi_note / 12;
    int note = midi_note % 12;
    static const int white_key_offsets[] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    return octave * 7 + white_key_offsets[note];
}

int PianoVisualizer::getOctave(int midi_note) {
    return midi_note / 12 - 1;
}

int PianoVisualizer::getNoteInOctave(int midi_note) {
    return midi_note % 12;
}

int PianoVisualizer::periodToMidi(int channel, int period) {
    if (period < 8) return -1;  // Too high frequency / silent
    
    float freq = 0;
    
    if (channel == 3) {
        // Noise channel - map to low notes
        int noise_idx = period & 0x0F;
        return 36 + (15 - noise_idx);  // C2 to C3 range
    } else if (channel == 4) {
        // DMC - fixed low note
        return 28;  // E1
    } else {
        // Square1, Square2, Triangle
        freq = NES_CPU_CLOCK / (16.0f * (period + 1));
    }
    
    return frequencyToMidi(freq);
}

void PianoVisualizer::processApuFrame(const int* periods, const int* lengths, const int* amplitudes, float current_time) {
    for (int ch = 0; ch < PIANO_NUM_CHANNELS_BASE; ++ch) {
        int period = periods[ch];
        int length = lengths[ch];
        int amp = std::abs(amplitudes[ch]);
        
        int midi_note = -1;
        float velocity = 0;
        
        if (ch == 3) {
            // Noise
            if (length > 0 && amp > 0) {
                midi_note = 36 + (15 - (period & 0x0F));
                velocity = std::min(1.0f, amp / 15.0f);
            }
        } else if (ch == 4) {
            // DMC - check if actively playing (length > 0 means bytes remaining)
            if (length > 0) {
                midi_note = 28;
                velocity = 0.8f;  // Fixed velocity when DMC is playing
            }
        } else if (ch == 2) {
            // Triangle - doesn't have volume control, just on/off
            // last_amp is the waveform position (0-15), not volume
            if (length > 0 && period >= 8) {
                float freq = NES_CPU_CLOCK / (16.0f * (period + 1));
                midi_note = frequencyToMidi(freq);
                velocity = 0.8f;  // Fixed velocity for Triangle
            }
        } else {
            // Square1, Square2
            if (length > 0 && amp > 0 && period >= 8) {
                float freq = NES_CPU_CLOCK / (16.0f * (period + 1));
                midi_note = frequencyToMidi(freq);
                velocity = std::min(1.0f, amp / 15.0f);
            }
        }
        
        int prev_note = preprocess_prev_notes_[ch];
        
        // Note changed or ended
        if (midi_note != prev_note || velocity < 0.01f) {
            // End previous note
            if (prev_note >= 0 && prev_note <= 127) {
                PianoRollNote note;
                note.channel = ch;
                note.midi_note = prev_note;
                note.velocity = preprocess_note_velocity_[ch];
                note.start_time = preprocess_note_start_[ch];
                note.end_time = current_time;
                
                // Only add if note has meaningful duration
                if (note.end_time - note.start_time > 0.01f) {
                    preprocessed_notes_.push_back(note);
                }
            }
            
            // Start new note
            if (midi_note >= 0 && midi_note <= 127 && velocity > 0.01f) {
                preprocess_prev_notes_[ch] = midi_note;
                preprocess_note_start_[ch] = current_time;
                preprocess_note_velocity_[ch] = velocity;
            } else {
                preprocess_prev_notes_[ch] = -1;
            }
        }
    }
}

void PianoVisualizer::processVrc6Frame(const int* periods, const int* volumes, const bool* enabled, float current_time) {
    // VRC6 channels: Pulse1 (ch 5), Pulse2 (ch 6), Saw (ch 7)
    for (int i = 0; i < PIANO_NUM_CHANNELS_VRC6; ++i) {
        int ch = PIANO_NUM_CHANNELS_BASE + i;  // 5, 6, 7
        int period = periods[i];
        int volume = volumes[i];
        bool is_enabled = enabled[i];
        
        int midi_note = -1;
        float velocity = 0;
        
        if (is_enabled && volume > 0 && period >= 1) {
            // VRC6 period formula: freq = CPU_CLOCK / (16 * (period + 1))
            float freq = NES_CPU_CLOCK / (16.0f * (period + 1));
            midi_note = frequencyToMidi(freq);
            
            if (i < 2) {
                // Pulse waves - 4-bit volume (0-15)
                velocity = std::min(1.0f, volume / 15.0f);
            } else {
                // Saw wave - accumulator rate (0-63)
                velocity = std::min(1.0f, volume / 42.0f);
            }
        }
        
        int prev_note = preprocess_prev_notes_[ch];
        
        // Note changed or ended
        if (midi_note != prev_note || velocity < 0.01f) {
            // End previous note
            if (prev_note >= 0 && prev_note <= 127) {
                PianoRollNote note;
                note.channel = ch;
                note.midi_note = prev_note;
                note.velocity = preprocess_note_velocity_[ch];
                note.start_time = preprocess_note_start_[ch];
                note.end_time = current_time;
                
                if (note.end_time - note.start_time > 0.01f) {
                    preprocessed_notes_.push_back(note);
                }
            }
            
            // Start new note
            if (midi_note >= 0 && midi_note <= 127 && velocity > 0.01f) {
                preprocess_prev_notes_[ch] = midi_note;
                preprocess_note_start_[ch] = current_time;
                preprocess_note_velocity_[ch] = velocity;
            } else {
                preprocess_prev_notes_[ch] = -1;
            }
        }
    }
}

void PianoVisualizer::finalizePreprocessing(float end_time) {
    // End any notes still playing
    for (int ch = 0; ch < PIANO_NUM_CHANNELS_MAX; ++ch) {
        int prev_note = preprocess_prev_notes_[ch];
        if (prev_note >= 0 && prev_note <= 127) {
            PianoRollNote note;
            note.channel = ch;
            note.midi_note = prev_note;
            note.velocity = preprocess_note_velocity_[ch];
            note.start_time = preprocess_note_start_[ch];
            note.end_time = end_time;
            
            if (note.end_time - note.start_time > 0.01f) {
                preprocessed_notes_.push_back(note);
            }
        }
        preprocess_prev_notes_[ch] = -1;
    }
    
    // Sort notes by start time
    std::sort(preprocessed_notes_.begin(), preprocessed_notes_.end(),
              [](const PianoRollNote& a, const PianoRollNote& b) {
                  return a.start_time < b.start_time;
              });
    
    track_duration_ = end_time;
    has_preprocessed_data_ = true;
}

bool PianoVisualizer::preprocessTrack(Music_Emu* emu, int track, long sample_rate,
                                       ApuDataCallback apu_callback,
                                       std::function<void(float)> progress_callback,
                                       Vrc6DataCallback vrc6_callback) {
    if (!emu || !apu_callback) return false;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Reset state
    preprocessed_notes_.clear();
    has_preprocessed_data_ = false;
    track_duration_ = 0.0f;
    
    // Check if VRC6 is available
    has_vrc6_ = (vrc6_callback != nullptr) && (vrc6_callback(emu) != nullptr);
    
    for (int i = 0; i < PIANO_NUM_CHANNELS_MAX; ++i) {
        preprocess_prev_notes_[i] = -1;
        preprocess_note_start_[i] = 0.0f;
        preprocess_note_velocity_[i] = 0.0f;
    }
    
    // Get track info for duration estimate
    track_info_t info;
    if (gme_track_info(emu, &info, track) != nullptr) {
        return false;
    }
    
    // Estimate duration (use length if available, otherwise default to 3 minutes)
    float estimated_duration = info.length > 0 ? info.length / 1000.0f : 180.0f;
    // Cap at 5 minutes for preprocessing
    estimated_duration = std::min(estimated_duration, 300.0f);
    
    // Start the track
    if (gme_start_track(emu, track) != nullptr) {
        return false;
    }
    
    // Process audio in chunks to extract note data
    const int chunk_samples = 1024;  // Stereo samples
    std::vector<short> buffer(chunk_samples * 2);
    
    float current_time = 0;
    float time_per_chunk = static_cast<float>(chunk_samples) / sample_rate;
    int chunks_processed = 0;
    int total_chunks = static_cast<int>(estimated_duration / time_per_chunk);
    
    while (current_time < estimated_duration && !gme_track_ended(emu)) {
        // Generate audio (we need this to advance the emulator state)
        gme_play(emu, chunk_samples * 2, buffer.data());
        
        // Get APU state
        Nes_Apu* apu = apu_callback(emu);
        if (apu) {
            int periods[5], lengths[5], amplitudes[5];
            for (int i = 0; i < 5; ++i) {
                periods[i] = apu->osc_period(i);
                lengths[i] = apu->osc_length(i);
                amplitudes[i] = apu->osc_amplitude(i);
            }
            processApuFrame(periods, lengths, amplitudes, current_time);
        }
        
        // Get VRC6 state if available
        if (has_vrc6_ && vrc6_callback) {
            Nes_Vrc6_Apu* vrc6 = vrc6_callback(emu);
            if (vrc6) {
                int vrc6_periods[3], vrc6_volumes[3];
                bool vrc6_enabled[3];
                for (int i = 0; i < 3; ++i) {
                    vrc6_periods[i] = vrc6->osc_period(i);
                    vrc6_volumes[i] = vrc6->osc_volume(i);
                    vrc6_enabled[i] = vrc6->osc_enabled(i);
                }
                processVrc6Frame(vrc6_periods, vrc6_volumes, vrc6_enabled, current_time);
            }
        }
        
        current_time += time_per_chunk;
        chunks_processed++;
        
        // Progress callback
        if (progress_callback && chunks_processed % 100 == 0) {
            float progress = std::min(1.0f, current_time / estimated_duration);
            progress_callback(progress);
        }
    }
    
    // Finalize
    finalizePreprocessing(current_time);
    
    if (progress_callback) {
        progress_callback(1.0f);
    }
    
    return true;
}

void PianoVisualizer::updatePlaybackTime(float current_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Update current notes based on preprocessed data
    for (int ch = 0; ch < PIANO_NUM_CHANNELS_MAX; ++ch) {
        current_notes_[ch].active = false;
    }
    
    // Find notes that are active at current_time
    for (const auto& note : preprocessed_notes_) {
        if (note.start_time <= current_time && note.end_time > current_time) {
            int ch = note.channel;
            if (ch >= 0 && ch < PIANO_NUM_CHANNELS_MAX) {
                current_notes_[ch].midi_note = note.midi_note;
                current_notes_[ch].velocity = note.velocity;
                current_notes_[ch].active = true;
            }
        }
    }
}

void PianoVisualizer::updateFromAPU(const int* periods, const int* lengths, const int* amplitudes, float current_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Update current notes for live keyboard display (base APU channels only)
    for (int ch = 0; ch < PIANO_NUM_CHANNELS_BASE; ++ch) {
        int period = periods[ch];
        int length = lengths[ch];
        int amp = std::abs(amplitudes[ch]);
        
        int midi_note = -1;
        float velocity = 0;
        
        if (ch == 3) {
            if (length > 0 && amp > 0) {
                midi_note = 36 + (15 - (period & 0x0F));
                velocity = std::min(1.0f, amp / 15.0f);
            }
        } else if (ch == 4) {
            // DMC - check if actively playing
            if (length > 0) {
                midi_note = 28;
                velocity = 0.8f;  // Fixed velocity when DMC is playing
            }
        } else if (ch == 2) {
            // Triangle - no volume control, just on/off
            if (length > 0 && period >= 8) {
                float freq = NES_CPU_CLOCK / (16.0f * (period + 1));
                midi_note = frequencyToMidi(freq);
                velocity = 0.8f;  // Fixed velocity for Triangle
            }
        } else {
            // Square1, Square2
            if (length > 0 && amp > 0 && period >= 8) {
                float freq = NES_CPU_CLOCK / (16.0f * (period + 1));
                midi_note = frequencyToMidi(freq);
                velocity = std::min(1.0f, amp / 15.0f);
            }
        }
        
        if (midi_note >= 0 && midi_note <= 127 && velocity > 0.01f) {
            current_notes_[ch].midi_note = midi_note;
            current_notes_[ch].velocity = velocity;
            current_notes_[ch].active = true;
        } else {
            current_notes_[ch].active = false;
        }
    }
}

void PianoVisualizer::updateFromVRC6(const int* periods, const int* volumes, const bool* enabled, float current_time) {
    if (!has_vrc6_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // VRC6 channels: Pulse1 (5), Pulse2 (6), Saw (7)
    for (int i = 0; i < PIANO_NUM_CHANNELS_VRC6; ++i) {
        int ch = PIANO_NUM_CHANNELS_BASE + i;  // 5, 6, 7
        int period = periods[i];
        int volume = volumes[i];
        bool is_enabled = enabled[i];
        
        int midi_note = -1;
        float velocity = 0;
        
        if (is_enabled && volume > 0 && period >= 1) {
            // VRC6 period formula: freq = CPU_CLOCK / (16 * (period + 1))
            // Same as NES square waves
            float freq = NES_CPU_CLOCK / (16.0f * (period + 1));
            midi_note = frequencyToMidi(freq);
            
            if (i < 2) {
                // Pulse waves - 4-bit volume (0-15)
                velocity = std::min(1.0f, volume / 15.0f);
            } else {
                // Saw wave - accumulator rate (0-63)
                velocity = std::min(1.0f, volume / 42.0f);  // Typical max is around 42
            }
        }
        
        if (midi_note >= 0 && midi_note <= 127 && velocity > 0.01f) {
            current_notes_[ch].midi_note = midi_note;
            current_notes_[ch].velocity = velocity;
            current_notes_[ch].active = true;
        } else {
            current_notes_[ch].active = false;
        }
    }
}

void PianoVisualizer::drawKey(ImDrawList* draw_list, ImVec2 pos, float width, float height,
                               int midi_note, bool is_black, int pressed_channel, float velocity) {
    ImU32 key_color;
    ImU32 border_color = IM_COL32(40, 40, 40, 255);
    
    if (pressed_channel >= 0 && velocity > 0.05f) {
        key_color = PianoChannelColors[pressed_channel];
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
    
    int start_note = octave_low_ * 12 + 12;
    int end_note = octave_high_ * 12 + 12;
    
    int white_key_count = 0;
    for (int note = start_note; note <= end_note; ++note) {
        if (!isBlackKey(note)) white_key_count++;
    }
    
    float white_key_width = width / white_key_count;
    float white_key_height = height;
    float black_key_width = white_key_width * 0.65f;
    float black_key_height = height * 0.6f;
    
    // Build map of pressed keys
    std::array<int, 128> note_channel;
    std::array<float, 128> note_velocity;
    note_channel.fill(-1);
    note_velocity.fill(0.0f);
    
    for (int ch = 0; ch < getActiveChannelCount(); ++ch) {
        if (current_notes_[ch].active && current_notes_[ch].midi_note >= 0 && 
            current_notes_[ch].midi_note < 128) {
            int note = current_notes_[ch].midi_note;
            if (note_channel[note] < 0 || current_notes_[ch].velocity > note_velocity[note]) {
                note_channel[note] = ch;
                note_velocity[note] = current_notes_[ch].velocity;
            }
        }
    }
    
    // Draw white keys
    int white_key_idx = 0;
    for (int note = start_note; note <= end_note; ++note) {
        if (!isBlackKey(note)) {
            ImVec2 key_pos(canvas_pos.x + white_key_idx * white_key_width, canvas_pos.y);
            drawKey(draw_list, key_pos, white_key_width - 1, white_key_height,
                   note, false, note_channel[note], note_velocity[note]);
            white_key_idx++;
        }
    }
    
    // Draw black keys
    white_key_idx = 0;
    for (int note = start_note; note <= end_note; ++note) {
        if (!isBlackKey(note)) {
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
        if (!isBlackKey(note) && getNoteInOctave(note) == 0) {
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
                            IM_COL32(20, 20, 28, 255));
    
    int start_note = octave_low_ * 12 + 12;
    int end_note = octave_high_ * 12 + 12;
    
    // Count white keys
    int white_key_count = 0;
    for (int note = start_note; note <= end_note; ++note) {
        if (!isBlackKey(note)) white_key_count++;
    }
    
    float white_key_width = width / white_key_count;
    float black_key_width = white_key_width * 0.65f;
    
    // Time range: show FUTURE notes (current_time at bottom, future at top)
    float time_end = current_time + piano_roll_seconds_;
    float pixels_per_second = height / piano_roll_seconds_;
    
    // Draw lane backgrounds
    int white_key_idx = 0;
    for (int note = start_note; note <= end_note; ++note) {
        if (!isBlackKey(note)) {
            float x = canvas_pos.x + white_key_idx * white_key_width;
            ImU32 lane_color = (getNoteInOctave(note) == 0) ? 
                IM_COL32(35, 35, 45, 255) : IM_COL32(28, 28, 36, 255);
            draw_list->AddRectFilled(
                ImVec2(x, canvas_pos.y),
                ImVec2(x + white_key_width, canvas_pos.y + height),
                lane_color
            );
            draw_list->AddLine(
                ImVec2(x, canvas_pos.y),
                ImVec2(x, canvas_pos.y + height),
                IM_COL32(50, 50, 60, 255)
            );
            white_key_idx++;
        }
    }
    
    // Draw time grid lines
    float time_grid = 0.5f;
    float grid_start = std::floor(current_time / time_grid) * time_grid;
    for (float t = grid_start; t <= time_end; t += time_grid) {
        if (t < current_time) continue;
        // Y: bottom = current_time, top = time_end
        float y = canvas_pos.y + height - (t - current_time) * pixels_per_second;
        if (y >= canvas_pos.y && y <= canvas_pos.y + height) {
            draw_list->AddLine(
                ImVec2(canvas_pos.x, y),
                ImVec2(canvas_pos.x + width, y),
                IM_COL32(45, 45, 55, 255)
            );
        }
    }
    
    // Helper to get X position for a note
    auto getNoteX = [&](int midi_note) -> std::pair<float, float> {
        if (midi_note < start_note || midi_note > end_note) 
            return {-1, -1};
        
        int white_idx = 0;
        for (int n = start_note; n < midi_note; ++n) {
            if (!isBlackKey(n)) white_idx++;
        }
        
        if (isBlackKey(midi_note)) {
            float x = canvas_pos.x + white_idx * white_key_width - black_key_width / 2;
            return {x, black_key_width};
        } else {
            float x = canvas_pos.x + white_idx * white_key_width;
            return {x, white_key_width - 1};
        }
    };
    
    // Draw notes from preprocessed data
    if (has_preprocessed_data_) {
        for (const auto& note : preprocessed_notes_) {
            // Only show notes in the visible time window
            if (note.end_time < current_time || note.start_time > time_end) continue;
            if (note.midi_note < start_note || note.midi_note > end_note) continue;
            
            // Y positions: bottom = current_time, top = future
            // note.start_time -> y2 (note starts, appears from top)
            // note.end_time -> y1 (note ends, reaches bottom and disappears)
            float y_start = canvas_pos.y + height - (note.start_time - current_time) * pixels_per_second;
            float y_end = canvas_pos.y + height - (note.end_time - current_time) * pixels_per_second;
            
            // y1 is top (smaller Y, earlier/end), y2 is bottom (larger Y, later/start)
            float y1 = std::max(y_end, canvas_pos.y);
            float y2 = std::min(y_start, canvas_pos.y + height);
            
            if (y2 <= y1) continue;
            
            auto [note_x, note_width] = getNoteX(note.midi_note);
            if (note_x < 0) continue;
            
            ImU32 note_color = PianoChannelColors[note.channel];
            
            // Glow effect for notes about to be played
            bool about_to_play = (note.start_time <= current_time + 0.1f && note.start_time >= current_time);
            if (about_to_play) {
                ImU32 glow_color = note_color & 0x00FFFFFF;
                glow_color |= 0x60000000;
                draw_list->AddRectFilled(
                    ImVec2(note_x - 3, y1 - 3),
                    ImVec2(note_x + note_width + 3, y2 + 3),
                    glow_color, 5.0f
                );
            }
            
            // Draw note
            draw_list->AddRectFilled(
                ImVec2(note_x + 1, y1),
                ImVec2(note_x + note_width - 1, y2),
                note_color, 3.0f
            );
            
            draw_list->AddRect(
                ImVec2(note_x + 1, y1),
                ImVec2(note_x + note_width - 1, y2),
                IM_COL32(255, 255, 255, 80), 3.0f
            );
        }
    }
    
    // Draw hit line at bottom
    draw_list->AddLine(
        ImVec2(canvas_pos.x, canvas_pos.y + height - 2),
        ImVec2(canvas_pos.x + width, canvas_pos.y + height - 2),
        IM_COL32(255, 255, 255, 180), 3.0f
    );
    
    // Border
    draw_list->AddRect(canvas_pos, 
                      ImVec2(canvas_pos.x + width, canvas_pos.y + height),
                      IM_COL32(60, 60, 80, 255));
    
    ImGui::Dummy(ImVec2(width, height));
}

void PianoVisualizer::drawPianoWindow(bool* p_open, float current_time) {
    ImGui::SetNextWindowSize(ImVec2(900, 500), ImGuiCond_FirstUseEver);
    
    if (!ImGui::Begin("Piano Visualizer", p_open)) {
        ImGui::End();
        return;
    }
    
    float available_width = ImGui::GetContentRegionAvail().x;
    float available_height = ImGui::GetContentRegionAvail().y;
    
    // Status and legend
    if (has_preprocessed_data_) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Ready");
        ImGui::SameLine();
        ImGui::Text("(%.1fs)", track_duration_);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "No data - load a track to preprocess");
    }
    
    ImGui::SameLine(150);
    for (int i = 0; i < getActiveChannelCount(); ++i) {
        ImVec4 color = ImGui::ColorConvertU32ToFloat4(PianoChannelColors[i]);
        ImGui::ColorButton(PianoChannelNames[i], color, ImGuiColorEditFlags_NoTooltip, ImVec2(16, 14));
        ImGui::SameLine();
        ImGui::Text("%s", PianoChannelNames[i]);
        ImGui::SameLine();
    }
    
    ImGui::SameLine(available_width - 280);
    ImGui::SetNextItemWidth(80);
    ImGui::SliderFloat("Ahead", &piano_roll_seconds_, 1.0f, 6.0f, "%.1fs");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::SliderInt("##oct1", &octave_low_, 1, 5);
    ImGui::SameLine();
    ImGui::Text("-");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::SliderInt("##oct2", &octave_high_, octave_low_ + 1, 7);
    
    ImGui::Separator();
    
    // Calculate sizes
    float keyboard_height = 90;
    float roll_height = available_height - keyboard_height - 30;
    
    // Piano roll (future notes falling down)
    drawPianoRoll("##roll", available_width, roll_height, current_time);
    
    // Keyboard (at bottom)
    drawPianoKeyboard("##keyboard", available_width, keyboard_height);
    
    ImGui::End();
}
