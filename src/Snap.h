#pragma once

#include <cstdint>
#include <vector>

#include "Project.h"

namespace zrecord {

struct SnapResult {
    int64_t startFrame = 0;  // where the clip should start
    bool snapped = false;    // false when nothing was near enough
    int64_t targetFrame = 0; // what it latched onto, for drawing a guide
};

// Every position a dragged clip can latch onto: zero, the playhead, and both
// edges of every clip on every track except `excludeClip` (the one being
// dragged). Clip edges come from all tracks deliberately -- aligning against a
// clip on another track is the main reason to want snapping here.
//
// The caller is responsible for holding `project.mutex` if it needs to.
std::vector<int64_t> collectSnapTargets(const Project& project, const Clip* excludeClip);

// Snaps a clip of `clipLength` frames whose left edge is at `rawStart`.
// Both edges are tried against every target and the closest wins, which is
// what makes butting a clip against its neighbour's end feel the same as
// aligning their left edges. Snapping is skipped if nothing lies within
// `thresholdFrames`, or if it would push the clip before zero.
SnapResult snapToTargets(int64_t rawStart, int64_t clipLength,
                          const std::vector<int64_t>& targets, int64_t thresholdFrames);

} // namespace zrecord
