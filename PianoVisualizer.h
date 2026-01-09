#pragma once

#include "imgui.h"
#include <vector>
#include <array>
#include <deque>
#include <mutex>
#include <cmath>
#include <functional>

// Forward declarations
struct Music_Emu;
class Nes_Apu;
class Nes_Vrc6_Apu;
struct tml_message;

// NES APU channel info for piano visualization
struct NesNoteInfo {
    int channel;        // 0-7: Square1, Square2, Triangle, Noise, DMC, VRC6_Pulse1, VRC6_Pulse2, VRC6_Saw
    int midi_note;      // MIDI note number (0-127)
    float velocity;     // 0.0 - 1.0
    bool active;        // Is the note currently playing
};

// Piano roll note event (preprocessed)
struct PianoRollNote {
    int channel;
    int midi_note;
    float velocity;
    float start_time;   // In seconds
    float end_time;     // In seconds (when note ends)
};

// Maximum channel count (base APU + VRC6)
constexpr int PIANO_NUM_CHANNELS_BASE = 5;
constexpr int PIANO_NUM_CHANNELS_VRC6 = 3;
constexpr int PIANO_NUM_CHANNELS_NES_MAX = 8;

// MIDI has 16 channels
constexpr int PIANO_NUM_CHANNELS_MIDI = 16;
constexpr int PIANO_NUM_CHANNELS_MAX = 16;  // Max of NES or MIDI

// Channel colors for piano visualization (NES)
inline const ImU32 PianoChannelColors[] = {
    IM_COL32(255, 80, 80, 220),   // Square 1 - Red
    IM_COL32(255, 160, 60, 220),  // Square 2 - Orange
    IM_COL32(80, 180, 255, 220),  // Triangle - Blue
    IM_COL32(230, 80, 230, 220),  // Noise - Magenta
    IM_COL32(230, 230, 80, 220),  // DMC - Yellow
    IM_COL32(60, 230, 130, 220),  // VRC6 Pulse1 - Green
    IM_COL32(100, 230, 180, 220), // VRC6 Pulse2 - Light Green
    IM_COL32(150, 100, 230, 220)  // VRC6 Saw - Purple
};

inline const char* PianoChannelNames[] = {
    "Sq1", "Sq2", "Tri", "Noi", "DMC", "V-P1", "V-P2", "V-Saw"
};

// MIDI channel colors (16 channels)
inline const ImU32 MidiChannelColors[] = {
    IM_COL32(255, 80, 80, 220),   // Ch 1 - Red
    IM_COL32(255, 140, 60, 220),  // Ch 2 - Orange
    IM_COL32(255, 200, 60, 220),  // Ch 3 - Yellow-Orange
    IM_COL32(220, 220, 80, 220),  // Ch 4 - Yellow
    IM_COL32(140, 220, 80, 220),  // Ch 5 - Yellow-Green
    IM_COL32(60, 220, 100, 220),  // Ch 6 - Green
    IM_COL32(60, 220, 180, 220),  // Ch 7 - Cyan-Green
    IM_COL32(60, 200, 220, 220),  // Ch 8 - Cyan
    IM_COL32(60, 140, 220, 220),  // Ch 9 - Light Blue
    IM_COL32(120, 80, 180, 220),  // Ch 10 (Drums) - Purple
    IM_COL32(180, 80, 220, 220),  // Ch 11 - Magenta
    IM_COL32(220, 80, 180, 220),  // Ch 12 - Pink
    IM_COL32(220, 100, 140, 220), // Ch 13 - Rose
    IM_COL32(180, 120, 100, 220), // Ch 14 - Brown
    IM_COL32(140, 140, 160, 220), // Ch 15 - Gray
    IM_COL32(100, 160, 180, 220)  // Ch 16 - Steel
};

// Callback type for getting APU data during preprocessing
using ApuDataCallback = std::function<Nes_Apu*(Music_Emu*)>;
using Vrc6DataCallback = std::function<Nes_Vrc6_Apu*(Music_Emu*)>;

class PianoVisualizer {
public:
    PianoVisualizer();
    ~PianoVisualizer() = default;

    // Reset state
    void reset();

    // Preprocess a track to generate all note data ahead of time
    // Returns true if successful, false if preprocessing failed
    // progress_callback: optional callback for progress updates (0.0-1.0)
    bool preprocessTrack(Music_Emu* emu, int track, long sample_rate,
                        ApuDataCallback apu_callback,
                        std::function<void(float)> progress_callback = nullptr,
                        Vrc6DataCallback vrc6_callback = nullptr);
    
    // Check if we have preprocessed data
    bool hasPreprocessedData() const { return has_preprocessed_data_; }
    
    // Get preprocessed track duration
    float getTrackDuration() const { return track_duration_; }

    // Update current playback time (for live keyboard display)
    void updatePlaybackTime(float current_time);
    
    // Update with NES APU register data for live keyboard highlighting
    void updateFromAPU(const int* periods, const int* lengths, const int* amplitudes, float current_time);
    
    // VRC6 support
    void setVRC6Enabled(bool enabled) { has_vrc6_ = enabled; }
    bool hasVRC6() const { return has_vrc6_; }
    void updateFromVRC6(const int* periods, const int* volumes, const bool* enabled, float current_time);
    int getActiveChannelCount() const { 
        if (is_midi_mode_) return PIANO_NUM_CHANNELS_MIDI;
        return has_vrc6_ ? PIANO_NUM_CHANNELS_NES_MAX : PIANO_NUM_CHANNELS_BASE; 
    }
    
    // MIDI support
    void setMidiMode(bool enabled) { is_midi_mode_ = enabled; }
    bool isMidiMode() const { return is_midi_mode_; }
    void midiNoteOn(int channel, int note, float velocity, float current_time);
    void midiNoteOff(int channel, int note, float current_time);
    void midiAllNotesOff();
    void preprocessMidi(const struct tml_message* midi_data);
    void updateMidiTime(float current_time);

    // Draw the piano keyboard
    void drawPianoKeyboard(const char* label, float width, float height);

    // Draw the piano roll (scrolling notes - shows FUTURE notes falling down)
    void drawPianoRoll(const char* label, float width, float height, float current_time);

    // Draw complete piano visualizer window
    void drawPianoWindow(bool* p_open, float current_time);

    // Settings
    void setPianoRollSpeed(float seconds_visible) { piano_roll_seconds_ = seconds_visible; }
    void setOctaveRange(int low, int high) { octave_low_ = low; octave_high_ = high; }

private:
    // Constants
    static constexpr int MIDI_NOTE_MIN = 21;   // A0
    static constexpr int MIDI_NOTE_MAX = 108;  // C8
    static constexpr float NES_CPU_CLOCK = 1789773.0f;  // NTSC

    // Current note state per channel (for live keyboard display)
    std::array<NesNoteInfo, PIANO_NUM_CHANNELS_MAX> current_notes_;
    
    // Preprocessed note data (sorted by start_time)
    std::vector<PianoRollNote> preprocessed_notes_;
    bool has_preprocessed_data_ = false;
    float track_duration_ = 0.0f;
    bool has_vrc6_ = false;
    bool is_midi_mode_ = false;
    
    // Active MIDI notes (for tracking note-off events)
    struct MidiNoteState {
        bool active = false;
        float start_time = 0.0f;
        float velocity = 0.0f;
    };
    std::array<std::array<MidiNoteState, 128>, PIANO_NUM_CHANNELS_MIDI> midi_note_states_;
    
    // For preprocessing: track note state
    std::array<int, PIANO_NUM_CHANNELS_MAX> preprocess_prev_notes_;
    std::array<float, PIANO_NUM_CHANNELS_MAX> preprocess_note_start_;
    std::array<float, PIANO_NUM_CHANNELS_MAX> preprocess_note_velocity_;
    
    // Settings
    float piano_roll_seconds_ = 3.0f;  // How many seconds of future notes to show
    int octave_low_ = 2;   // C2
    int octave_high_ = 7;  // C7
    
    // Thread safety
    std::mutex mutex_;
    
    // Helper functions
    static int frequencyToMidi(float frequency);
    static float midiToFrequency(int midi_note);
    static bool isBlackKey(int midi_note);
    static int getWhiteKeyIndex(int midi_note);
    static int getOctave(int midi_note);
    static int getNoteInOctave(int midi_note);
    
    void drawKey(ImDrawList* draw_list, ImVec2 pos, float width, float height, 
                 int midi_note, bool is_black, int pressed_channel, float velocity);
    
    // Process APU data during preprocessing
    void processApuFrame(const int* periods, const int* lengths, const int* amplitudes, float current_time);
    void processVrc6Frame(const int* periods, const int* volumes, const bool* enabled, float current_time);
    void finalizePreprocessing(float end_time);
    
    // Convert APU period to MIDI note
    int periodToMidi(int channel, int period);
};
