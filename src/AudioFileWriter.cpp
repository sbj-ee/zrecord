#include "AudioFileWriter.h"

#include <sndfile.h>
#include <algorithm>
#include <cstring>

namespace zrecord {

namespace {

int sfFormatFor(AudioFormat format) {
    switch (format) {
        case AudioFormat::Wav:
            return SF_FORMAT_WAV | SF_FORMAT_PCM_24;
        case AudioFormat::Flac:
            return SF_FORMAT_FLAC | SF_FORMAT_PCM_24;
        case AudioFormat::OggVorbis:
            return SF_FORMAT_OGG | SF_FORMAT_VORBIS;
        case AudioFormat::Mp3:
            return SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
    }
    return SF_FORMAT_WAV | SF_FORMAT_PCM_24;
}

} // namespace

namespace {

// Either a contiguous vector or a chunked SampleBuffer; the latter is streamed
// out through a small staging buffer so saving never makes a flat copy.
struct Source {
    const std::vector<float>* vector = nullptr;
    const SampleBuffer* buffer = nullptr;
    size_t size() const { return vector != nullptr ? vector->size() : buffer->size(); }
};

bool writeWithFormat(const std::string& path, const Source& source, int sampleRate,
                     int channels, int sfFormat, std::string& errorMessage) {
    SF_INFO info;
    std::memset(&info, 0, sizeof(info));
    info.samplerate = sampleRate;
    info.channels = channels;
    info.format = sfFormat;

    if (!sf_format_check(&info)) {
        errorMessage = "Unsupported format/sample-rate/channel combination";
        return false;
    }

    SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!file) {
        errorMessage = sf_strerror(nullptr);
        return false;
    }

    // Without this, libsndfile converts out-of-range floats to integer PCM by
    // plain overflow: 1.2 comes back as -0.8. Clipping is the only sane
    // behaviour for an integer target. (Float targets are unaffected.)
    if ((sfFormat & SF_FORMAT_SUBMASK) != SF_FORMAT_FLOAT) {
        sf_command(file, SFC_SET_CLIPPING, nullptr, SF_TRUE);
    }

    sf_count_t frameCount = channels > 0
        ? static_cast<sf_count_t>(source.size() / static_cast<size_t>(channels))
        : 0;
    sf_count_t written = 0;
    if (source.vector != nullptr) {
        written = sf_writef_float(file, source.vector->data(), frameCount);
    } else {
        constexpr sf_count_t kStageFrames = 8192;
        std::vector<float> stage(static_cast<size_t>(kStageFrames) * static_cast<size_t>(channels));
        while (written < frameCount) {
            sf_count_t n = std::min(kStageFrames, frameCount - written);
            source.buffer->copyTo(static_cast<size_t>(written) * channels, static_cast<size_t>(n) * channels,
                                  stage.data());
            sf_count_t w = sf_writef_float(file, stage.data(), n);
            written += w;
            if (w != n) {
                break;
            }
        }
    }

    bool ok = written == frameCount;
    if (!ok) {
        errorMessage = sf_strerror(file);
    }

    if (sf_close(file) != 0 && ok) {
        errorMessage = "Could not finish writing the file";
        ok = false;
    }
    return ok;
}

} // namespace

bool AudioFileWriter::write(const std::string& path,
                             const std::vector<float>& interleaved,
                             int sampleRate,
                             int channels,
                             AudioFormat format,
                             std::string& errorMessage) {
    return writeWithFormat(path, Source{&interleaved, nullptr}, sampleRate, channels, sfFormatFor(format),
                           errorMessage);
}

bool AudioFileWriter::writeFloatWav(const std::string& path,
                                     const SampleBuffer& interleaved,
                                     int sampleRate,
                                     int channels,
                                     std::string& errorMessage) {
    return writeWithFormat(path, Source{nullptr, &interleaved}, sampleRate, channels, SF_FORMAT_WAV | SF_FORMAT_FLOAT,
                           errorMessage);
}

const char* AudioFileWriter::extensionFor(AudioFormat format) {
    switch (format) {
        case AudioFormat::Wav: return "wav";
        case AudioFormat::Flac: return "flac";
        case AudioFormat::OggVorbis: return "ogg";
        case AudioFormat::Mp3: return "mp3";
    }
    return "wav";
}

const char* AudioFileWriter::nameFor(AudioFormat format) {
    switch (format) {
        case AudioFormat::Wav: return "WAV (uncompressed)";
        case AudioFormat::Flac: return "FLAC (lossless)";
        case AudioFormat::OggVorbis: return "OGG Vorbis";
        case AudioFormat::Mp3: return "MP3";
    }
    return "WAV";
}

} // namespace zrecord
