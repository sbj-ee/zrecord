#include "Commands.h"

#include <algorithm>

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

ApplyEffectCommand::ApplyEffectCommand(Project& project, int trackIndex, int64_t startFrame, int64_t endFrame,
                                        const FilterSettings& settings, double sampleRate, int channels)
    : TrackEditCommand(project, trackIndex, "Apply Effect"),
      startFrame_(startFrame),
      endFrame_(endFrame),
      settings_(settings),
      sampleRate_(sampleRate),
      channels_(channels) {}

void ApplyEffectCommand::apply() {
    std::vector<float> region = Project::copyRange(track(), startFrame_, endFrame_, channels_);
    if (region.empty()) {
        return;
    }
    FilterChain chain;
    chain.prepare(sampleRate_, channels_);
    chain.setSettings(settings_);
    int64_t frameCount = channels_ > 0 ? static_cast<int64_t>(region.size()) / channels_ : 0;
    chain.process(region, static_cast<size_t>(frameCount));
    Project::writeRange(track(), startFrame_, endFrame_, region, channels_);
}

FadeCommand::FadeCommand(Project& project, int trackIndex, int64_t startFrame, int64_t endFrame,
                          FadeShape shape, int channels)
    : TrackEditCommand(project, trackIndex, shape == FadeShape::In ? "Fade In" : "Fade Out"),
      startFrame_(startFrame),
      endFrame_(endFrame),
      shape_(shape),
      channels_(channels) {}

void FadeCommand::apply() {
    std::vector<float> region = Project::copyRange(track(), startFrame_, endFrame_, channels_);
    if (region.empty()) {
        return;
    }
    applyLinearFade(region, channels_, shape_);
    Project::writeRange(track(), startFrame_, endFrame_, region, channels_);
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

namespace {
// Inserts `clip` into `track` keeping clips sorted by startFrame, and returns
// the index it landed at.
int insertClipSorted(Track& track, Clip clip) {
    auto pos = std::lower_bound(track.clips.begin(), track.clips.end(), clip.startFrame,
                                 [](const Clip& c, int64_t f) { return c.startFrame < f; });
    int index = static_cast<int>(pos - track.clips.begin());
    track.clips.insert(pos, std::move(clip));
    return index;
}
} // namespace

MoveClipCommand::MoveClipCommand(Project& project, int fromTrack, int clipIndex, int toTrack, int64_t newStartFrame)
    : QUndoCommand("Move Clip"),
      project_(project),
      fromTrack_(fromTrack),
      clipIndex_(clipIndex),
      toTrack_(toTrack),
      newStartFrame_(newStartFrame) {}

void MoveClipCommand::redo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    Track& from = project_.tracks[static_cast<size_t>(fromTrack_)];
    Clip clip = std::move(from.clips[static_cast<size_t>(clipIndex_)]);
    from.clips.erase(from.clips.begin() + clipIndex_);

    origStartFrame_ = clip.startFrame;
    clip.startFrame = newStartFrame_;
    insertedIndex_ = insertClipSorted(project_.tracks[static_cast<size_t>(toTrack_)], std::move(clip));

    project_.selection.clear();
}

void MoveClipCommand::undo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    Track& to = project_.tracks[static_cast<size_t>(toTrack_)];
    Clip clip = std::move(to.clips[static_cast<size_t>(insertedIndex_)]);
    to.clips.erase(to.clips.begin() + insertedIndex_);

    clip.startFrame = origStartFrame_;
    insertClipSorted(project_.tracks[static_cast<size_t>(fromTrack_)], std::move(clip));

    project_.selection.clear();
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
