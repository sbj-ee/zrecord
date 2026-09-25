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
        playheadBefore_ = project_.playheadFrame;
        apply();
        afterSnapshot_ = track();
        selectionAfter_ = project_.selection;
        playheadAfter_ = project_.playheadFrame;
    } else {
        track() = *afterSnapshot_;
        project_.selection = selectionAfter_;
        project_.playheadFrame = playheadAfter_;
    }
}

void TrackEditCommand::undo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    track() = beforeSnapshot_;
    project_.selection = selectionBefore_;
    // Several edits move the playhead (delete pulls it to the cut, paste and
    // record push it past the new audio). Undo has to put it back, or the
    // cursor ends up pointing somewhere the audio no longer justifies.
    project_.playheadFrame = playheadBefore_;
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

CrossfadeCommand::CrossfadeCommand(Project& project, int trackIndex, int firstClipIndex,
                                    int64_t frames, int channels)
    : TrackEditCommand(project, trackIndex, "Crossfade"),
      firstClipIndex_(firstClipIndex),
      frames_(frames),
      channels_(channels) {}

void CrossfadeCommand::apply() {
    Project::crossfadeClips(track(), firstClipIndex_, frames_, channels_);
    project().selection.clear();
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
    // Park the playhead at the start of what was just added, so Play
    // auditions it immediately. Leaving it at the end would make Play look
    // broken, and buys nothing: appendClip always places a take at the
    // track's end regardless of where the playhead sits.
    int64_t appendedAt = track().endFrame();
    Project::appendClip(track(), samples_, channels_);
    project().playheadFrame = appendedAt;
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

EnvelopeEditCommand::EnvelopeEditCommand(Project& project, int trackIndex, std::vector<EnvelopePoint> before,
                                          std::vector<EnvelopePoint> after, const QString& text)
    : QUndoCommand(text),
      project_(project),
      trackIndex_(trackIndex),
      before_(std::move(before)),
      after_(std::move(after)) {}

void EnvelopeEditCommand::redo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    project_.tracks[static_cast<size_t>(trackIndex_)].envelope = after_;
}

void EnvelopeEditCommand::undo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    project_.tracks[static_cast<size_t>(trackIndex_)].envelope = before_;
}

MoveClipsCommand::MoveClipsCommand(Project& project, std::vector<ClipMove> moves)
    : QUndoCommand(moves.size() > 1 ? "Move Clips" : "Move Clip"),
      project_(project),
      moves_(std::move(moves)) {
    for (const ClipMove& move : moves_) {
        for (int track : {move.fromTrack, move.toTrack}) {
            if (std::find(affectedTracks_.begin(), affectedTracks_.end(), track) == affectedTracks_.end()) {
                affectedTracks_.push_back(track);
            }
        }
    }
    std::sort(affectedTracks_.begin(), affectedTracks_.end());
}

void MoveClipsCommand::restore(const std::vector<Track>& snapshot) {
    for (size_t i = 0; i < affectedTracks_.size(); ++i) {
        project_.tracks[static_cast<size_t>(affectedTracks_[i])] = snapshot[i];
    }
}

void MoveClipsCommand::redo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    if (after_.has_value()) {
        restore(*after_);
        project_.selection.clear();
        return;
    }

    before_.clear();
    for (int track : affectedTracks_) {
        before_.push_back(project_.tracks[static_cast<size_t>(track)]);
    }

    // Lift every clip out first, removing from the back of each track so the
    // indices in `moves_` stay valid while we work.
    std::vector<ClipMove> ordered = moves_;
    std::sort(ordered.begin(), ordered.end(), [](const ClipMove& a, const ClipMove& b) {
        if (a.fromTrack != b.fromTrack) return a.fromTrack > b.fromTrack;
        return a.clipIndex > b.clipIndex;
    });

    std::vector<std::pair<Clip, ClipMove>> lifted;
    lifted.reserve(ordered.size());
    for (const ClipMove& move : ordered) {
        Track& from = project_.tracks[static_cast<size_t>(move.fromTrack)];
        Clip clip = std::move(from.clips[static_cast<size_t>(move.clipIndex)]);
        from.clips.erase(from.clips.begin() + move.clipIndex);
        lifted.emplace_back(std::move(clip), move);
    }

    for (auto& entry : lifted) {
        entry.first.startFrame = entry.second.newStartFrame;
        Track& to = project_.tracks[static_cast<size_t>(entry.second.toTrack)];
        auto pos = std::lower_bound(to.clips.begin(), to.clips.end(), entry.first.startFrame,
                                     [](const Clip& c, int64_t f) { return c.startFrame < f; });
        to.clips.insert(pos, std::move(entry.first));
    }

    after_.emplace();
    for (int track : affectedTracks_) {
        after_->push_back(project_.tracks[static_cast<size_t>(track)]);
    }
    project_.selection.clear();
}

void MoveClipsCommand::undo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    restore(before_);
    project_.selection.clear();
}

AddLabelCommand::AddLabelCommand(Project& project, Label label)
    : QUndoCommand("Add Label"), project_(project), label_(std::move(label)) {}

void AddLabelCommand::redo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    insertedIndex_ = project_.insertLabel(label_);
}

void AddLabelCommand::undo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    project_.labels.erase(project_.labels.begin() + insertedIndex_);
}

RemoveLabelCommand::RemoveLabelCommand(Project& project, int index)
    : QUndoCommand("Remove Label"), project_(project), index_(index) {}

void RemoveLabelCommand::redo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    removed_ = project_.labels[static_cast<size_t>(index_)];
    project_.labels.erase(project_.labels.begin() + index_);
}

void RemoveLabelCommand::undo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    project_.labels.insert(project_.labels.begin() + index_, removed_);
}

RenameLabelCommand::RenameLabelCommand(Project& project, int index, std::string text)
    : QUndoCommand("Rename Label"), project_(project), index_(index), newText_(std::move(text)) {}

void RenameLabelCommand::redo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    oldText_ = project_.labels[static_cast<size_t>(index_)].text;
    project_.labels[static_cast<size_t>(index_)].text = newText_;
}

void RenameLabelCommand::undo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    project_.labels[static_cast<size_t>(index_)].text = oldText_;
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

TargetsEditCommand::TargetsEditCommand(Project& project, std::vector<GainTarget> targets, const QString& text)
    : QUndoCommand(text), project_(project), targets_(std::move(targets)) {}

void TargetsEditCommand::redo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    if (!applied_) {
        applied_ = true;
        for (const GainTarget& target : targets_) {
            if (target.trackIndex >= 0 && target.trackIndex < static_cast<int>(project_.tracks.size())) {
                before_.emplace(target.trackIndex, project_.tracks[static_cast<size_t>(target.trackIndex)]);
            }
        }
        apply(project_, targets_);
        for (const auto& entry : before_) {
            after_.emplace(entry.first, project_.tracks[static_cast<size_t>(entry.first)]);
        }
    } else {
        for (const auto& entry : after_) {
            project_.tracks[static_cast<size_t>(entry.first)] = entry.second;
        }
    }
}

void TargetsEditCommand::undo() {
    std::lock_guard<std::mutex> lock(project_.mutex);
    for (const auto& entry : before_) {
        project_.tracks[static_cast<size_t>(entry.first)] = entry.second;
    }
}

} // namespace zrecord
