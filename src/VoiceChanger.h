#pragma once

#include <vector>

#include "Gain.h"
#include "Project.h"

namespace zrecord {

// Offline voice changer: pitch shift that keeps duration, with an
// independent formant shift, plus an optional robot (monotone) voice.
//
// Implementation: a phase vocoder (STFT, Hann window, 4x overlap). Each
// frame's magnitude spectrum is split into a smooth spectral envelope
// (cepstral smoothing, ~1 ms lifter) and the excitation (magnitude divided
// by the envelope). The excitation is moved to its new frequencies with its
// phase advanced from each partial's measured true frequency, so pitch moves
// by exactly 2^(semitones/12) while the analysis and synthesis hop stay
// equal (duration is unchanged, sample for sample). The envelope is then
// re-applied, stretched by 2^(formantSemitones/12): at 0 the formants stay
// where they were (a natural-sounding shifted voice), and setting it equal
// to the pitch shift moves them with the pitch (the classic chipmunk).
// Robot sets every synthesis phase to zero each hop, which leaves the
// spectrum's shape but replaces the pitch with a buzz at sampleRate / hop.
struct VoiceSettings {
    float pitchSemitones = 0.0f;   // +/-12
    float formantSemitones = 0.0f; // +/-12, relative to the original formants
    bool robot = false;

    bool isIdentity() const { return pitchSemitones == 0.0f && formantSemitones == 0.0f && !robot; }
    bool operator==(const VoiceSettings& o) const {
        return pitchSemitones == o.pitchSemitones && formantSemitones == o.formantSemitones && robot == o.robot;
    }
};

enum class VoicePreset { Deeper, Higher, Robot, Chipmunk, Custom };

// The settings a preset stands for (Custom: identity, the dialog keeps
// whatever the user set).
VoiceSettings voicePresetSettings(VoicePreset preset);
const char* voicePresetName(VoicePreset preset);

// STFT frame size used at `sampleRate` (2048 at 44.1/48 kHz); hop is 1/4.
size_t voiceFrameSize(double sampleRate);

// Processes one channel. The result has exactly input.size() samples, and
// its overall RMS is matched to the input's. Identity settings return the
// input unchanged.
std::vector<float> processVoice(const std::vector<float>& mono, double sampleRate, const VoiceSettings& settings);

// Interleaved convenience: each channel processed independently.
void processVoiceInterleaved(std::vector<float>& interleaved, int channels, double sampleRate,
                             const VoiceSettings& settings);

// Runs the voice changer over the audio inside `targets`, clip by clip,
// writing the result back in place (copy-on-write) and rebuilding peaks.
void applyVoiceChange(Project& project, const std::vector<GainTarget>& targets, const VoiceSettings& settings);

} // namespace zrecord
