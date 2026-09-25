#pragma once

#include <string>
#include <vector>

namespace zrecord {

// Sample-rate conversion for imported audio (libsamplerate, sinc medium
// quality: transparent for speech/music editing and fast enough to run on
// import without a progress dialog).
class Resampler {
public:
    // Converts interleaved `in` (`channels` wide) from `fromRate` to
    // `toRate`. Returns false with `errorMessage` set on failure.
    static bool convert(const std::vector<float>& in, int channels, double fromRate, double toRate,
                        std::vector<float>& out, std::string& errorMessage);
};

} // namespace zrecord
