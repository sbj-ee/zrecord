#include "Snap.h"

#include <cstdlib>

namespace zrecord {

std::vector<int64_t> collectSnapTargets(const Project& project, const Clip* excludeClip) {
    std::vector<int64_t> targets;
    targets.push_back(0);
    targets.push_back(project.playheadFrame);
    for (const auto& track : project.tracks) {
        for (const auto& clip : track.clips) {
            if (&clip == excludeClip) {
                continue;
            }
            targets.push_back(clip.startFrame);
            targets.push_back(clip.endFrame());
        }
    }
    return targets;
}

SnapResult snapToTargets(int64_t rawStart, int64_t clipLength,
                          const std::vector<int64_t>& targets, int64_t thresholdFrames) {
    SnapResult result;
    result.startFrame = rawStart;
    if (thresholdFrames <= 0) {
        return result;
    }

    int64_t bestDistance = thresholdFrames + 1;
    for (int64_t target : targets) {
        int64_t startDistance = std::abs(rawStart - target);
        if (startDistance < bestDistance && target >= 0) {
            bestDistance = startDistance;
            result.startFrame = target;
            result.targetFrame = target;
            result.snapped = true;
        }
        int64_t endDistance = std::abs(rawStart + clipLength - target);
        if (endDistance < bestDistance && target - clipLength >= 0) {
            bestDistance = endDistance;
            result.startFrame = target - clipLength;
            result.targetFrame = target;
            result.snapped = true;
        }
    }

    if (bestDistance > thresholdFrames) {
        return SnapResult{rawStart, false, 0};
    }
    return result;
}

} // namespace zrecord
