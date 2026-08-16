#include "Commands.h"

namespace zrecord {

TrackEditCommand::TrackEditCommand(Project& project, int trackIndex, const QString& text)
    : QUndoCommand(text), project_(project), trackIndex_(trackIndex) {}

void TrackEditCommand::redo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    if (!afterSnapshot_.has_value()) {
        beforeSnapshot_ = track();
        selectionBefore_ = project_.selection;
        apply();
        afterSnapshot_ = track();
        selectionAfter_ = project_.selection;
    } else {
        track() = *afterSnapshot_;
        project_.selection = selectionAfter_;
    }
}

void TrackEditCommand::undo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    track() = beforeSnapshot_;
    project_.selection = selectionBefore_;
}

DeleteSelectionCommand::DeleteSelectionCommand(Project& project, int trackIndex, int64_t startFrame, int64_t endFrame)
    : TrackEditCommand(project, trackIndex, "Delete"), startFrame_(startFrame), endFrame_(endFrame) {}

void DeleteSelectionCommand::apply() {
    Project::removeRange(track(), startFrame_, endFrame_, project().channels);
    project().selection.clear();
    project().playheadFrame = startFrame_;
}

SilenceSelectionCommand::SilenceSelectionCommand(Project& project, int trackIndex, int64_t startFrame, int64_t endFrame)
    : TrackEditCommand(project, trackIndex, "Silence"), startFrame_(startFrame), endFrame_(endFrame) {}

void SilenceSelectionCommand::apply() {
    Project::silenceRange(track(), startFrame_, endFrame_, project().channels);
}

PasteCommand::PasteCommand(Project& project, int trackIndex, int64_t atFrame, std::vector<float> samples, int channels)
    : TrackEditCommand(project, trackIndex, "Paste"), atFrame_(atFrame), samples_(std::move(samples)), channels_(channels) {}

void PasteCommand::apply() {
    Project::insertRange(track(), atFrame_, samples_, channels_);
    int64_t frameCount = channels_ > 0 ? static_cast<int64_t>(samples_.size()) / channels_ : 0;
    project().playheadFrame = atFrame_ + frameCount;
}

AppendClipCommand::AppendClipCommand(Project& project, int trackIndex, std::vector<float> samples, int channels, const QString& text)
    : TrackEditCommand(project, trackIndex, text), samples_(std::move(samples)), channels_(channels) {}

void AppendClipCommand::apply() {
    Project::appendClip(track(), samples_, channels_);
    project().playheadFrame = track().endFrame();
}

AddTrackCommand::AddTrackCommand(Project& project, std::string name)
    : QUndoCommand("Add Track"), project_(project), name_(std::move(name)) {}

void AddTrackCommand::redo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    Track t;
    t.name = name_;
    project_.tracks.push_back(std::move(t));
}

void AddTrackCommand::undo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    if (!project_.tracks.empty()) {
        project_.tracks.pop_back();
    }
}

RemoveTrackCommand::RemoveTrackCommand(Project& project, int trackIndex)
    : QUndoCommand("Remove Track"), project_(project), trackIndex_(trackIndex) {}

void RemoveTrackCommand::redo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    removed_ = project_.tracks[static_cast<size_t>(trackIndex_)];
    project_.tracks.erase(project_.tracks.begin() + trackIndex_);
    if (project_.selection.trackIndex == trackIndex_) {
        project_.selection.clear();
    }
}

void RemoveTrackCommand::undo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    project_.tracks.insert(project_.tracks.begin() + trackIndex_, removed_);
}

} // namespace zrecord
