#include "Capture.h"

#include <algorithm>
#include <cmath>

namespace zrecord {

double inputGainToLinear(double db) {
    return std::pow(10.0, std::clamp(db, kInputGainMinDb, kInputGainMaxDb) / 20.0);
}

CapturePeaks processCaptureBlock(std::vector<float>& block, size_t frameCount, int channels, float inputGain,
                                 FilterChain& chain) {
    CapturePeaks peaks;
    const size_t count = std::min(block.size(), frameCount * static_cast<size_t>(std::max(1, channels)));
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
