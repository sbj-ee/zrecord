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

    // All of this happens before the stream starts, so the audio thread is not
    // running yet and none of it needs guarding.
    filterChain_.prepare(sampleRate_, channels_);
    {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        filterChain_.setSettings(pendingSettings_);
        settingsDirty_.store(false, std::memory_order_relaxed);
    }
    captureBuffer_.clear();
    consumedOffset_ = 0;

    // Ten seconds of headroom: the UI drains every 50 ms, so this only runs
    // out if the UI thread is wedged, and then the overrun flag reports it.
    const size_t ringFrames = static_cast<size_t>(sampleRate_ * 10.0);
    captureRing_.reset(ringFrames * static_cast<size_t>(channels_));
    // paFramesPerBufferUnspecified means the host picks; size the scratch
    // generously and grow it in the callback only if the host ever exceeds it.
    scratch_.assign(static_cast<size_t>(sampleRate_ * 0.5) * static_cast<size_t>(channels_), 0.0f);

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
    // A previous playback that ran to the end is inactive but its stream is
    // still open; release it rather than overwriting (and leaking) it.
    stopPlayback();

    if (project.lengthFrames() <= 0) {
        errorMessage = "Nothing to play";
        return false;
    }

    PaStreamParameters outputParams{};
    outputParams.device = Pa_GetDefaultOutputDevice();
    if (outputParams.device == paNoDevice) {
        errorMessage = "No default output device available";
        return false;
    }

    // Set only once nothing can bail out without clearing it again.
    playbackProject_ = &project;
    playbackPos_ = static_cast<size_t>(std::max<int64_t>(0, project.playheadFrame));
    playbackFinished_.store(false);
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
    if (playbackProject_ != nullptr && !playbackFinished_.load()) {
        playbackProject_->playheadFrame = static_cast<int64_t>(playbackPos_);
    }
    playbackProject_ = nullptr;
}

bool AudioEngine::isPlaying() const {
    if (outputStream_ == nullptr) {
        return false;
    }
    return Pa_IsStreamActive(outputStream_) == 1;
}

void AudioEngine::setFilterSettings(const FilterSettings& settings) {
    std::lock_guard<std::mutex> lock(settingsMutex_);
    pendingSettings_ = settings;
    settingsDirty_.store(true, std::memory_order_release);
    if (!recording_.load()) {
        // Nothing is running, so apply it directly rather than waiting for a
        // callback that will not come.
        filterChain_.setSettings(settings);
        settingsDirty_.store(false, std::memory_order_relaxed);
    }
}

FilterSettings AudioEngine::filterSettings() const {
    std::lock_guard<std::mutex> lock(settingsMutex_);
    return pendingSettings_;
}

void AudioEngine::drainCapture() {
    captureRing_.readAll(captureBuffer_);
}

bool AudioEngine::capturedOverrun() const {
    return captureRing_.overran();
}

float AudioEngine::peakLevel() const {
    return peakLevel_.load(std::memory_order_relaxed);
}

double AudioEngine::capturedSeconds() const {
    const_cast<AudioEngine*>(this)->drainCapture();
    if (channels_ <= 0 || sampleRate_ <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(captureBuffer_.size()) / (channels_ * sampleRate_);
}

size_t AudioEngine::capturedFrameCount() const {
    const_cast<AudioEngine*>(this)->drainCapture();
    return channels_ > 0 ? captureBuffer_.size() / static_cast<size_t>(channels_) : 0;
}

std::vector<float> AudioEngine::copyCapturedBuffer() const {
    const_cast<AudioEngine*>(this)->drainCapture();
    return captureBuffer_;
}

std::vector<float> AudioEngine::consumeNewSamples() {
    drainCapture();
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
    // Real-time thread. No allocation, no blocking lock, no unbounded growth:
    // the scratch block is preallocated, settings are picked up with try_lock,
    // and the samples leave via a lock-free ring the UI drains.
    const size_t needed = static_cast<size_t>(frameCount) * static_cast<size_t>(channels_);
    if (scratch_.size() < needed) {
        // The host asked for a bigger block than we sized for. Growing here
        // allocates, which is exactly what we are avoiding -- but dropping the
        // audio would be worse, and the next take will be sized correctly.
        scratch_.resize(needed);
    }

    if (input != nullptr && !inputMuted_.load(std::memory_order_relaxed)) {
        std::copy(input, input + needed, scratch_.begin());
    } else {
        // A muted take still advances (so timing stays intact) but records
        // silence, and the level meter correctly reads zero.
        std::fill_n(scratch_.begin(), needed, 0.0f);
    }

    float peak = 0.0f;
    for (size_t i = 0; i < needed; ++i) {
        peak = std::max(peak, std::fabs(scratch_[i]));
    }

    if (settingsDirty_.load(std::memory_order_acquire)) {
        // try_lock, never lock: if the UI happens to hold it this instant we
        // simply use the current settings for one more block.
        std::unique_lock<std::mutex> lock(settingsMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            filterChain_.setSettings(pendingSettings_);
            settingsDirty_.store(false, std::memory_order_relaxed);
        }
    }

    filterChain_.process(scratch_, frameCount);
    captureRing_.write(scratch_.data(), needed);

    peakLevel_.store(peak, std::memory_order_relaxed);
    return paContinue;
}

int AudioEngine::handleOutput(float* output, unsigned long frameCount) {
    if (playbackProject_ == nullptr) {
        std::fill(output, output + frameCount * static_cast<unsigned long>(channels_), 0.0f);
        playbackFinished_.store(true);
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
    if (reachedEnd) {
        playbackFinished_.store(true);
        return paComplete;
    }
    return paContinue;
}

} // namespace zrecord
