#pragma once

#include <string>
#include <vector>

#include "SampleBuffer.h"

namespace zrecord {

enum class AudioFormat { Wav, Flac, OggVorbis, Mp3 };

// Writes an interleaved float buffer to disk in one of the supported
// container/codec formats via libsndfile.
class AudioFileWriter {
public:
    // Returns true on success; on failure errorMessage is populated.
    static bool write(const std::string& path,
                       const std::vector<float>& interleaved,
                       int sampleRate,
                       int channels,
                       AudioFormat format,
                       std::string& errorMessage);

    // Lossless storage for project clips: 32-bit float WAV keeps every
    // sample exactly, including values beyond +/-1.0 that gain or effects can
    // produce. (Integer PCM cannot hold those; see write().)
    static bool writeFloatWav(const std::string& path,
                               const SampleBuffer& interleaved,
                               int sampleRate,
                               int channels,
                               std::string& errorMessage);

    static const char* extensionFor(AudioFormat format);
    static const char* nameFor(AudioFormat format);
};

} // namespace zrecord
