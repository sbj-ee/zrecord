#include "AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace zrecord {

AudioEngine::AudioEngine() {
    Pa_Initialize();
}

AudioEngine::~AudioEngine() {
    stopRecording();
    stopPlayback();
    Pa_Terminate();
}

std::vector<AudioDeviceInfo> AudioEngine::listInputDevices() const {
    std::vector<DeviceInfo> devices;
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info != nullptr && info->maxInputChannels > 0) {
            DeviceInfo d;
            d.index = i;
            d.name = info->name;
            d.maxInputChannels = info->maxInputChannels;
            d.defaultSampleRate = info->defaultSampleRate;
            devices.push_back(d);
        }
    }
    return devices;
}

int AudioEngine::defaultInputDeviceIndex() const {
    return Pa_GetDefaultInputDevice();
}

bool AudioEngine::startRecording(int deviceIndex, int channels, double sampleRate, std::string& errorMessage) {
    if (recording_.load()) {
        errorMessage = "Already recording";
        return false;
    }

    channels_ = std::max(1, channels);
    sampleRate_ = sampleRate;
    inputMuted_.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(captureMutex_);
        filterChain_.prepare(sampleRate_, channels_);
        captureBuffer_.clear();
        consumedOffset_ = 0;
    }

    PaStreamParameters inputParams{};
    inputParams.device = deviceIndex;
    inputParams.channelCount = channels_;
    inputParams.sampleFormat = paFloat32;
    const PaDeviceInfo* devInfo = Pa_GetDeviceInfo(deviceIndex);
    inputParams.suggestedLatency = devInfo != nullptr ? devInfo->defaultLowInputLatency : 0.05;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(&inputStream_, &inputParams, nullptr, sampleRate_,
                                 paFramesPerBufferUnspecified, paNoFlag,
                                 &AudioEngine::inputCallbackStatic, this);
    if (err != paNoError) {
        errorMessage = Pa_GetErrorText(err);
        inputStream_ = nullptr;
        return false;
    }

    err = Pa_StartStream(inputStream_);
    if (err != paNoError) {
        errorMessage = Pa_GetErrorText(err);
        Pa_CloseStream(inputStream_);
        inputStream_ = nullptr;
        return false;
    }

    recording_ = true;
    return true;
}

void AudioEngine::stopRecording() {
    if (inputStream_ != nullptr) {
        Pa_StopStream(inputStream_);
        Pa_CloseStream(inputStream_);
        inputStream_ = nullptr;
    }
    recording_ = false;
    peakLevel_.store(0.0f, std::memory_order_relaxed);
}

bool AudioEngine::isRecording() const {
    return recording_.load();
}

void AudioEngine::setInputMuted(bool muted) {
    inputMuted_.store(muted, std::memory_order_relaxed);
}

bool AudioEngine::isInputMuted() const {
    return inputMuted_.load(std::memory_order_relaxed);
}

bool AudioEngine::startPlayback(Project& project, std::string& errorMessage) {
    if (isPlaying()) {
        errorMessage = "Already playing";
        return false;
    }
    if (project.lengthFrames() <= 0) {
        errorMessage = "Nothing to play";
        return false;
    }

    playbackProject_ = &project;
    playbackPos_ = static_cast<size_t>(std::max<int64_t>(0, project.playheadFrame));

    PaStreamParameters outputParams{};
    outputParams.device = Pa_GetDefaultOutputDevice();
    if (outputParams.device == paNoDevice) {
        errorMessage = "No default output device available";
        return false;
    }
    outputParams.channelCount = project.channels;
    outputParams.sampleFormat = paFloat32;
    const PaDeviceInfo* devInfo = Pa_GetDeviceInfo(outputParams.device);
    outputParams.suggestedLatency = devInfo != nullptr ? devInfo->defaultLowOutputLatency : 0.05;
    outputParams.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(&outputStream_, nullptr, &outputParams, project.sampleRate,
                                 paFramesPerBufferUnspecified, paNoFlag,
                                 &AudioEngine::outputCallbackStatic, this);
    if (err != paNoError) {
        errorMessage = Pa_GetErrorText(err);
        outputStream_ = nullptr;
        playbackProject_ = nullptr;
        return false;
    }

    err = Pa_StartStream(outputStream_);
    if (err != paNoError) {
        errorMessage = Pa_GetErrorText(err);
        Pa_CloseStream(outputStream_);
        outputStream_ = nullptr;
        playbackProject_ = nullptr;
        return false;
    }
    return true;
}

void AudioEngine::stopPlayback() {
    if (outputStream_ != nullptr) {
        Pa_StopStream(outputStream_);
        Pa_CloseStream(outputStream_);
        outputStream_ = nullptr;
    }
    if (playbackProject_ != nullptr) {
        playbackProject_->playheadFrame = static_cast<int64_t>(playbackPos_);
        playbackProject_ = nullptr;
    }
}

bool AudioEngine::isPlaying() const {
    if (outputStream_ == nullptr) {
        return false;
    }
    return Pa_IsStreamActive(outputStream_) == 1;
}

void AudioEngine::setFilterSettings(const FilterSettings& settings) {
    std::lock_guard<std::mutex> lock(captureMutex_);
    filterChain_.setSettings(settings);
}

FilterSettings AudioEngine::filterSettings() const {
    std::lock_guard<std::mutex> lock(captureMutex_);
    return filterChain_.settings();
}

float AudioEngine::peakLevel() const {
    return peakLevel_.load(std::memory_order_relaxed);
}

double AudioEngine::capturedSeconds() const {
    std::lock_guard<std::mutex> lock(captureMutex_);
    if (channels_ <= 0 || sampleRate_ <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(captureBuffer_.size()) / (channels_ * sampleRate_);
}

size_t AudioEngine::capturedFrameCount() const {
    std::lock_guard<std::mutex> lock(captureMutex_);
    return channels_ > 0 ? captureBuffer_.size() / static_cast<size_t>(channels_) : 0;
}

std::vector<float> AudioEngine::copyCapturedBuffer() const {
    std::lock_guard<std::mutex> lock(captureMutex_);
    return captureBuffer_;
}

std::vector<float> AudioEngine::consumeNewSamples() {
    std::lock_guard<std::mutex> lock(captureMutex_);
    std::vector<float> result(captureBuffer_.begin() + static_cast<long>(consumedOffset_), captureBuffer_.end());
    consumedOffset_ = captureBuffer_.size();
    return result;
}

int AudioEngine::inputCallbackStatic(const void* input, void* /*output*/, unsigned long frameCount,
                                      const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                      unsigned long /*statusFlags*/, void* userData) {
    auto* self = static_cast<AudioEngine*>(userData);
    return self->handleInput(static_cast<const float*>(input), frameCount);
}

int AudioEngine::outputCallbackStatic(const void* /*input*/, void* output, unsigned long frameCount,
                                       const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                       unsigned long /*statusFlags*/, void* userData) {
    auto* self = static_cast<AudioEngine*>(userData);
    return self->handleOutput(static_cast<float*>(output), frameCount);
}

int AudioEngine::handleInput(const float* input, unsigned long frameCount) {
    std::vector<float> block(static_cast<size_t>(frameCount) * static_cast<size_t>(channels_), 0.0f);
    // A muted take still advances (so timing stays intact) but records
    // silence, and the level meter correctly reads zero.
    if (input != nullptr && !inputMuted_.load(std::memory_order_relaxed)) {
        std::copy(input, input + block.size(), block.begin());
    }

    float peak = 0.0f;
    for (float sample : block) {
        peak = std::max(peak, std::fabs(sample));
    }

    {
        std::lock_guard<std::mutex> lock(captureMutex_);
        filterChain_.process(block, frameCount);
        captureBuffer_.insert(captureBuffer_.end(), block.begin(), block.end());
    }

    peakLevel_.store(peak, std::memory_order_relaxed);
    return paContinue;
}

int AudioEngine::handleOutput(float* output, unsigned long frameCount) {
    if (playbackProject_ == nullptr) {
        std::fill(output, output + frameCount * static_cast<unsigned long>(channels_), 0.0f);
        return paComplete;
    }

    Project& project = *playbackProject_;
    std::vector<float> mix(static_cast<size_t>(frameCount) * static_cast<size_t>(project.channels), 0.0f);
    {
        std::lock_guard<std::mutex> lock(project.mutex);
        project.readMix(static_cast<int64_t>(playbackPos_), static_cast<int64_t>(frameCount), mix);
    }
    std::copy(mix.begin(), mix.end(), output);

    playbackPos_ += frameCount;
    bool reachedEnd = static_cast<int64_t>(playbackPos_) >= project.lengthFrames();
    return reachedEnd ? paComplete : paContinue;
}

} // namespace zrecord
