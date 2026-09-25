#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "Project.h"

namespace zrecord {

// An immutable copy of what playback needs from a Project. Copying tracks is
// cheap -- clip audio is shared (see SampleBuffer) -- so the UI thread can
// take a fresh one whenever the project may have changed.
struct PlaybackSnapshot {
    std::vector<Track> tracks;
    int channels = 1;
    int64_t lengthFrames = 0;

    // Takes project.mutex briefly (UI thread).
    static std::shared_ptr<const PlaybackSnapshot> capture(const Project& project);
};

// Hands PlaybackSnapshots from the UI thread to the audio callback without
// locks. The callback reads the current snapshot through an atomic pointer;
// replaced snapshots are kept alive until the callback is known to be done
// with them, then freed on the UI thread, so the audio thread never blocks,
// allocates or frees.
class PlaybackMixer {
public:
    PlaybackMixer() = default;
    PlaybackMixer(const PlaybackMixer&) = delete;
    PlaybackMixer& operator=(const PlaybackMixer&) = delete;

    // UI thread.
    void publish(std::shared_ptr<const PlaybackSnapshot> snapshot);
    void reclaim();   // frees replaced snapshots the callback can no longer see
    void clear();     // drops everything; only once the callback has stopped
    size_t retiredCount() const { return retired_.size(); }

    // Audio thread: real-time safe. Mixes `frames` frames starting at
    // `position` into `out` (`outChannels` interleaved), overwriting it.
    // Returns false once `position + frames` reaches the end (or when there is
    // nothing to play), which is the caller's cue to finish.
    bool render(int64_t position, float* out, size_t frames, int outChannels);

private:
    std::atomic<const PlaybackSnapshot*> live_{nullptr};
    // Odd while render() is running. Lets the UI thread tell whether the
    // callback might still hold a pointer it has since replaced.
    std::atomic<uint64_t> renderSeq_{0};

    std::shared_ptr<const PlaybackSnapshot> current_; // UI thread
    std::vector<std::pair<std::shared_ptr<const PlaybackSnapshot>, uint64_t>> retired_; // UI thread
};

} // namespace zrecord
