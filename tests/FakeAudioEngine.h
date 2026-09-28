#pragma once

#include "AudioEngineInterface.h"
#include "TakeFile.h"

#include <algorithm>
#include <utility>

namespace zrecord {

// An AudioEngineInterface that touches no hardware. Recording and playback are
// just flags, so a test can put MainWindow into any transport state and check
// what the UI does about it.
class FakeAudioEngine : public AudioEngineInterface {
public:
    FakeAudioEngine() {
        AudioDeviceInfo device;
        device.index = 0;
        device.name = "Fake Input";
        device.maxInputChannels = 2;
        devices_ = {device};
    }
    std::vector<AudioDeviceInfo> listInputDevices() const override { return devices_; }
    int defaultInputDeviceIndex() const override { return devices_.empty() ? -1 : devices_.front().index; }
    void setInputDevices(std::vector<AudioDeviceInfo> devices) { devices_ = std::move(devices); }

    bool startRecording(int deviceIndex, int channels, double sampleRate, std::string&) override {
        recordingRate_ = sampleRate;
        recording_ = true;
        muted_ = false;
        lastRecordingDevice_ = deviceIndex;
        lastRecordingChannels_ = channels;
        inputClip_ = {};
        dropouts_.clear();
        takeStatus_ = TakeFileStatus{};
        takeStatus_.path = nextTakePath_;
        nextTakePath_.clear();
        return true;
    }
    int lastRecordingDevice() const { return lastRecordingDevice_; }
    int lastRecordingChannels() const { return lastRecordingChannels_; }
    // Like the real engine, the take ends up in its file: the captured
    // buffer (set by the test) is written there at Stop -- all of it, or
    // with a failure injected, only the frames "on disk".
    void stopRecording() override {
        if (recording_ && !takeStatus_.path.empty()) {
            const int channels = std::max(1, lastRecordingChannels_);
            const int64_t frames = static_cast<int64_t>(captured_.size()) / channels;
            const int64_t onDisk = takeStatus_.failed ? std::min(takeStatus_.framesOnDisk, frames) : frames;
            FloatWavAppender file;
            std::string error;
            if (file.open(takeStatus_.path, channels, static_cast<int>(recordingRate_), error)) {
                file.append(captured_.data(), onDisk);
                file.close();
            }
            takeStatus_.framesOnDisk = onDisk;
            takeStatus_.framesInMemory = frames - onDisk;
        }
        recording_ = false;
    }
    bool isRecording() const override { return recording_; }

    void setInputMuted(bool muted) override { muted_ = muted; }
    bool isInputMuted() const override { return muted_; }

    bool startPlayback(Project& project, std::string&) override {
        playing_ = true;
        lastPlaybackProject_ = &project;
        ++startPlaybackCalls_;
        return true;
    }
    // The project the last startPlayback() was given (may be dangling once
    // its owner drops it; compare, don't dereference, unless still playing).
    const Project* lastPlaybackProject() const { return lastPlaybackProject_; }
    int startPlaybackCalls() const { return startPlaybackCalls_; }
    void stopPlayback() override {
        playing_ = false;
        ++stopPlaybackCalls_;
    }
    bool isPlaying() const override { return playing_; }
    void refreshPlayback() override { ++refreshCalls_; }
    int64_t playbackFrame() const override { return playbackFrame_; }
    void setPlaybackFrame(int64_t frame) { playbackFrame_ = frame; }
    void seekPlayback(int64_t frame) override {
        playbackFrame_ = frame;
        lastSeek_ = frame;
    }
    int64_t lastSeek() const { return lastSeek_; }
    int refreshCalls() const { return refreshCalls_; }


    void setInputGainDb(double db) override { inputGainDb_ = db; }
    double inputGainDb() const override { return inputGainDb_; }
    size_t drainMeterBlocks(std::vector<MeterBlock>& out) override {
        const size_t n = meterBlocks_.size();
        out.insert(out.end(), meterBlocks_.begin(), meterBlocks_.end());
        meterBlocks_.clear();
        return n;
    }
    // Queue a block for the meter's next tick.
    void pushMeterBlock(const MeterBlock& block) { meterBlocks_.push_back(block); }
    // A steady mono level (RMS = peak), with the raw input at the same level.
    void setMeterPeak(float peak) { pushMeterBlock(steadyMeterBlock(1, peak, peak)); }
    // Just the raw input before gain (nothing recorded yet).
    void setInputPeak(float peak) {
        MeterBlock b = steadyMeterBlock(1, 0.0f, 0.0f);
        b.inputPeak[0] = peak;
        pushMeterBlock(b);
    }
    InputClipStats inputClipStats() const override { return inputClip_; }
    void setInputClipStats(InputClipStats stats) { inputClip_ = stats; }
    std::vector<LostInterval> takeDropouts() const override { return dropouts_; }
    // Losses in the take handed over at the next Stop (frames into the take).
    void setTakeDropouts(std::vector<LostInterval> dropouts) { dropouts_ = std::move(dropouts); }
    double capturedSeconds() const override { return 0.0; }
    void setNextTakePath(const std::string& path) override { nextTakePath_ = path; }
    const std::string& nextTakePath() const { return nextTakePath_; }
    TakeFileStatus takeFileStatus() const override { return takeStatus_; }
    // The take's writer "fails" now (disk full), `framesOnDisk` into it.
    void failTakeWrite(const std::string& error, int64_t framesOnDisk) {
        takeStatus_.failed = true;
        takeStatus_.error = error;
        takeStatus_.framesOnDisk = framesOnDisk;
    }
    std::vector<float> copyCapturedBuffer() const override { return captured_; }
    bool consumeLivePeak(float&, float&) override { return false; }

    // Lets a test hand a finished "take" to MainWindow when it stops.
    void setCapturedBuffer(std::vector<float> samples) { captured_ = std::move(samples); }

    // Playback reaching the end by itself: the stream goes inactive without
    // anyone calling stopPlayback().
    void finishPlayback() { playing_ = false; }
    int stopPlaybackCalls() const { return stopPlaybackCalls_; }

private:
    InputClipStats inputClip_;
    TakeFileStatus takeStatus_;
    double recordingRate_ = 44100.0;
    std::string nextTakePath_;
    std::vector<LostInterval> dropouts_;
    bool recording_ = false;
    bool playing_ = false;
    bool muted_ = false;
    int stopPlaybackCalls_ = 0;
    int startPlaybackCalls_ = 0;
    const Project* lastPlaybackProject_ = nullptr;
    std::vector<AudioDeviceInfo> devices_;
    int lastRecordingDevice_ = -1;
    int lastRecordingChannels_ = 0;
    int refreshCalls_ = 0;
    int64_t playbackFrame_ = 0;
    int64_t lastSeek_ = -1;
    std::vector<MeterBlock> meterBlocks_;
    double inputGainDb_ = 0.0;
    std::vector<float> captured_;
};

} // namespace zrecord
