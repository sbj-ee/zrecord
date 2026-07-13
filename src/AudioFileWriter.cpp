#include "AudioFileWriter.h"

#include <sndfile.h>
#include <cstring>

namespace zrecord {

namespace {

int sfFormatFor(AudioFormat format) {
    switch (format) {
        case AudioFormat::Wav:
            return SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        case AudioFormat::Flac:
            return SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
        case AudioFormat::OggVorbis:
            return SF_FORMAT_OGG | SF_FORMAT_VORBIS;
        case AudioFormat::Mp3:
            return SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
    }
    return SF_FORMAT_WAV | SF_FORMAT_PCM_16;
}

} // namespace

bool AudioFileWriter::write(const std::string& path,
                             const std::vector<float>& interleaved,
                             int sampleRate,
                             int channels,
                             AudioFormat format,
                             std::string& errorMessage) {
    SF_INFO info;
    std::memset(&info, 0, sizeof(info));
    info.samplerate = sampleRate;
    info.channels = channels;
    info.format = sfFormatFor(format);

    if (!sf_format_check(&info)) {
        errorMessage = "Unsupported format/sample-rate/channel combination";
        return false;
    }

    SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!file) {
        errorMessage = sf_strerror(nullptr);
        return false;
    }

    sf_count_t frameCount = channels > 0
        ? static_cast<sf_count_t>(interleaved.size() / static_cast<size_t>(channels))
        : 0;
    sf_count_t written = sf_writef_float(file, interleaved.data(), frameCount);

    bool ok = written == frameCount;
    if (!ok) {
        errorMessage = sf_strerror(file);
    }

    sf_close(file);
    return ok;
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
