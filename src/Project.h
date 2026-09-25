#pragma once

#include <QMetaType>

#include <cstdint>
#include <mutex>
#include <string>
#include <memory>
#include <vector>

#include "SampleBuffer.h"

namespace zrecord {

// Lazily-built min/max mipmap for one clip's samples, so the timeline can
// paint zoomed-out waveforms without rescanning every raw sample per frame.
class PeakCache {
public:
    static constexpr int64_t kBlockFrames = 256;

    // (Re)builds the cache from `samples` (interleaved, `channels` wide).
    void build(const SampleBuffer& samples, int channels);
    void invalidate();

    struct MinMax {
        float minValue = 0.0f;
        float maxValue = 0.0f;
    };

    // Peak of frame block `blockIndex` (covering
    // [blockIndex*kBlockFrames, (blockIndex+1)*kBlockFrames) ). Valid only
    // after build().
    MinMax blockAt(int64_t blockIndex) const;
    int64_t blockCount() const { return blocks_ ? static_cast<int64_t>(blocks_->size()) : 0; }
    bool isBuilt() const { return built_; }

private:
    // Shared and immutable, like the samples, so copying a clip for an undo
    // snapshot doesn't copy its peaks either.
    std::shared_ptr<const std::vector<MinMax>> blocks_;
    bool built_ = false;
};

// One contiguous span of recorded/imported audio, placed on a track's
// timeline at startFrame. The samples are shared, immutable chunks (see
// SampleBuffer), so copying a Clip is cheap and never copies audio.
struct Clip {
    SampleBuffer samples; // interleaved, `channels` wide
    int64_t startFrame = 0;
    int channels = 1;
    PeakCache peaks;

    int64_t frameCount() const {
        return channels > 0 ? static_cast<int64_t>(samples.size()) / channels : 0;
    }
    int64_t endFrame() const { return startFrame + frameCount(); }
};

// A point on a track's volume envelope. Envelopes belong to the track rather
// than to a clip, matching Audacity: a curve is something you draw over a
// stretch of time, and tying it to clips would mean deciding what happens to it
// every time one is split, moved or crossfaded.
struct EnvelopePoint {
    int64_t frame = 0;
    float gain = 1.0f; // linear, 1.0 = unity
};

// How a track's audio is drawn. Spectrogram is a view setting, not audio
// state: it changes nothing about playback or export.
enum class TrackDisplay { Waveform, Spectrogram };

struct Track {
    std::string name;
    TrackDisplay display = TrackDisplay::Waveform;
    std::vector<Clip> clips; // kept sorted by startFrame, non-overlapping
    bool muted = false;
    bool soloed = false;
    bool recordArmed = false;
    double gainDb = 0.0;
    std::vector<EnvelopePoint> envelope; // kept sorted by frame; empty = unity

    int64_t endFrame() const;

    // Envelope gain at `frame`: linear interpolation between the surrounding
    // points, flat outside the first and last, and unity when there are none.
    float envelopeGainAt(int64_t frame) const;

    // Inserts a point keeping `envelope` sorted; replaces one already at that
    // frame. Returns its index.
    int insertEnvelopePoint(const EnvelopePoint& point);
};

// A named point or span on the timeline. Labels belong to the project rather
// than to a track (Audacity puts them in a dedicated label *track*); with a
// single global lane there's no track-type polymorphism to introduce, at the
// cost of not being able to keep separate sets of labels.
//
// Positions are absolute and do not follow ripple edits: a label marks a point
// in time, not a point in a particular track's audio.
struct Label {
    int64_t startFrame = 0;
    int64_t endFrame = 0; // equal to startFrame for a point marker
    std::string text;

    bool isRange() const { return endFrame > startFrame; }
};

// One clip's requested destination. A multi-clip drag produces a set of these
// so the whole move can be validated and applied as a single undoable step.
struct ClipMove {
    int fromTrack = -1;
    int clipIndex = -1;
    int toTrack = -1;
    int64_t newStartFrame = 0;
};

struct Selection {
    int trackIndex = -1;
    int64_t startFrame = 0;
    int64_t endFrame = 0;

    bool isEmpty() const { return trackIndex < 0 || startFrame >= endFrame; }
    void clear() { trackIndex = -1; startFrame = 0; endFrame = 0; }
};

// The full editable document: a set of tracks sharing one sample rate and
// channel count, a selection, and a playhead. All mutation happens on the UI
// thread. Playback never reads the Project from the audio callback: it plays
// a PlaybackSnapshot taken under `mutex`, which mutations also hold, so a
// snapshot always sees a consistent set of tracks.
class Project {
public:
    mutable std::mutex mutex;

    double sampleRate = 44100.0;
    int channels = 2;
    std::vector<Track> tracks;
    std::vector<Label> labels; // kept sorted by startFrame
    Selection selection;
    int64_t playheadFrame = 0;
    std::vector<float> clipboard; // interleaved, `channels` wide

    int64_t lengthFrames() const;

    // Resets to an empty single project in place (Project holds a mutex, so
    // it cannot be copy/move-assigned wholesale).
    void reset();

    // Splits the clip covering `frame` on `track` into two at that frame, if
    // `frame` falls strictly inside a clip. No-op otherwise.
    static void splitClipAt(Track& track, int64_t frame, int channels);

    // Removes [startFrame, endFrame) from `track`, rippling later clips left
    // by the removed length. Returns the removed audio (silence-filled where
    // no clip covered a gap).
    static std::vector<float> removeRange(Track& track, int64_t startFrame, int64_t endFrame, int channels);

    // Inserts `samples` at `atFrame` on `track`, rippling later clips right.
    static void insertRange(Track& track, int64_t atFrame, const SampleBuffer& samples, int channels);

    // Zeroes [startFrame, endFrame) in place; track length is unchanged.
    static void silenceRange(Track& track, int64_t startFrame, int64_t endFrame, int channels);

    // Reads [startFrame, endFrame) from `track` without modifying it
    // (silence-filled where no clip covers a gap). Used by Copy.
    static std::vector<float> copyRange(const Track& track, int64_t startFrame, int64_t endFrame, int channels);

    // Overwrites [startFrame, endFrame) in place with `samples` (which must
    // hold (endFrame-startFrame)*channels values); track length and clip
    // boundaries are unchanged. Frames in a gap (not covered by any clip)
    // are left as-is, since there's no clip to hold them. Used to write back
    // the result of processing a selection through an effect.
    static void writeRange(Track& track, int64_t startFrame, int64_t endFrame, const std::vector<float>& samples, int channels);

    // Appends `samples` as a new clip at the track's current end.
    static void appendClip(Track& track, const SampleBuffer& samples, int channels);

    // Merges clips `firstClipIndex` and the one after it into a single clip,
    // overlapping them by `frames` and mixing that overlap with equal-power
    // ramps. Later clips ripple left by `frames`, so the track shortens by the
    // crossfade length -- which is what a real crossfade does.
    //
    // Merging rather than letting the two clips overlap on the timeline keeps
    // the "clips are sorted and non-overlapping" invariant that every range
    // operation here relies on. Returns false, leaving the track untouched, if
    // the clips aren't adjacent or either is shorter than `frames`.
    static bool crossfadeClips(Track& track, int firstClipIndex, int64_t frames, int channels);

    // Inserts `label` keeping `labels` sorted by startFrame; returns its index.
    int insertLabel(const Label& label);

    // Mixes all audible tracks (soloed tracks only, if any are soloed;
    // otherwise all unmuted tracks) over [startFrame, startFrame+frameCount)
    // into `out`, which must already be sized frameCount*channels and will
    // be overwritten (not accumulated into). Caller must hold `mutex`.
    void readMix(int64_t startFrame, int64_t frameCount, std::vector<float>& out) const;

    // The mixing behind readMix, over any set of tracks: writes
    // frameCount*channels samples to `out`. Allocation- and lock-free, so the
    // playback callback can call it on a snapshot.
    static void mixTracks(const std::vector<Track>& tracks, int channels, int64_t startFrame,
                          int64_t frameCount, float* out);

    // Renders the whole project to one interleaved buffer, for export.
    std::vector<float> renderMixdown() const;
};

} // namespace zrecord

// Registered so QSignalSpy can carry a move list through QVariant.
Q_DECLARE_METATYPE(std::vector<zrecord::ClipMove>)
