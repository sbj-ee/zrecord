#pragma once

#include <string>
#include <vector>

#include "Filters.h"
#include "Project.h"

namespace zrecord {

struct AudioDeviceInfo {
    int index = -1;
    std::string name;
    int maxInputChannels = 0;
    double defaultSampleRate = 44100.0;
};

// What MainWindow needs from the audio layer, and nothing more.
//
// This exists so MainWindow can be constructed in a test. The concrete
// AudioEngine calls Pa_Initialize() and opens real devices, which a CI runner
// with no sound hardware can't satisfy -- and MainWindow is precisely where
// enabled-state wiring across menu, toolbar and buttons is easy to get subtly
// wrong, so it's the class most worth being able to exercise headlessly.
class AudioEngineInterface {
public:
    virtual ~AudioEngineInterface() = default;

    virtual std::vector<AudioDeviceInfo> listInputDevices() const = 0;
    virtual int defaultInputDeviceIndex() const = 0;

    virtual bool startRecording(int deviceIndex, int channels, double sampleRate,
                                 std::string& errorMessage) = 0;
    virtual void stopRecording() = 0;
    virtual bool isRecording() const = 0;

    virtual void setInputMuted(bool muted) = 0;
    virtual bool isInputMuted() const = 0;

    virtual bool startPlayback(Project& project, std::string& errorMessage) = 0;
    virtual void stopPlayback() = 0;
    virtual bool isPlaying() const = 0;

    virtual void setFilterSettings(const FilterSettings& settings) = 0;

    virtual float peakLevel() const = 0;
    virtual double capturedSeconds() const = 0;
    virtual std::vector<float> copyCapturedBuffer() const = 0;
    virtual std::vector<float> consumeNewSamples() = 0;
};

} // namespace zrecord
