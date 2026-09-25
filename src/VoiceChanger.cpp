#include "VoiceChanger.h"

#include <algorithm>
#include <cmath>

#include "Fft.h"

namespace zrecord {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Smooth log-magnitude envelope by cepstral liftering. `mag` has half+1
// bins; `re`/`im` are N-long scratch.
void spectralEnvelope(const std::vector<float>& mag, size_t lifter, std::vector<float>& re, std::vector<float>& im,
                      std::vector<float>& env) {
    const size_t n = re.size();
    const size_t half = n / 2;
    for (size_t k = 0; k <= half; ++k) {
        re[k] = std::log(std::max(mag[k], 1e-9f));
        im[k] = 0.0f;
    }
    for (size_t k = half + 1; k < n; ++k) {
        re[k] = re[n - k];
        im[k] = 0.0f;
    }
    fftRadix2(re, im); // symmetric real in, so this is N x the real cepstrum
    const float scale = 1.0f / static_cast<float>(n);
    for (size_t q = 0; q < n; ++q) {
        const bool keep = q <= lifter || q >= n - lifter;
        re[q] = keep ? re[q] * scale : 0.0f;
        im[q] = 0.0f;
    }
    fftRadix2(re, im); // back to the (smoothed) log spectrum
    for (size_t k = 0; k <= half; ++k) {
        env[k] = std::max(std::exp(re[k]), 1e-9f);
    }
}

} // namespace

VoiceSettings voicePresetSettings(VoicePreset preset) {
    switch (preset) {
    case VoicePreset::Deeper: return {-4.0f, -2.0f, false};  // lower voice, slightly bigger "throat"
    case VoicePreset::Higher: return {4.0f, 2.0f, false};
    case VoicePreset::Robot: return {0.0f, 0.0f, true};
    case VoicePreset::Chipmunk: return {8.0f, 8.0f, false};  // formants ride along with the pitch
    case VoicePreset::Custom: break;
    }
    return {};
}

const char* voicePresetName(VoicePreset preset) {
    switch (preset) {
    case VoicePreset::Deeper: return "Deeper";
    case VoicePreset::Higher: return "Higher";
    case VoicePreset::Robot: return "Robot";
    case VoicePreset::Chipmunk: return "Chipmunk";
    case VoicePreset::Custom: return "Custom";
    }
    return "Custom";
}

size_t voiceFrameSize(double sampleRate) {
    size_t n = 2048;
    while (static_cast<double>(n) < sampleRate * 0.035) { // keep ~40 ms frames at high rates
        n *= 2;
    }
    return n;
}

std::vector<float> processVoice(const std::vector<float>& input, double sampleRate, const VoiceSettings& settings) {
    if (input.empty() || settings.isIdentity() || sampleRate <= 0.0) {
        return input;
    }
    const size_t n = voiceFrameSize(sampleRate);
    const size_t hop = n / 4;
    const size_t half = n / 2;
    const double alpha = std::pow(2.0, settings.pitchSemitones / 12.0);
    const double beta = std::pow(2.0, settings.formantSemitones / 12.0);
    const double expect = kTwoPi * static_cast<double>(hop) / static_cast<double>(n); // phase advance per bin per hop
    const size_t lifter = std::clamp<size_t>(static_cast<size_t>(std::lround(sampleRate * 0.001)), 8, half / 4);

    const size_t len = input.size();
    std::vector<float> padded(len + 2 * n, 0.0f);
    std::copy(input.begin(), input.end(), padded.begin() + static_cast<long>(n));
    std::vector<float> out(padded.size(), 0.0f);

    std::vector<float> window(n);
    for (size_t i = 0; i < n; ++i) {
        window[i] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(n)));
    }
    // Periodic Hann at 4x overlap, applied on analysis and synthesis:
    // the squared windows sum to 1.5.
    const float olaScale = 1.0f / (1.5f * static_cast<float>(n));

    std::vector<float> re(n), im(n), cre(n), cim(n);
    std::vector<float> mag(half + 1), env(half + 1), outMag(half + 1), best(half + 1);
    std::vector<double> trueBin(half + 1), outBin(half + 1), prevPhase(half + 1, 0.0), sumPhase(half + 1, 0.0);

    for (size_t pos = 0; pos + n <= padded.size(); pos += hop) {
        for (size_t i = 0; i < n; ++i) {
            re[i] = padded[pos + i] * window[i];
            im[i] = 0.0f;
        }
        fftRadix2(re, im);

        // Analysis: magnitude and each bin's true frequency (in bins).
        for (size_t k = 0; k <= half; ++k) {
            mag[k] = std::hypot(re[k], im[k]);
            const double phase = std::atan2(static_cast<double>(im[k]), static_cast<double>(re[k]));
            double delta = phase - prevPhase[k] - static_cast<double>(k) * expect;
            prevPhase[k] = phase;
            delta -= kTwoPi * std::round(delta / kTwoPi);
            trueBin[k] = static_cast<double>(k) + delta / expect;
        }
        spectralEnvelope(mag, lifter, cre, cim, env);

        // Move the excitation to its shifted frequencies.
        std::fill(outMag.begin(), outMag.end(), 0.0f);
        std::fill(best.begin(), best.end(), 0.0f);
        for (size_t k = 0; k <= half; ++k) {
            outBin[k] = static_cast<double>(k);
        }
        for (size_t k = 0; k <= half; ++k) {
            const size_t target = static_cast<size_t>(std::lround(static_cast<double>(k) * alpha));
            if (target > half) {
                break;
            }
            const float excitation = mag[k] / env[k];
            outMag[target] += excitation;
            if (excitation > best[target]) { // the loudest contributor sets the frequency
                best[target] = excitation;
                outBin[target] = trueBin[k] * alpha;
            }
        }

        // Re-apply the envelope, moved by the formant factor, and synthesize.
        for (size_t k = 0; k <= half; ++k) {
            const double src = static_cast<double>(k) / beta;
            float e;
            if (src >= static_cast<double>(half)) {
                e = env[half];
            } else {
                const size_t i0 = static_cast<size_t>(src);
                const float frac = static_cast<float>(src - static_cast<double>(i0));
                e = env[i0] * (1.0f - frac) + env[i0 + 1] * frac;
            }
            const float m = outMag[k] * e;
            sumPhase[k] = std::fmod(sumPhase[k] + outBin[k] * expect, kTwoPi);
            const double phase = settings.robot ? 0.0 : sumPhase[k];
            re[k] = static_cast<float>(m * std::cos(phase));
            im[k] = static_cast<float>(m * std::sin(phase));
        }
        im[0] = 0.0f;
        im[half] = 0.0f;
        for (size_t k = half + 1; k < n; ++k) {
            re[k] = re[n - k];
            im[k] = -im[n - k];
        }
        // Inverse transform via the conjugate trick.
        for (size_t k = 0; k < n; ++k) {
            im[k] = -im[k];
        }
        fftRadix2(re, im);
        for (size_t i = 0; i < n; ++i) {
            out[pos + i] += re[i] * window[i] * olaScale;
        }
    }

    std::vector<float> result(out.begin() + static_cast<long>(n), out.begin() + static_cast<long>(n + len));

    // Moving and summing bins changes the level a little; keep the overall
    // loudness where it was (bounded, so near-silence isn't blown up).
    double inEnergy = 0.0;
    double outEnergy = 0.0;
    for (size_t i = 0; i < len; ++i) {
        inEnergy += static_cast<double>(input[i]) * input[i];
        outEnergy += static_cast<double>(result[i]) * result[i];
    }
    if (outEnergy > 0.0 && inEnergy > 0.0) {
        const float gain = static_cast<float>(std::clamp(std::sqrt(inEnergy / outEnergy), 0.25, 4.0));
        for (float& s : result) {
            s *= gain;
        }
    }
    return result;
}

void processVoiceInterleaved(std::vector<float>& interleaved, int channels, double sampleRate,
                             const VoiceSettings& settings) {
    if (channels <= 0 || settings.isIdentity()) {
        return;
    }
    const size_t ch = static_cast<size_t>(channels);
    const size_t frames = interleaved.size() / ch;
    std::vector<float> mono(frames);
    for (size_t c = 0; c < ch; ++c) {
        for (size_t f = 0; f < frames; ++f) {
            mono[f] = interleaved[f * ch + c];
        }
        const std::vector<float> processed = processVoice(mono, sampleRate, settings);
        for (size_t f = 0; f < frames; ++f) {
            interleaved[f * ch + c] = processed[f];
        }
    }
}

void applyVoiceChange(Project& project, const std::vector<GainTarget>& targets, const VoiceSettings& settings) {
    if (settings.isIdentity()) {
        return;
    }
    for (const GainTarget& target : targets) {
        if (target.trackIndex < 0 || target.trackIndex >= static_cast<int>(project.tracks.size())) {
            continue;
        }
        for (Clip& clip : project.tracks[static_cast<size_t>(target.trackIndex)].clips) {
            const int64_t start = std::max(clip.startFrame, target.startFrame);
            const int64_t end = std::min(clip.endFrame(), target.endFrame);
            if (start >= end || clip.channels <= 0) {
                continue;
            }
            const size_t first = static_cast<size_t>(start - clip.startFrame) * static_cast<size_t>(clip.channels);
            const size_t count = static_cast<size_t>(end - start) * static_cast<size_t>(clip.channels);
            std::vector<float> block(count);
            clip.samples.copyTo(first, count, block.data());
            processVoiceInterleaved(block, clip.channels, project.sampleRate, settings);
            clip.samples.write(first, block.data(), count);
            clip.peaks.build(clip.samples, clip.channels);
        }
    }
}

} // namespace zrecord
