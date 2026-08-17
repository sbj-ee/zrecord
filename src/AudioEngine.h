#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <portaudio.h>

#include "AudioEngineInterface.h"
#include "Filters.h"
#include "RingBuffer.h"
#include "Project.h"

namespace zrecord {

// Owns the PortAudio streams. Recording captures into a private scratch
// buffer (through the live FilterChain) that the caller commits to a track
// once stopped; playback mixes directly from a Project.
class AudioEngine : public AudioEngineInterface {
public:
    // Kept as an alias so existing call sites read unchanged.
    using DeviceInfo = AudioDeviceInfo;

    AudioEngine();
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    std::vector<DeviceInfo> listInputDevices() const override;
    int defaultInputDeviceIndex() const override;

    bool startRecording(int deviceIndex, int channels, double sampleRate, std::string& errorMessage) override;
    void stopRecording() override;
    bool isRecording() const override;

    // While muted, incoming audio is replaced with silence before it reaches
    // the filter chain, so the take keeps running (and stays in sync) but
    // captures nothing -- a "cough button". Cleared by startRecording().
    void setInputMuted(bool muted) override;
    bool isInputMuted() const override;

    // Plays back `project` from its current playheadFrame. `project` must
    // outlive the AudioEngine or until stopPlayback() is called.
    bool startPlayback(Project& project, std::string& errorMessage) override;
    void stopPlayback() override;
    bool isPlaying() const override;

    void setFilterSettings(const FilterSettings& settings) override;
    FilterSettings filterSettings() const;

    float peakLevel() const override;
    double capturedSeconds() const override;
    size_t capturedFrameCount() const;

    int channels() const { return channels_; }
    double sampleRate() const { return sampleRate_; }

    // Returns a copy of the current capture buffer (the take currently being
    // or just having been recorded), safe to call any time.
    std::vector<float> copyCapturedBuffer() const override;

    // Returns interleaved samples appended since the last call (or since
    // startRecording), for incremental consumers like a live waveform view.
    std::vector<float> consumeNewSamples() override;

    // True if the capture ring ever overflowed during this take, meaning audio
    // was dropped. Latched until the next startRecording().
    bool capturedOverrun() const;

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

    // Moves whatever the audio thread has produced into captureBuffer_.
    // Consumer side only: every public accessor below calls it first, and they
    // are all UI-thread.
    void drainCapture();

    // Guards only the filter settings handoff, never the capture path. The
    // audio thread takes it with try_lock, so it can never be blocked by the
    // UI holding it.
    mutable std::mutex settingsMutex_;
    FilterSettings pendingSettings_;
    std::atomic<bool> settingsDirty_{false};

    FilterChain filterChain_;   // audio thread only, once recording starts
    std::vector<float> scratch_; // preallocated so the callback never allocates
    RingBuffer captureRing_;

    std::vector<float> captureBuffer_; // UI thread only
    size_t consumedOffset_ = 0;

    Project* playbackProject_ = nullptr;
    size_t playbackPos_ = 0;

    std::atomic<float> peakLevel_{0.0f};
    std::atomic<bool> recording_{false};
    std::atomic<bool> inputMuted_{false};

    int channels_ = 1;
    double sampleRate_ = 44100.0;
};

} // namespace zrecord
