#include "PlaybackMixer.h"

#include <algorithm>

namespace zrecord {

std::shared_ptr<const PlaybackSnapshot> PlaybackSnapshot::capture(const Project& project) {
    auto snapshot = std::make_shared<PlaybackSnapshot>();
    std::lock_guard<std::mutex> lock(project.mutex);
    snapshot->tracks = project.tracks; // clip headers only; audio is shared
    snapshot->channels = project.channels;
    for (const auto& track : snapshot->tracks) {
        snapshot->lengthFrames = std::max(snapshot->lengthFrames, track.endFrame());
    }
    return snapshot;
}

void PlaybackMixer::publish(std::shared_ptr<const PlaybackSnapshot> snapshot) {
    live_.store(snapshot.get());
    // Read after the store: if render() isn't running now, any later call
    // loads the new pointer, so the old one is immediately unreachable.
    uint64_t seq = renderSeq_.load();
    if (current_) {
        retired_.emplace_back(std::move(current_), seq);
    }
    current_ = std::move(snapshot);
    reclaim();
}

void PlaybackMixer::reclaim() {
    const uint64_t now = renderSeq_.load();
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [now](const auto& entry) {
                                      bool wasIdle = (entry.second & 1u) == 0;
                                      return wasIdle || now != entry.second;
                                  }),
                   retired_.end());
}

void PlaybackMixer::clear() {
    live_.store(nullptr);
    retired_.clear();
    current_.reset();
}

bool PlaybackMixer::render(int64_t position, float* out, size_t frames, int outChannels) {
    renderSeq_.fetch_add(1); // odd: in use
    const PlaybackSnapshot* snapshot = live_.load();
    bool more = false;
    if (snapshot == nullptr || snapshot->channels != outChannels) {
        std::fill(out, out + frames * static_cast<size_t>(std::max(outChannels, 0)), 0.0f);
    } else {
        Project::mixTracks(snapshot->tracks, snapshot->channels, position, static_cast<int64_t>(frames), out);
        more = position + static_cast<int64_t>(frames) < snapshot->lengthFrames;
    }
    renderSeq_.fetch_add(1); // even: done with `snapshot`
    return more;
}

} // namespace zrecord
