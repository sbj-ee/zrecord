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
    double gain_ = 0.0; // starts closed, as reset() leaves it
};

// Downward compressor: reduces gain above a threshold by a fixed ratio,
// with an attack/release-smoothed envelope follower, to even out overall
// dynamics (distinct from the Limiter, which only catches peaks).
class Compressor {
public:
    void configure(double sampleRate, double thresholdDb, double ratio, double attackMs, double releaseMs);
    float process(float x);
    void reset();

private:
    double thresholdDb_ = -20.0;
    double ratio_ = 3.0;
    double attackCoeff_ = 0.0;
    double releaseCoeff_ = 0.0;
    double envelope_ = 0.0;
};

// Peak limiter with instant (sample-accurate) attack and exponential
// release, to protect downstream filters and the recording from clipping
// on hot input.
class Limiter {
public:
    void configure(double sampleRate, double ceilingDb, double releaseMs);
    float process(float x);
    void reset();

private:
    double ceilingLinear_ = 1.0;
    double releaseCoeff_ = 0.0;
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
    // Allocates the delay line for delays up to `maxDelayMs`, so later
    // configure() calls within that never allocate (real-time safe).
    void reserve(double sampleRate, double maxDelayMs);
    // Sets the delay, feedback and mix. Allocates only if the delay is longer
    // than any reserved so far; otherwise keeps the line's contents.
    void configure(double sampleRate, double delayMs, double feedback, double mix);
    float process(float x);
    void reset();

private:
    std::vector<float> buffer_; // capacity; the first length_ samples are the line
    size_t length_ = 0;
    size_t writePos_ = 0;
    double feedback_ = 0.4;
    double mix_ = 0.5;
};

// Real-time pitch shifter using a two-tap, Hann-crossfaded delay line (no
// FFT). Works at a fixed output rate of one sample per input sample, so it
// fits directly into a per-sample effect stack without altering block sizes.
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
    double normalizer_ = 0.99998771; // tanh(6)
};

enum class FadeShape { In, Out };

// Applies a linear gain ramp across the whole of `interleaved` in place
// (silence -> unity for In, unity -> silence for Out). Unlike the classes
// above this is a one-shot buffer operation, not a streaming filter: the ramp
// is positioned relative to the buffer's own length, so the caller passes
// exactly the span it wants faded.
void applyLinearFade(std::vector<float>& interleaved, int channels, FadeShape shape);

// Crossfades `incoming` over `outgoing` in place; both must hold the same
// number of frames. Uses equal-power (cos/sin) ramps rather than linear ones:
// two uncorrelated signals summed with linear ramps lose about 3 dB in the
// middle of the transition, which is audible as a dip.
void mixEqualPowerCrossfade(std::vector<float>& outgoing, const std::vector<float>& incoming, int channels);

} // namespace zrecord
