#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Filters.h"

namespace zrecord {

// Non-destructive per-track effects, applied on playback and export (the
// idea of a per-track effect stack follows Audacity/Tenacity's realtime
// effects: ideas only, no code). The take itself is recorded raw; a track's
// stack only changes what is heard, until it is baked into the audio.

enum class EffectType {
    Gain,
    HighPass,
    LowPass,
    NoiseGate,
    Compressor,
    Robot,
    Echo,
    DeepVoice,
    Chipmunk,
    Distortion,
    Limiter,
};
constexpr int kEffectTypeCount = 11;
constexpr int kMaxEffectParams = 3;

struct EffectParamInfo {
    const char* key;   // stable name, used in project files
    const char* label; // shown in the UI
    const char* unit;  // "dB", "Hz", "ms", "" ...
    double minValue;
    double maxValue;
    double defaultValue;
    int decimals; // for display and spin boxes
};

struct EffectInfo {
    EffectType type;
    const char* key;  // stable name, used in project files
    const char* name; // shown in the UI
    int paramCount;
    EffectParamInfo params[kMaxEffectParams];
};

const EffectInfo& effectInfo(EffectType type);
// Looks up a type by its project-file key; false if unknown.
bool effectTypeFromKey(const std::string& key, EffectType& type);

// One effect in a track's stack: its type, parameters (meaning per
// effectInfo(type).params) and whether it is bypassed. Plain data.
struct Effect {
    EffectType type = EffectType::Gain;
    bool bypassed = false;
    std::array<double, kMaxEffectParams> params{};

    // An effect of `type` with every parameter at its default.
    static Effect make(EffectType type);
    // Clamps every parameter into its range.
    void clampParams();
    double param(int index) const { return params[static_cast<size_t>(index)]; }

    bool operator==(const Effect& other) const {
        return type == other.type && bypassed == other.bypassed && params == other.params;
    }
    bool operator!=(const Effect& other) const { return !(*this == other); }
};

// True if some effect in `effects` is not bypassed.
bool anyEffectActive(const std::vector<Effect>& effects);
// "Echo, Gain" (bypassed ones in parentheses), for tooltips.
std::string describeEffects(const std::vector<Effect>& effects);

// The runtime instance of one Effect over interleaved audio: per-channel DSP
// state, allocated once by prepare(). After that, apply() (new parameters or
// bypass), reset() and process() never allocate or lock.
class EffectProcessor {
public:
    // Longest echo delay the UI allows; the delay line is reserved for it.
    static constexpr double kMaxEchoDelayMs = 2000.0;

    // Not real-time safe.
    void prepare(const Effect& effect, double sampleRate, int channels);
    // Real-time safe. `effect.type` must be the prepared type. Coefficients
    // are updated in place and running state is kept, so a parameter change
    // doesn't click; an effect coming out of bypass starts from rest.
    void apply(const Effect& effect);
    void reset();
    // Processes `frames` interleaved frames in place (nothing when bypassed).
    void process(float* interleaved, size_t frames);

    const Effect& effect() const { return effect_; }

private:
    void configure();

    Effect effect_;
    double sampleRate_ = 44100.0;
    int channels_ = 1;
    float gain_ = 1.0f;
    std::vector<Biquad> biquads_;
    std::vector<NoiseGate> gates_;
    std::vector<Compressor> compressors_;
    std::vector<Limiter> limiters_;
    std::vector<RingModulator> ringMods_;
    std::vector<EchoEffect> echoes_;
    std::vector<PitchShifter> pitchShifters_;
    Distortion distortion_;
};

// A track's effect stack at runtime: one EffectProcessor per effect, in
// order. Built on the UI thread by prepare(). Parameter and bypass changes
// reach the audio thread lock-free through publish(): per-effect atomics and
// a version counter the audio thread checks at the start of each block (no
// allocation, no lock, no waiting on either side). Adding, removing or
// reordering effects needs a new stack (see EffectRack).
class EffectStack {
public:
    EffectStack() = default;
    EffectStack(const EffectStack&) = delete;
    EffectStack& operator=(const EffectStack&) = delete;

    // UI thread, before the stack is visible to the audio thread.
    void prepare(const std::vector<Effect>& effects, double sampleRate, int channels);
    // UI thread: same effect types in the same order (so publish() applies).
    bool sameStructure(const std::vector<Effect>& effects) const;
    // UI thread, any time: hands new parameters and bypass flags to the
    // audio thread. `effects` must have the same structure.
    void publish(const std::vector<Effect>& effects);

    // Audio thread (or whoever renders). Picks up published changes; returns
    // true if any effect is active (not bypassed) afterwards.
    bool update();
    void process(float* interleaved, size_t frames);
    void reset();
    size_t size() const { return processors_.size(); }

private:
    struct SharedParams {
        std::atomic<bool> bypassed{false};
        std::atomic<double> params[kMaxEffectParams];
    };
    std::vector<EffectProcessor> processors_;
    std::unique_ptr<SharedParams[]> shared_;
    std::vector<EffectType> types_;
    std::atomic<uint64_t> version_{0};
    uint64_t applied_ = 0; // audio thread
    bool active_ = false;  // audio thread
};

} // namespace zrecord
