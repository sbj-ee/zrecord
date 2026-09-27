#pragma once

#include <cstddef>
#include <vector>

#include "Filters.h"

namespace zrecord {

// A raw input sample at or above this magnitude means the signal reached the
// converter's (or the sound server's) full scale before zrecord saw it: the
// clipping is already in the audio and no gain setting here can undo it.
// 0.999 is just under int16 full scale (32767 / 32768 = 0.99997).
constexpr float kInputClipLevel = 0.999f;

// Input gain range, in dB, for the UI control and the engine.
constexpr double kInputGainMinDb = -24.0;
constexpr double kInputGainMaxDb = 24.0;

double inputGainToLinear(double db); // clamped to the range above

struct CapturePeaks {
    float input = 0.0f;    // largest |sample| as it arrived, before any gain
    float recorded = 0.0f; // largest |sample| of what goes into the take
};

// The per-block work of the capture callback: applies the input gain, then
// the live filter chain, to the first frameCount frames of `block` in place.
// Returns the peaks before and after, so the level meter can show what is
// actually recorded (it used to show the raw input, missing any gain or
// effect the chain added). Real-time safe: no allocation, no locks.
CapturePeaks processCaptureBlock(std::vector<float>& block, size_t frameCount, int channels, float inputGain,
                                 FilterChain& chain);

} // namespace zrecord
