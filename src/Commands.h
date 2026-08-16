#pragma once

#include <QUndoCommand>
#include <optional>

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

// Inserts audio at a given frame, rippling later clips right. Used for
// pasting the clipboard.
class PasteCommand : public TrackEditCommand {
public:
    PasteCommand(Project& project, int trackIndex, int64_t atFrame, std::vector<float> samples, int channels);

protected:
    void apply() override;

private:
    int64_t atFrame_;
    std::vector<float> samples_;
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
    std::vector<float> samples_;
    int channels_;
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
