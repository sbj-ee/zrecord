#include "AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace zrecord {

namespace {
void discardMeterBlocks(MeterQueue& queue) {
    MeterBlock block;
    while (queue.tryPop(block)) {
    }
}
} // namespace

AudioEngine::AudioEngine() {
    // The meter queues are allocated once, here: the callbacks never allocate.
    inputMeterQueue_.reset(kMeterQueueBlocks);
    outputMeterQueue_.reset(kMeterQueueBlocks);
    // A failure here used to go unnoticed and surface later as empty device
    // lists and puzzling errors; keep it and report it where it matters.
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        initError_ = std::string("The audio system failed to start: ") + Pa_GetErrorText(err);
    }
}

AudioEngine::~AudioEngine() {
    if (!initError_.empty()) {
        return; // nothing was started, and Pa_Terminate must match a successful Pa_Initialize
    }
    stopRecording();
    stopPlayback();
    Pa_Terminate();
}

std::vector<AudioDeviceInfo> AudioEngine::listInputDevices() const {
    std::vector<DeviceInfo> devices;
    if (!initError_.empty()) {
        return devices;
    }
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
    if (!initError_.empty()) {
        errorMessage = initError_;
        return false;
    }
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
    inputMeterFeed_.reset(channels_);
    discardMeterBlocks(inputMeterQueue_); // nothing stale from a previous take
    inputMeterTail_ = MeterBlock{};
    inputClip_.reset(channels_);
    inputClipEvents_.store(0, std::memory_order_relaxed);
    inputClippedSamples_.store(0, std::memory_order_relaxed);

    // Ten seconds of headroom: the UI drains every 50 ms, so this only runs
    // out if the UI thread is wedged, and then the overrun flag reports it.
    const size_t ringFrames = static_cast<size_t>(sampleRate_ * 10.0);
    captureRing_.reset(ringFrames * static_cast<size_t>(channels_));
    captureWriter_.reset(&captureRing_, channels_, sampleRate_);
    injectedInputLoss_.store(0, std::memory_order_relaxed);
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
        // The callback can't run any more, so its feed is ours now.
        MeterBlock tail;
        if (inputMeterFeed_.takePending(tail)) inputMeterTail_.merge(tail);
        // Silence still owed for the last losses completes the take, so its
        // length matches the time it covered.
        const int64_t owed = captureWriter_.finish();
        drainCapture();
        captureBuffer_.insert(captureBuffer_.end(), static_cast<size_t>(owed) * static_cast<size_t>(channels_), 0.0f);
    }
    recording_ = false;
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
    if (!initError_.empty()) {
        errorMessage = initError_;
        return false;
    }
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
    seekRequest_.store(-1);
    playbackPos_ = static_cast<size_t>(std::max<int64_t>(0, project.playheadFrame));
    playbackFrame_.store(static_cast<int64_t>(playbackPos_));
    playbackChannels_ = project.channels;
    outputMeterFeed_.reset(playbackChannels_); // the old stream is closed above
    discardMeterBlocks(outputMeterQueue_);
    outputMeterTail_ = MeterBlock{};
    playbackFinished_.store(false);
    mixer_.publish(PlaybackSnapshot::capture(project));
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
        mixer_.clear();
        return false;
    }

    err = Pa_StartStream(outputStream_);
    if (err != paNoError) {
        errorMessage = Pa_GetErrorText(err);
        Pa_CloseStream(outputStream_);
        outputStream_ = nullptr;
        playbackProject_ = nullptr;
        mixer_.clear();
        return false;
    }
    return true;
}

void AudioEngine::stopPlayback() {
    if (outputStream_ != nullptr) {
        Pa_StopStream(outputStream_);
        Pa_CloseStream(outputStream_);
        outputStream_ = nullptr;
        MeterBlock tail;
        if (outputMeterFeed_.takePending(tail)) outputMeterTail_.merge(tail);
    }
    int64_t pendingSeek = seekRequest_.exchange(-1);
    if (pendingSeek >= 0) {
        playbackPos_ = static_cast<size_t>(pendingSeek); // the callback never got to it
    }
    if (playbackProject_ != nullptr && !playbackFinished_.load()) {
        playbackProject_->playheadFrame = static_cast<int64_t>(playbackPos_);
    }
    playbackProject_ = nullptr;
    mixer_.clear(); // the stream is closed, so the callback can't be running
}

void AudioEngine::seekPlayback(int64_t frame) {
    if (outputStream_ == nullptr) {
        return;
    }
    frame = std::max<int64_t>(0, frame);
    // The callback owns playbackPos_; it picks this up at its next block.
    seekRequest_.store(frame, std::memory_order_release);
    playbackFrame_.store(frame, std::memory_order_relaxed);
}

void AudioEngine::refreshPlayback() {
    if (playbackProject_ == nullptr || outputStream_ == nullptr) {
        return;
    }
    mixer_.publish(PlaybackSnapshot::capture(*playbackProject_));
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

void AudioEngine::setInputGainDb(double db) {
    db = std::clamp(db, kInputGainMinDb, kInputGainMaxDb);
    inputGainDb_.store(db, std::memory_order_relaxed);
    inputGain_.store(static_cast<float>(inputGainToLinear(db)), std::memory_order_relaxed);
}

double AudioEngine::inputGainDb() const {
    return inputGainDb_.load(std::memory_order_relaxed);
}

size_t AudioEngine::drainMeterBlocks(std::vector<MeterBlock>& out) {
    // Recording and playback don't overlap, so at most one of these has
    // anything in it; draining both keeps the order within each.
    size_t n = drainMeterQueue(inputMeterQueue_, out);
    if (!inputMeterTail_.empty()) {
        out.push_back(inputMeterTail_);
        inputMeterTail_ = MeterBlock{};
        ++n;
    }
    n += drainMeterQueue(outputMeterQueue_, out);
    if (!outputMeterTail_.empty()) {
        out.push_back(outputMeterTail_);
        outputMeterTail_ = MeterBlock{};
        ++n;
    }
    return n;
}

std::vector<LostInterval> AudioEngine::takeDropouts() const {
    return captureWriter_.log().intervals();
}

InputClipStats AudioEngine::inputClipStats() const {
    return {inputClipEvents_.load(std::memory_order_relaxed), inputClippedSamples_.load(std::memory_order_relaxed)};
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
                                      const PaStreamCallbackTimeInfo* timeInfo,
                                      unsigned long statusFlags, void* userData) {
    auto* self = static_cast<AudioEngine*>(userData);
    return self->handleInput(static_cast<const float*>(input), frameCount, (statusFlags & paInputOverflow) != 0,
                             timeInfo != nullptr ? timeInfo->inputBufferAdcTime : 0.0);
}

int AudioEngine::outputCallbackStatic(const void* /*input*/, void* output, unsigned long frameCount,
                                       const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                       unsigned long /*statusFlags*/, void* userData) {
    auto* self = static_cast<AudioEngine*>(userData);
    return self->handleOutput(static_cast<float*>(output), frameCount);
}

int AudioEngine::handleInput(const float* input, unsigned long frameCount, bool overflow, double adcTime) {
    // Real-time thread. No allocation, no blocking lock, no unbounded growth:
    // the scratch block is preallocated, settings are picked up with try_lock,
    // and the samples leave via a lock-free ring the UI drains.
    if (settingsDirty_.load(std::memory_order_acquire)) {
        // try_lock, never lock: if the UI happens to hold it this instant we
        // simply use the current settings for one more block.
        std::unique_lock<std::mutex> lock(settingsMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            filterChain_.setSettings(pendingSettings_);
            settingsDirty_.store(false, std::memory_order_relaxed);
        }
    }

    // Input the host dropped before this callback (paInputOverflow) becomes
    // silence of the measured length, ahead of this block.
    const int64_t injected = injectedInputLoss_.exchange(0, std::memory_order_relaxed);
    if (injected > 0) {
        captureWriter_.noteHostLoss(injected);
    }
    captureWriter_.beginCallback(frameCount, overflow, adcTime);

    // The scratch block (0.5 s, sized at startRecording) is never grown here:
    // a host block larger than that is processed in slices instead of
    // allocating on the audio thread.
    const size_t channels = static_cast<size_t>(channels_);
    const size_t sliceFrames = scratch_.size() / channels;
    const bool muted = input == nullptr || inputMuted_.load(std::memory_order_relaxed);
    const float gain = inputGain_.load(std::memory_order_relaxed);
    for (size_t done = 0; done < frameCount && sliceFrames > 0; done += sliceFrames) {
        const size_t frames = std::min(sliceFrames, static_cast<size_t>(frameCount) - done);
        const size_t count = frames * channels;
        if (!muted) {
            std::copy(input + done * channels, input + done * channels + count, scratch_.begin());
            inputMeterFeed_.addInput(scratch_.data(), frames); // the raw input, before any gain
        } else {
            // A muted take still advances (so timing stays intact) but records
            // silence, and the level meter correctly reads zero.
            std::fill_n(scratch_.begin(), count, 0.0f);
        }
        // Input gain, then the filter chain. The meter follows what is
        // recorded, so gain or an effect that pushes the take past full scale
        // shows up (and lights CLIP) instead of hiding behind the raw level.
        // The raw input is also checked for clipping (runs of full-scale
        // samples, carried across callbacks) before the gain touches it.
        processCaptureBlock(scratch_, frames, channels_, gain, filterChain_, &inputClip_);
        inputMeterFeed_.addSignal(scratch_.data(), frames); // what the take gets
        captureWriter_.write(scratch_.data(), frames); // or logs it lost, if the ring is full
    }

    inputMeterFeed_.publish(inputMeterQueue_);
    inputClipEvents_.store(inputClip_.events(), std::memory_order_relaxed);
    inputClippedSamples_.store(inputClip_.clippedSamples(), std::memory_order_relaxed);
    return paContinue;
}

int AudioEngine::handleOutput(float* output, unsigned long frameCount) {
    // Real-time path: no lock, no allocation. The mixer renders straight into
    // PortAudio's buffer from the current snapshot.
    int64_t seek = seekRequest_.exchange(-1, std::memory_order_acquire);
    if (seek >= 0) {
        playbackPos_ = static_cast<size_t>(seek);
    }
    bool more = mixer_.render(static_cast<int64_t>(playbackPos_), output, frameCount, playbackChannels_);
    outputMeterFeed_.addSignal(output, frameCount);
    outputMeterFeed_.publish(outputMeterQueue_);
    playbackPos_ += frameCount;
    playbackFrame_.store(static_cast<int64_t>(playbackPos_), std::memory_order_relaxed);
    if (!more) {
        playbackFinished_.store(true);
        return paComplete;
    }
    return paContinue;
}

} // namespace zrecord
