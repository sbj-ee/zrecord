#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <portaudio.h>

#include "Filters.h"
#include "Project.h"

namespace zrecord {

// Owns the PortAudio streams. Recording captures into a private scratch
// buffer (through the live FilterChain) that the caller commits to a track
// once stopped; playback mixes directly from a Project.
class AudioEngine {
public:
    struct DeviceInfo {
        int index = -1;
        std::string name;
        int maxInputChannels = 0;
        double defaultSampleRate = 44100.0;
    };

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    std::vector<DeviceInfo> listInputDevices() const;
    int defaultInputDeviceIndex() const;

    bool startRecording(int deviceIndex, int channels, double sampleRate, std::string& errorMessage);
    void stopRecording();
    bool isRecording() const;

    // Plays back `project` from its current playheadFrame. `project` must
    // outlive the AudioEngine or until stopPlayback() is called.
    bool startPlayback(Project& project, std::string& errorMessage);
    void stopPlayback();
    bool isPlaying() const;

    void setFilterSettings(const FilterSettings& settings);
    FilterSettings filterSettings() const;

    float peakLevel() const;
    double capturedSeconds() const;
    size_t capturedFrameCount() const;

    int channels() const { return channels_; }
    double sampleRate() const { return sampleRate_; }

    // Returns a copy of the current capture buffer (the take currently being
    // or just having been recorded), safe to call any time.
    std::vector<float> copyCapturedBuffer() const;

    // Returns interleaved samples appended since the last call (or since
    // startRecording), for incremental consumers like a live waveform view.
    std::vector<float> consumeNewSamples();

private:
    static int inputCallbackStatic(const void* input, void* output, unsigned long frameCount,
                                    const PaStreamCallbackTimeInfo* timeInfo,
                                    unsigned long statusFlags, void* userData);
    static int outputCallbackStatic(const void* input, void* output, unsigned long frameCount,
                                     const PaStreamCallbackTimeInfo* timeInfo,
                                     unsigned long statusFlags, void* userData);

    int handleInput(const float* input, unsigned long frameCount);
    int handleOutput(float* output, unsigned long frameCount);

    PaStream* inputStream_ = nullptr;
    PaStream* outputStream_ = nullptr;

    mutable std::mutex captureMutex_;
    FilterChain filterChain_;
    std::vector<float> captureBuffer_;
    size_t consumedOffset_ = 0;

    Project* playbackProject_ = nullptr;
    size_t playbackPos_ = 0;

    std::atomic<float> peakLevel_{0.0f};
    std::atomic<bool> recording_{false};

    int channels_ = 1;
    double sampleRate_ = 44100.0;
};

} // namespace zrecord
