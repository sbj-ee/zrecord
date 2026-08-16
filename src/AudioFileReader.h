#pragma once

#include <string>
#include <vector>

namespace zrecord {

// Reads an audio file (any format libsndfile understands: WAV, FLAC, OGG,
// AIFF, ...) into an interleaved float buffer, for File > Import Audio and
// for reloading a saved project's per-clip files.
class AudioFileReader {
public:
    // Returns true on success; on failure errorMessage is populated. On
    // success, `outSampleRate`/`outChannels` are set from the file.
    static bool read(const std::string& path,
                      std::vector<float>& outSamples,
                      int& outSampleRate,
                      int& outChannels,
                      std::string& errorMessage);
};

} // namespace zrecord
