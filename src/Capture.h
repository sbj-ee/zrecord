#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>


namespace zrecord {

// Clip detection (the design follows Audacity/Tenacity's meter: ideas only,
// no code). One sample at full scale is just a loud peak; the signal has
// clipped when several consecutive samples sit at full scale, i.e. the
// waveform has been flattened against the rail.
//
// "Full scale" for 16-bit sources means the integer extremes, 32767 and
// -32768. zrecord captures as float32 and PortAudio/ALSA convert int16 by
// x / 32768 exactly, so those extremes arrive as 32767/32768 and -1.0; the
// float test below is exactly that image (and 32766 or -32767 are not full
// scale). For 24-bit/32-bit/float sources the same thresholds mean "within
// one 16-bit step of full scale, or beyond it": float input that overshoots
// +/-1.0 counts too, because it will be clipped on playback and export.
constexpr float kFullScalePositive = 32767.0f / 32768.0f;
constexpr float kFullScaleNegative = -1.0f;
constexpr int16_t kInt16FullScalePositive = 32767;
constexpr int16_t kInt16FullScaleNegative = -32768;

// How many consecutive full-scale samples (in one channel) make a clip.
constexpr int kClipRunLength = 3;

inline bool isFullScale(float sample) {
    return sample >= kFullScalePositive || sample <= kFullScaleNegative;
}
inline bool isFullScale(int16_t sample) {
    return sample == kInt16FullScalePositive || sample == kInt16FullScaleNegative;
}

// Counts clip events in an interleaved stream fed block by block. Each
// channel keeps its run of full-scale samples across calls, so a clip that
// straddles two callback buffers is still one clip. An event is a run
// reaching kClipRunLength; clippedSamples counts every sample of such runs.
// Real-time safe: no allocation after reset().
class ClipDetector {
public:
    static constexpr int kMaxChannels = 32; // more are ignored

    void reset(int channels);
    // Returns the number of new clip events in this block.
    int64_t feed(const float* interleaved, size_t frames);
    int64_t feed(const int16_t* interleaved, size_t frames);

    int64_t events() const { return events_; }
    int64_t clippedSamples() const { return clippedSamples_; }

private:
    template <typename T>
    int64_t feedImpl(const T* interleaved, size_t frames);
    int channels_ = 1;
    int runs_[kMaxChannels] = {};
    int64_t events_ = 0;
    int64_t clippedSamples_ = 0;
};

// Input gain range, in dB, for the UI control and the engine.
constexpr double kInputGainMinDb = -24.0;
constexpr double kInputGainMaxDb = 24.0;

double inputGainToLinear(double db); // clamped to the range above

struct CapturePeaks {
    float input = 0.0f;    // largest |sample| as it arrived, before any gain
    float recorded = 0.0f; // largest |sample| of what goes into the take
    int64_t inputClipEvents = 0; // new clips in the raw input (needs a detector)
};

// The per-block work of the capture callback: applies the input gain -- and
// nothing else -- to the first frameCount frames of `block` in place. The
// take is the raw input; effects are a track's playback stack, applied when
// it's heard or exported, never recorded. Returns the peaks before and after
// the gain, so the level meter shows what is actually recorded. With a
// detector, the raw input (before any gain) is also checked for clipping.
// Real-time safe: no allocation, no locks.
CapturePeaks processCaptureBlock(std::vector<float>& block, size_t frameCount, int channels, float inputGain,
                                 ClipDetector* inputClip = nullptr);

} // namespace zrecord
