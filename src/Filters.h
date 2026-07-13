#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace zrecord {

// Single-channel biquad filter using the RBJ Audio EQ Cookbook formulas.
class Biquad {
public:
    enum class Type { LowPass, HighPass };

    void configure(Type type, double sampleRate, double cutoffHz, double q = 0.70710678);
    float process(float x);
    void reset();

private:
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0;
    double a1_ = 0.0, a2_ = 0.0;
    double z1_ = 0.0, z2_ = 0.0;
};

// Per-channel gate that attenuates signal below a threshold, with
// exponential attack/release smoothing to avoid clicks.
class NoiseGate {
public:
    void configure(double sampleRate, double thresholdDb, double attackMs, double releaseMs);
    float process(float x);
    void reset();

private:
    double sampleRate_ = 44100.0;
    double thresholdLinear_ = 0.0;
    double attackCoeff_ = 0.0;
    double releaseCoeff_ = 0.0;
    double envelope_ = 0.0;
    double gain_ = 1.0;
};

// Classic "robot voice" effect: multiplies the signal by a low-frequency
// carrier tone (ring modulation).
class RingModulator {
public:
    void configure(double sampleRate, double carrierHz);
    float process(float x);
    void reset();

private:
    double sampleRate_ = 44100.0;
    double carrierHz_ = 30.0;
    double phase_ = 0.0;
};

// Feedback delay line producing repeating echoes.
class EchoEffect {
public:
    void configure(double sampleRate, double delayMs, double feedback, double mix);
    float process(float x);
    void reset();

private:
    std::vector<float> buffer_;
    size_t writePos_ = 0;
    double feedback_ = 0.4;
    double mix_ = 0.5;
};

// Real-time pitch shifter using a two-tap, Hann-crossfaded delay line (no
// FFT). Works at a fixed output rate of one sample per input sample, so it
// fits directly into a per-sample filter chain without altering block sizes.
class PitchShifter {
public:
    void configure(double pitchRatio);
    float process(float x);
    void reset();

private:
    static constexpr int kBufferSize = 4096;
    static constexpr int kWindowSize = 1024;

    float readInterpolated(double delayBack) const;
    static float hannWeight(double posFraction);

    std::array<float, kBufferSize> buffer_{};
    int writePos_ = 0;
    double grainPos_ = 0.0;
    double pitchRatio_ = 1.0;
};

// Soft-clipping waveshaper for a gritty/distorted voice.
class Distortion {
public:
    void configure(double driveAmount);
    float process(float x) const;

private:
    double driveAmount_ = 6.0;
};

enum class VoiceEffect { None, Robot, Echo, DeepVoice, Chipmunk, Distortion };

struct FilterSettings {
    bool gainEnabled = false;
    double gainDb = 0.0;

    bool highPassEnabled = false;
    double highPassHz = 100.0;

    bool lowPassEnabled = false;
    double lowPassHz = 8000.0;

    bool noiseGateEnabled = false;
    double noiseGateThresholdDb = -40.0;
    double noiseGateAttackMs = 5.0;
    double noiseGateReleaseMs = 80.0;

    VoiceEffect voiceEffect = VoiceEffect::None;
};

// Applies the configured chain (gain -> high-pass -> low-pass -> noise gate)
// to interleaved multi-channel float buffers. Each channel gets independent
// filter state.
class FilterChain {
public:
    void prepare(double sampleRate, int channels);
    void setSettings(const FilterSettings& settings);
    FilterSettings settings() const;

    // Processes interleaved samples in place. frameCount * channels_ must
    // equal interleaved.size().
    void process(std::vector<float>& interleaved, size_t frameCount);

private:
    double sampleRate_ = 44100.0;
    int channels_ = 1;
    FilterSettings settings_;

    std::vector<Biquad> highPass_;
    std::vector<Biquad> lowPass_;
    std::vector<NoiseGate> noiseGate_;
    std::vector<RingModulator> ringMod_;
    std::vector<EchoEffect> echo_;
    std::vector<PitchShifter> pitchShifter_;
    std::vector<Distortion> distortion_;

    void reconfigureFilters();
};

} // namespace zrecord
