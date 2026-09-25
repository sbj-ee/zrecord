#pragma once

#include "AudioEngineInterface.h"

namespace zrecord {

// An AudioEngineInterface that touches no hardware. Recording and playback are
// just flags, so a test can put MainWindow into any transport state and check
// what the UI does about it.
class FakeAudioEngine : public AudioEngineInterface {
public:
    std::vector<AudioDeviceInfo> listInputDevices() const override {
        AudioDeviceInfo device;
        device.index = 0;
        device.name = "Fake Input";
        device.maxInputChannels = 2;
        return {device};
    }
    int defaultInputDeviceIndex() const override { return 0; }

    bool startRecording(int, int, double, std::string&) override {
        recording_ = true;
        muted_ = false;
        return true;
    }
    void stopRecording() override { recording_ = false; }
    bool isRecording() const override { return recording_; }

    void setInputMuted(bool muted) override { muted_ = muted; }
    bool isInputMuted() const override { return muted_; }

    bool startPlayback(Project&, std::string&) override {
        playing_ = true;
        return true;
    }
    void stopPlayback() override {
        playing_ = false;
        ++stopPlaybackCalls_;
    }
    bool isPlaying() const override { return playing_; }

    void setFilterSettings(const FilterSettings& settings) override { settings_ = settings; }
    FilterSettings settings() const { return settings_; }

    float peakLevel() const override { return 0.0f; }
    double capturedSeconds() const override { return 0.0; }
    std::vector<float> copyCapturedBuffer() const override { return captured_; }
    std::vector<float> consumeNewSamples() override { return {}; }

    // Lets a test hand a finished "take" to MainWindow when it stops.
    void setCapturedBuffer(std::vector<float> samples) { captured_ = std::move(samples); }

    // Playback reaching the end by itself: the stream goes inactive without
    // anyone calling stopPlayback().
    void finishPlayback() { playing_ = false; }
    int stopPlaybackCalls() const { return stopPlaybackCalls_; }

private:
    bool recording_ = false;
    bool playing_ = false;
    bool muted_ = false;
    int stopPlaybackCalls_ = 0;
    FilterSettings settings_;
    std::vector<float> captured_;
};

} // namespace zrecord
