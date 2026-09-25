#pragma once

#include <QUndoCommand>
#include <optional>

#include "Filters.h"
#include "Project.h"

namespace zrecord {

// Base for edits that mutate exactly one track. Undo/redo work by snapshotting
// the track's full clip list before and after the edit, rather than trying to
// invert each operation precisely -- simple to get right, and the clips
// involved are small enough (desktop recording scale) that copying them is
// cheap relative to the safety it buys.
class TrackEditCommand : public QUndoCommand {
public:
    TrackEditCommand(Project& project, int trackIndex, const QString& text);

    void undo() override;
    void redo() override;

protected:
    Project& project() { return project_; }
    Track& track() { return project_.tracks[static_cast<size_t>(trackIndex_)]; }

    // Mutates track(), starting from the pre-edit snapshot. Called exactly
    // once, on the first redo().
    virtual void apply() = 0;

    Selection selectionBefore_;
    Selection selectionAfter_;
    int64_t playheadBefore_ = 0;
    int64_t playheadAfter_ = 0;

private:
    Project& project_;
    int trackIndex_;
    Track beforeSnapshot_;
    std::optional<Track> afterSnapshot_;
};

class DeleteSelectionCommand : public TrackEditCommand {
public:
    DeleteSelectionCommand(Project& project, int trackIndex, int64_t startFrame, int64_t endFrame);

protected:
    void apply() override;

private:
    int64_t startFrame_;
    int64_t endFrame_;
};

class SilenceSelectionCommand : public TrackEditCommand {
public:
    SilenceSelectionCommand(Project& project, int trackIndex, int64_t startFrame, int64_t endFrame);

protected:
    void apply() override;

private:
    int64_t startFrame_;
    int64_t endFrame_;
};

// Runs a selection through a FilterChain configured with `settings` and
// writes the result back in place (same length, same clip boundaries). The
// chain starts from a clean state, so attack/release-style filters (noise
// gate, compressor, limiter) don't carry over context from audio before the
// selection -- a known limitation of applying an effect to an isolated span.
class ApplyEffectCommand : public TrackEditCommand {
public:
    ApplyEffectCommand(Project& project, int trackIndex, int64_t startFrame, int64_t endFrame,
                        const FilterSettings& settings, double sampleRate, int channels);

protected:
    void apply() override;

private:
    int64_t startFrame_;
    int64_t endFrame_;
    FilterSettings settings_;
    double sampleRate_;
    int channels_;
};

// Ramps the selection from silence to unity, or unity to silence, in place.
// Same read-process-write shape as ApplyEffectCommand, but the ramp is
// positioned across the selection rather than driven by the filter panel.
class FadeCommand : public TrackEditCommand {
public:
    FadeCommand(Project& project, int trackIndex, int64_t startFrame, int64_t endFrame,
                 FadeShape shape, int channels);

protected:
    void apply() override;

private:
    int64_t startFrame_;
    int64_t endFrame_;
    FadeShape shape_;
    int channels_;
};

// Crossfades a clip into the one after it. The pair merges into a single
// clip, so the track shortens by the crossfade length and later clips ripple
// left -- see Project::crossfadeClips for why merging beats overlapping.
class CrossfadeCommand : public TrackEditCommand {
public:
    CrossfadeCommand(Project& project, int trackIndex, int firstClipIndex, int64_t frames, int channels);

protected:
    void apply() override;

private:
    int firstClipIndex_;
    int64_t frames_;
    int channels_;
};

// Inserts audio at a given frame, rippling later clips right. Used for
// pasting the clipboard.
class PasteCommand : public TrackEditCommand {
public:
    PasteCommand(Project& project, int trackIndex, int64_t atFrame, std::vector<float> samples, int channels);

protected:
    void apply() override;

private:
    int64_t atFrame_;
    SampleBuffer samples_;
    int channels_;
};

// Appends audio as a new clip at the track's current end. Used both to
// commit a just-finished recording take and to import an audio file.
class AppendClipCommand : public TrackEditCommand {
public:
    AppendClipCommand(Project& project, int trackIndex, std::vector<float> samples, int channels, const QString& text);

protected:
    void apply() override;

private:
    SampleBuffer samples_;
    int channels_;
};

// Moves one clip along its track's timeline and/or to another track. Spans
// two tracks, so it can't reuse TrackEditCommand's single-track snapshot.
class MoveClipCommand : public QUndoCommand {
public:
    MoveClipCommand(Project& project, int fromTrack, int clipIndex, int toTrack, int64_t newStartFrame);

    void undo() override;
    void redo() override;

private:
    Project& project_;
    int fromTrack_;
    int clipIndex_;
    int toTrack_;
    int64_t newStartFrame_;
    int64_t origStartFrame_ = 0;
    int insertedIndex_ = -1; // where the clip landed, so undo can find it again
};

// Moves any number of clips at once, possibly between tracks. Snapshots only
// the tracks the move touches: rebuilding indices as clips are pulled out and
// reinserted is fiddly enough that restoring a copy is both simpler and safer,
// and the cost stays bounded by the tracks actually involved.
// Envelope edits snapshot just the one track's point list -- it's a handful of
// values, so restoring a copy is cheaper than reasoning about how an index
// shifts when a point is inserted or removed.
class EnvelopeEditCommand : public QUndoCommand {
public:
    EnvelopeEditCommand(Project& project, int trackIndex, std::vector<EnvelopePoint> after,
                         const QString& text);

    void undo() override;
    void redo() override;

private:
    Project& project_;
    int trackIndex_;
    std::vector<EnvelopePoint> before_;
    std::vector<EnvelopePoint> after_;
    bool captured_ = false;
};

class MoveClipsCommand : public QUndoCommand {
public:
    MoveClipsCommand(Project& project, std::vector<ClipMove> moves);

    void undo() override;
    void redo() override;

private:
    void restore(const std::vector<Track>& snapshot);

    Project& project_;
    std::vector<ClipMove> moves_;
    std::vector<int> affectedTracks_;
    std::vector<Track> before_;
    std::optional<std::vector<Track>> after_;
};

class AddLabelCommand : public QUndoCommand {
public:
    AddLabelCommand(Project& project, Label label);

    void undo() override;
    void redo() override;

private:
    Project& project_;
    Label label_;
    int insertedIndex_ = -1;
};

class RemoveLabelCommand : public QUndoCommand {
public:
    RemoveLabelCommand(Project& project, int index);

    void undo() override;
    void redo() override;

private:
    Project& project_;
    int index_;
    Label removed_;
};

class RenameLabelCommand : public QUndoCommand {
public:
    RenameLabelCommand(Project& project, int index, std::string text);

    void undo() override;
    void redo() override;

private:
    Project& project_;
    int index_;
    std::string newText_;
    std::string oldText_;
};

class AddTrackCommand : public QUndoCommand {
public:
    AddTrackCommand(Project& project, std::string name);

    void undo() override;
    void redo() override;

private:
    Project& project_;
    std::string name_;
};

class RemoveTrackCommand : public QUndoCommand {
public:
    RemoveTrackCommand(Project& project, int trackIndex);

    void undo() override;
    void redo() override;

private:
    Project& project_;
    int trackIndex_;
    Track removed_;
};

} // namespace zrecord
