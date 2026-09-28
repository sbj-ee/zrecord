#include "Capture.h"

#include <algorithm>
#include <cmath>

namespace zrecord {

double inputGainToLinear(double db) {
    return std::pow(10.0, std::clamp(db, kInputGainMinDb, kInputGainMaxDb) / 20.0);
}

void ClipDetector::reset(int channels) {
    channels_ = std::max(1, channels);
    std::fill(std::begin(runs_), std::end(runs_), 0);
    events_ = 0;
    clippedSamples_ = 0;
}

template <typename T>
int64_t ClipDetector::feedImpl(const T* interleaved, size_t frames) {
    const int tracked = std::min(channels_, kMaxChannels);
    int64_t newEvents = 0;
    for (size_t f = 0; f < frames; ++f) {
        const T* frame = interleaved + f * static_cast<size_t>(channels_);
        for (int c = 0; c < tracked; ++c) {
            if (!isFullScale(frame[c])) {
                runs_[c] = 0;
                continue;
            }
            // Saturate the run length: only "reached the threshold" matters.
            const int run = runs_[c] = std::min(runs_[c] + 1, kClipRunLength + 1);
            if (run == kClipRunLength) {
                ++newEvents;
                clippedSamples_ += kClipRunLength; // the run so far, now known to be a clip
            } else if (run > kClipRunLength) {
                ++clippedSamples_;
            }
        }
    }
    events_ += newEvents;
    return newEvents;
}

int64_t ClipDetector::feed(const float* interleaved, size_t frames) { return feedImpl(interleaved, frames); }
int64_t ClipDetector::feed(const int16_t* interleaved, size_t frames) { return feedImpl(interleaved, frames); }

CapturePeaks processCaptureBlock(std::vector<float>& block, size_t frameCount, int channels, float inputGain,
                                 FilterChain& chain, ClipDetector* inputClip) {
    CapturePeaks peaks;
    const size_t count = std::min(block.size(), frameCount * static_cast<size_t>(std::max(1, channels)));
    if (inputClip != nullptr) {
        peaks.inputClipEvents = inputClip->feed(block.data(), count / static_cast<size_t>(std::max(1, channels)));
    }
    for (size_t i = 0; i < count; ++i) {
        peaks.input = std::max(peaks.input, std::fabs(block[i]));
    }
    if (inputGain != 1.0f) {
        for (size_t i = 0; i < count; ++i) {
            block[i] *= inputGain;
        }
    }
    chain.process(block, frameCount);
    for (size_t i = 0; i < count; ++i) {
        peaks.recorded = std::max(peaks.recorded, std::fabs(block[i]));
    }
    return peaks;
}

} // namespace zrecord
