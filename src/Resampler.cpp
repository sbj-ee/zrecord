#include "Resampler.h"

#include <samplerate.h>

#include <cmath>

namespace zrecord {

bool Resampler::convert(const std::vector<float>& in, int channels, double fromRate, double toRate,
                        std::vector<float>& out, std::string& errorMessage) {
    if (channels <= 0 || fromRate <= 0.0 || toRate <= 0.0) {
        errorMessage = "Invalid sample rate or channel count";
        return false;
    }
    if (fromRate == toRate || in.empty()) {
        out = in;
        return true;
    }
    const double ratio = toRate / fromRate;
    const long inFrames = static_cast<long>(in.size() / static_cast<size_t>(channels));
    const long outFrames = static_cast<long>(std::ceil(static_cast<double>(inFrames) * ratio)) + 1;
    out.assign(static_cast<size_t>(outFrames) * static_cast<size_t>(channels), 0.0f);

    SRC_DATA data{};
    data.data_in = in.data();
    data.input_frames = inFrames;
    data.data_out = out.data();
    data.output_frames = outFrames;
    data.src_ratio = ratio;
    int err = src_simple(&data, SRC_SINC_MEDIUM_QUALITY, channels);
    if (err != 0) {
        errorMessage = src_strerror(err);
        out.clear();
        return false;
    }
    out.resize(static_cast<size_t>(data.output_frames_gen) * static_cast<size_t>(channels));
    return true;
}

} // namespace zrecord
