#include "AudioFileReader.h"

#include <sndfile.h>
#include <cstring>

namespace zrecord {

bool AudioFileReader::read(const std::string& path,
                            std::vector<float>& outSamples,
                            int& outSampleRate,
                            int& outChannels,
                            std::string& errorMessage) {
    SF_INFO info;
    std::memset(&info, 0, sizeof(info));

    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
    if (!file) {
        errorMessage = sf_strerror(nullptr);
        return false;
    }

    outSampleRate = info.samplerate;
    outChannels = info.channels;

    outSamples.assign(static_cast<size_t>(info.frames) * static_cast<size_t>(info.channels), 0.0f);
    sf_count_t read = sf_readf_float(file, outSamples.data(), info.frames);
    bool ok = read == info.frames;
    if (!ok) {
        errorMessage = sf_strerror(file);
    }

    sf_close(file);
    return ok;
}

} // namespace zrecord
