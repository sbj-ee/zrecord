#include "Filters.h"

#include <algorithm>
#include <cmath>

namespace zrecord {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

void Biquad::configure(Type type, double sampleRate, double cutoffHz, double q) {
    double nyquist = sampleRate * 0.5;
    double f0 = std::clamp(cutoffHz, 1.0, nyquist - 1.0);
    double w0 = 2.0 * kPi * f0 / sampleRate;
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double alpha = sinw0 / (2.0 * q);

    double b0, b1, b2;
    if (type == Type::LowPass) {
        b0 = (1.0 - cosw0) / 2.0;
        b1 = 1.0 - cosw0;
        b2 = (1.0 - cosw0) / 2.0;
    } else {
        b0 = (1.0 + cosw0) / 2.0;
        b1 = -(1.0 + cosw0);
        b2 = (1.0 + cosw0) / 2.0;
    }
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cosw0;
    double a2 = 1.0 - alpha;

    b0_ = b0 / a0;
    b1_ = b1 / a0;
    b2_ = b2 / a0;
    a1_ = a1 / a0;
    a2_ = a2 / a0;
    reset();
}

float Biquad::process(float x) {
    double y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return static_cast<float>(y);
}

void Biquad::reset() {
    z1_ = 0.0;
    z2_ = 0.0;
}

void NoiseGate::configure(double sampleRate, double thresholdDb, double attackMs, double releaseMs) {
    sampleRate_ = sampleRate;
    thresholdLinear_ = std::pow(10.0, thresholdDb / 20.0);

    auto timeToCoeff = [sampleRate](double ms) {
        double seconds = std::max(ms, 0.001) / 1000.0;
        return std::exp(-1.0 / (sampleRate * seconds));
    };
    attackCoeff_ = timeToCoeff(attackMs);
    releaseCoeff_ = timeToCoeff(releaseMs);
    reset();
}

float NoiseGate::process(float x) {
    double rectified = std::fabs(static_cast<double>(x));
    if (rectified > envelope_) {
        envelope_ = rectified;
    } else {
        envelope_ = rectified + (envelope_ - rectified) * releaseCoeff_;
    }

    double target = envelope_ >= thresholdLinear_ ? 1.0 : 0.0;
    double coeff = target > gain_ ? attackCoeff_ : releaseCoeff_;
    gain_ = target + (gain_ - target) * coeff;

    return static_cast<float>(x * gain_);
}

void NoiseGate::reset() {
    envelope_ = 0.0;
    gain_ = 0.0;
}

void Compressor::configure(double sampleRate, double thresholdDb, double ratio, double attackMs, double releaseMs) {
    thresholdDb_ = thresholdDb;
    ratio_ = std::max(1.0, ratio);

    auto timeToCoeff = [sampleRate](double ms) {
        double seconds = std::max(ms, 0.001) / 1000.0;
        return std::exp(-1.0 / (sampleRate * seconds));
    };
    attackCoeff_ = timeToCoeff(attackMs);
    releaseCoeff_ = timeToCoeff(releaseMs);
    reset();
}

float Compressor::process(float x) {
    double rectified = std::fabs(static_cast<double>(x));
    if (rectified > envelope_) {
        envelope_ = rectified + (envelope_ - rectified) * attackCoeff_;
    } else {
        envelope_ = rectified + (envelope_ - rectified) * releaseCoeff_;
    }

    double levelDb = 20.0 * std::log10(std::max(envelope_, 1e-6));
    double gainReductionDb = 0.0;
    if (levelDb > thresholdDb_) {
        double overDb = levelDb - thresholdDb_;
        gainReductionDb = overDb - overDb / ratio_;
    }

    double gainLinear = std::pow(10.0, -gainReductionDb / 20.0);
    return static_cast<float>(x * gainLinear);
}

void Compressor::reset() {
    envelope_ = 0.0;
}

void Limiter::configure(double sampleRate, double ceilingDb, double releaseMs) {
    ceilingLinear_ = std::pow(10.0, ceilingDb / 20.0);
    double seconds = std::max(releaseMs, 1.0) / 1000.0;
    releaseCoeff_ = std::exp(-1.0 / (sampleRate * seconds));
    reset();
}

float Limiter::process(float x) {
    double absX = std::fabs(static_cast<double>(x));
    double desiredGain = absX > ceilingLinear_ ? (ceilingLinear_ / absX) : 1.0;

    if (desiredGain < gain_) {
        gain_ = desiredGain;
    } else {
        gain_ = desiredGain + (gain_ - desiredGain) * releaseCoeff_;
    }

    return static_cast<float>(x * gain_);
}

void Limiter::reset() {
    gain_ = 1.0;
}

void RingModulator::configure(double sampleRate, double carrierHz) {
    sampleRate_ = sampleRate;
    carrierHz_ = carrierHz;
    reset();
}

float RingModulator::process(float x) {
    float y = static_cast<float>(x * std::cos(2.0 * kPi * phase_));
    phase_ += carrierHz_ / sampleRate_;
    if (phase_ >= 1.0) {
        phase_ -= 1.0;
    }
    return y;
}

void RingModulator::reset() {
    phase_ = 0.0;
}

void EchoEffect::configure(double sampleRate, double delayMs, double feedback, double mix) {
    size_t delaySamples = static_cast<size_t>(std::max(1.0, sampleRate * delayMs / 1000.0));
    buffer_.assign(delaySamples, 0.0f);
    writePos_ = 0;
    feedback_ = feedback;
    mix_ = mix;
}

float EchoEffect::process(float x) {
    if (buffer_.empty()) {
        return x;
    }
    float delayed = buffer_[writePos_];
    buffer_[writePos_] = static_cast<float>(x + delayed * feedback_);
    writePos_ = (writePos_ + 1) % buffer_.size();
    return static_cast<float>(x + delayed * mix_);
}

void EchoEffect::reset() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    writePos_ = 0;
}

void PitchShifter::configure(double pitchRatio) {
    pitchRatio_ = pitchRatio;
}

void PitchShifter::reset() {
    buffer_.fill(0.0f);
    writePos_ = 0;
    grainPos_ = 0.0;
}

float PitchShifter::readInterpolated(double delayBack) const {
    double readPosF = static_cast<double>(writePos_) - delayBack;
    while (readPosF < 0.0) {
        readPosF += kBufferSize;
    }
    int idx0 = static_cast<int>(readPosF) % kBufferSize;
    int idx1 = (idx0 + 1) % kBufferSize;
    double frac = readPosF - std::floor(readPosF);
    return static_cast<float>(buffer_[idx0] * (1.0 - frac) + buffer_[idx1] * frac);
}

float PitchShifter::hannWeight(double posFraction) {
    return static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * posFraction));
}

float PitchShifter::process(float x) {
    buffer_[writePos_] = x;

    double grainPos2 = grainPos_ + kWindowSize / 2.0;
    if (grainPos2 >= kWindowSize) {
        grainPos2 -= kWindowSize;
    }

    double delayBack1 = kWindowSize - grainPos_;
    double delayBack2 = kWindowSize - grainPos2;

    float sample1 = readInterpolated(delayBack1);
    float sample2 = readInterpolated(delayBack2);

    float w1 = hannWeight(grainPos_ / kWindowSize);
    float w2 = hannWeight(grainPos2 / kWindowSize);

    float out = sample1 * w1 + sample2 * w2;

    grainPos_ += pitchRatio_;
    if (grainPos_ >= kWindowSize) {
        grainPos_ -= kWindowSize;
    }

    writePos_ = (writePos_ + 1) % kBufferSize;

    return out;
}

void Distortion::configure(double driveAmount) {
    driveAmount_ = driveAmount;
}

float Distortion::process(float x) const {
    double normalizer = std::tanh(driveAmount_);
    return static_cast<float>(std::tanh(driveAmount_ * x) / normalizer);
}

void FilterChain::prepare(double sampleRate, int channels) {
    sampleRate_ = sampleRate;
    channels_ = std::max(1, channels);
    reconfigureFilters();
}

void FilterChain::setSettings(const FilterSettings& settings) {
    settings_ = settings;
    reconfigureFilters();
}

FilterSettings FilterChain::settings() const {
    return settings_;
}

void FilterChain::reconfigureFilters() {
    limiter_.assign(static_cast<size_t>(channels_), Limiter{});
    highPass_.assign(static_cast<size_t>(channels_), Biquad{});
    lowPass_.assign(static_cast<size_t>(channels_), Biquad{});
    noiseGate_.assign(static_cast<size_t>(channels_), NoiseGate{});
    compressor_.assign(static_cast<size_t>(channels_), Compressor{});
    ringMod_.assign(static_cast<size_t>(channels_), RingModulator{});
    echo_.assign(static_cast<size_t>(channels_), EchoEffect{});
    pitchShifter_.assign(static_cast<size_t>(channels_), PitchShifter{});
    distortion_.assign(static_cast<size_t>(channels_), Distortion{});

    double pitchRatio = 1.0;
    if (settings_.voiceEffect == VoiceEffect::DeepVoice) {
        pitchRatio = 0.75;
    } else if (settings_.voiceEffect == VoiceEffect::Chipmunk) {
        pitchRatio = 1.5;
    }

    for (int c = 0; c < channels_; ++c) {
        limiter_[c].configure(sampleRate_, settings_.limiterCeilingDb, 50.0);
        highPass_[c].configure(Biquad::Type::HighPass, sampleRate_, settings_.highPassHz);
        lowPass_[c].configure(Biquad::Type::LowPass, sampleRate_, settings_.lowPassHz);
        noiseGate_[c].configure(sampleRate_, settings_.noiseGateThresholdDb,
                                 settings_.noiseGateAttackMs, settings_.noiseGateReleaseMs);
        compressor_[c].configure(sampleRate_, settings_.compressorThresholdDb,
                                  settings_.compressorRatio, 10.0, 150.0);
        ringMod_[c].configure(sampleRate_, 30.0);
        echo_[c].configure(sampleRate_, 280.0, 0.35, 0.5);
        pitchShifter_[c].configure(pitchRatio);
        distortion_[c].configure(6.0);
    }
}

void FilterChain::process(std::vector<float>& interleaved, size_t frameCount) {
    double gainLinear = settings_.gainEnabled ? std::pow(10.0, settings_.gainDb / 20.0) : 1.0;

    for (size_t i = 0; i < frameCount; ++i) {
        for (int c = 0; c < channels_; ++c) {
            size_t idx = i * static_cast<size_t>(channels_) + static_cast<size_t>(c);
            float sample = interleaved[idx];

            if (settings_.limiterEnabled) {
                sample = limiter_[c].process(sample);
            }
            if (settings_.gainEnabled) {
                sample = static_cast<float>(sample * gainLinear);
            }
            if (settings_.highPassEnabled) {
                sample = highPass_[c].process(sample);
            }
            if (settings_.lowPassEnabled) {
                sample = lowPass_[c].process(sample);
            }
            if (settings_.noiseGateEnabled) {
                sample = noiseGate_[c].process(sample);
            }
            if (settings_.compressorEnabled) {
                sample = compressor_[c].process(sample);
            }

            switch (settings_.voiceEffect) {
                case VoiceEffect::Robot:
                    sample = ringMod_[c].process(sample);
                    break;
                case VoiceEffect::Echo:
                    sample = echo_[c].process(sample);
                    break;
                case VoiceEffect::DeepVoice:
                case VoiceEffect::Chipmunk:
                    sample = pitchShifter_[c].process(sample);
                    break;
                case VoiceEffect::Distortion:
                    sample = distortion_[c].process(sample);
                    break;
                case VoiceEffect::None:
                    break;
            }

            interleaved[idx] = sample;
        }
    }
}

void applyLinearFade(std::vector<float>& interleaved, int channels, FadeShape shape) {
    if (channels <= 0 || interleaved.empty()) {
        return;
    }
    size_t frameCount = interleaved.size() / static_cast<size_t>(channels);
    if (frameCount == 0) {
        return;
    }
    // Divide by frameCount-1 so the ramp actually reaches unity on the last
    // frame rather than stopping just short of it.
    double lastFrame = frameCount > 1 ? static_cast<double>(frameCount - 1) : 1.0;

    for (size_t frame = 0; frame < frameCount; ++frame) {
        double position = frameCount > 1 ? static_cast<double>(frame) / lastFrame : 1.0;
        float gain = static_cast<float>(shape == FadeShape::In ? position : 1.0 - position);
        for (int c = 0; c < channels; ++c) {
            interleaved[frame * static_cast<size_t>(channels) + static_cast<size_t>(c)] *= gain;
        }
    }
}

void mixEqualPowerCrossfade(std::vector<float>& outgoing, const std::vector<float>& incoming, int channels) {
    if (channels <= 0 || outgoing.empty() || outgoing.size() != incoming.size()) {
        return;
    }
    size_t frameCount = outgoing.size() / static_cast<size_t>(channels);
    if (frameCount == 0) {
        return;
    }
    double lastFrame = frameCount > 1 ? static_cast<double>(frameCount - 1) : 1.0;

    for (size_t frame = 0; frame < frameCount; ++frame) {
        double position = frameCount > 1 ? static_cast<double>(frame) / lastFrame : 1.0;
        // cos/sin keep gainOut^2 + gainIn^2 == 1 across the whole ramp.
        float gainOut = static_cast<float>(std::cos(position * M_PI / 2.0));
        float gainIn = static_cast<float>(std::sin(position * M_PI / 2.0));
        for (int c = 0; c < channels; ++c) {
            size_t i = frame * static_cast<size_t>(channels) + static_cast<size_t>(c);
            outgoing[i] = outgoing[i] * gainOut + incoming[i] * gainIn;
        }
    }
}

} // namespace zrecord
