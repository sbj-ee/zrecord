#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <portaudio.h>

#include "AudioEngineInterface.h"
#include "Capture.h"
#include "Dropouts.h"
#include "Meter.h"
#include "PlaybackMixer.h"
#include "RingBuffer.h"
#include "TakeFile.h"
#include "Project.h"

namespace zrecord {

// Owns the PortAudio streams. Recording streams the raw input (with only the
// input gain applied) to a take file: the callback writes into a lock-free
// ring and a writer thread moves it to disk. The caller commits the take to
// a track once stopped. Playback mixes directly from a Project, through
// each track's effect stack.
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

    // While muted, incoming audio is replaced with silence before the input
    // gain, so the take keeps running (and stays in sync) but
    // captures nothing -- a "cough button". Cleared by startRecording().
    void setInputMuted(bool muted) override;
    bool isInputMuted() const override;

    // Plays back `project` from its current playheadFrame. `project` must
    // outlive the AudioEngine or until stopPlayback() is called.
    //
    // When playback runs to the end on its own, isPlaying() turns false but
    // the stream stays open until stopPlayback() (or the next startPlayback())
    // releases it. The playhead is written back only when playback was
    // stopped early; after a natural end it stays where playback started.
    bool startPlayback(Project& project, std::string& errorMessage) override;
    void stopPlayback() override;
    bool isPlaying() const override;
    void refreshPlayback() override;
    int64_t playbackFrame() const override { return playbackFrame_.load(std::memory_order_relaxed); }
    void seekPlayback(int64_t frame) override;

    void setInputGainDb(double db) override;
    double inputGainDb() const override;
    size_t drainMeterBlocks(std::vector<MeterBlock>& out) override;
    InputClipStats inputClipStats() const override;
    std::vector<LostInterval> takeDropouts() const override;
    // Frames of this take lost and padded with silence so far.
    int64_t lostFrames() const { return captureWriter_.lostFrames(); }
    // Fault injection for tests: the next input callback behaves as if the
    // host had dropped `frames` of input just before it (paInputOverflow with
    // a measured gap). Safe to call while recording.
    void simulateInputOverflowForTesting(int64_t frames) {
        injectedInputLoss_.store(frames, std::memory_order_relaxed);
    }
    double capturedSeconds() const override;
    // Frames of the take so far (on the timeline: stored plus padded).
    size_t capturedFrameCount() const;

    int channels() const { return channels_; }
    double sampleRate() const { return sampleRate_; }

    void setNextTakePath(const std::string& path) override { nextTakePath_ = path; }
    TakeFileStatus takeFileStatus() const override;
    std::vector<float> copyCapturedBuffer() const override;
    bool consumeLivePeak(float& minValue, float& maxValue) override { return takeWriter_.consumeLivePeak(minValue, maxValue); }

    // Tests: the next take's writer fails as if the disk filled up after
    // `bytes` of audio.
    void simulateTakeWriteFailureAfterBytesForTesting(int64_t bytes) { failTakeAfterBytes_ = bytes; }
    // Tests: the writer thread's timing (the ALSA null device captures far
    // faster than real time, so the tests drain more often than a real
    // device ever needs), and pausing it to overrun the ring on purpose.
    void setTakeWriterOptionsForTesting(TakeWriter::Options options) { takeWriterOptions_ = options; }
    void pauseTakeWriterForTesting(bool paused) { takeWriter_.setPausedForTesting(paused); }

    // True if the capture ring ever overflowed during this take, meaning audio
    // was dropped. Latched until the next startRecording().
    bool capturedOverrun() const;

private:
    std::string initError_; // set when Pa_Initialize failed
    static int inputCallbackStatic(const void* input, void* output, unsigned long frameCount,
                                    const PaStreamCallbackTimeInfo* timeInfo,
                                    unsigned long statusFlags, void* userData);
    static int outputCallbackStatic(const void* input, void* output, unsigned long frameCount,
                                     const PaStreamCallbackTimeInfo* timeInfo,
                                     unsigned long statusFlags, void* userData);

    int handleInput(const float* input, unsigned long frameCount, bool overflow, double adcTime);
    int handleOutput(float* output, unsigned long frameCount);

    PaStream* inputStream_ = nullptr;
    PaStream* outputStream_ = nullptr;

    std::vector<float> scratch_; // preallocated so the callback never allocates
    RingBuffer captureRing_;
    // Writes blocks into captureRing_, pads losses with silence and logs
    // them (see Dropouts.h). Audio thread while recording.
    CaptureWriter captureWriter_;
    std::atomic<int64_t> injectedInputLoss_{0}; // tests only; see above
    // Drains captureRing_ into the take file (its own thread while recording).
    TakeWriter takeWriter_;
    TakeWriter::Options takeWriterOptions_;
    std::string nextTakePath_;
    std::string tempTakePath_; // a take file we made up, removed with the next
    int64_t failTakeAfterBytes_ = -1;

    // Playback never touches the Project (or its mutex) from the callback: it
    // renders from snapshots handed over by the mixer. playbackProject_ is
    // only used on the UI thread, to refresh snapshots and write the playhead.
    Project* playbackProject_ = nullptr;
    PlaybackMixer mixer_;
    int playbackChannels_ = 1;
    size_t playbackPos_ = 0; // audio thread while the stream runs
    std::atomic<int64_t> playbackFrame_{0};
    std::atomic<int64_t> seekRequest_{-1}; // UI -> audio thread; -1 = none
    std::atomic<bool> playbackFinished_{false}; // set by the audio thread at the end

    // One queue per callback thread, so each stays single-producer.
    static constexpr size_t kMeterQueueBlocks = 512;
    MeterQueue inputMeterQueue_;
    MeterQueue outputMeterQueue_;
    MeterFeed inputMeterFeed_;  // input callback
    MeterFeed outputMeterFeed_; // output callback
    // A last block the feed held back (queue full) when its stream stopped;
    // handed out after the queue's contents. UI thread only.
    MeterBlock inputMeterTail_;
    MeterBlock outputMeterTail_;
    ClipDetector inputClip_;             // audio thread while recording
    std::atomic<int64_t> inputClipEvents_{0};
    std::atomic<int64_t> inputClippedSamples_{0};
    std::atomic<double> inputGainDb_{0.0};
    std::atomic<float> inputGain_{1.0f}; // linear, read by the audio thread
    std::atomic<bool> recording_{false};
    std::atomic<bool> inputMuted_{false};

    int channels_ = 1;
    double sampleRate_ = 44100.0;
};

} // namespace zrecord
