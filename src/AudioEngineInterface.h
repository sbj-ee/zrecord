#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Dropouts.h"
#include "Filters.h"
#include "Meter.h"
#include "Project.h"

namespace zrecord {

// Clipping found in the raw recording input (see ClipDetector in Capture.h).
struct InputClipStats {
    int64_t events = 0;  // runs of consecutive full-scale samples
    int64_t samples = 0; // samples in those runs, all channels
};

// Where the current (or last) take is being streamed, and how that's going.
struct TakeFileStatus {
    std::string path;           // the take's WAV file
    bool failed = false;        // a write failed: the take should be stopped
    std::string error;          // why (e.g. "... No space left on device")
    int64_t framesOnDisk = 0;   // the file holds this many frames, intact
    int64_t framesInMemory = 0; // captured after the failure, kept in memory
    int64_t framesDropped = 0;  // beyond even that (reported, never silent)
};

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
    // Hands playback a fresh copy of the project, so edits made while playing
    // are heard. Cheap; called from the UI tick while playing.
    virtual void refreshPlayback() = 0;
    // The frame playback has reached (valid while playing and just after).
    virtual int64_t playbackFrame() const = 0;
    // Jumps running playback to `frame` without restarting the stream.
    virtual void seekPlayback(int64_t frame) = 0;

    // Digital input gain, in dB: the only processing on the capture path
    // (effects are per-track and applied on playback). 0 dB (the default)
    // records exactly what arrives.
    virtual void setInputGainDb(double db) = 0;
    virtual double inputGainDb() const = 0;

    // Level-meter blocks measured by the audio callbacks since the previous
    // call, appended to `out` oldest first: while recording, what is written
    // to the take (after the input gain) plus the raw input peak before any
    // gain; while playing, the mixed output (after each track's effects).
    // Each block has per-channel peak, sum of squares (for RMS) and clip
    // flags, and none is dropped: if the UI falls behind, blocks are merged, never lost.
    // Lock-free on both sides; call from the UI thread.
    virtual size_t drainMeterBlocks(std::vector<MeterBlock>& out) = 0;
    // Clipping in the raw input (before any gain) during the current take, or
    // the last one once stopped. Starts again from zero with every take.
    virtual InputClipStats inputClipStats() const = 0;
    // Input lost during the current take, or the last one once stopped:
    // where (frames into the take) and how much, oldest first. Lost stretches
    // are padded with silence in the take, so these line up with its audio.
    // Complete only after stopRecording().
    virtual std::vector<LostInterval> takeDropouts() const = 0;
    virtual double capturedSeconds() const = 0;

    // Takes are streamed to disk while they're recorded, by a writer thread
    // (never the audio callback), so memory stays bounded however long the
    // take. The next startRecording() streams into `path` (a .wav); empty
    // means a temporary file the engine removes itself.
    virtual void setNextTakePath(const std::string& path) = 0;
    virtual TakeFileStatus takeFileStatus() const = 0;
    // The finished take, after stopRecording(): what the file holds, then
    // anything a write failure left in memory.
    virtual std::vector<float> copyCapturedBuffer() const = 0;
    // The quietest and loudest sample captured since the previous call, for
    // the live waveform; false if nothing new arrived.
    virtual bool consumeLivePeak(float& minValue, float& maxValue) = 0;
};

} // namespace zrecord
