#include "Gain.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace zrecord {

namespace {

// Clips overlapping `target`, with the overlap in clip-local samples.
template <typename ClipT, typename Fn>
void forEachOverlap(ClipT& clip, const GainTarget& target, Fn&& fn) {
    const int64_t overlapStart = std::max(clip.startFrame, target.startFrame);
    const int64_t overlapEnd = std::min(clip.endFrame(), target.endFrame);
    if (overlapStart >= overlapEnd || clip.channels <= 0) {
        return;
    }
    const size_t first = static_cast<size_t>(overlapStart - clip.startFrame) * static_cast<size_t>(clip.channels);
    const size_t count = static_cast<size_t>(overlapEnd - overlapStart) * static_cast<size_t>(clip.channels);
    fn(first, count);
}

constexpr size_t kBlock = 64 * 1024;

} // namespace

float dbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

float linearToDb(float linear) {
    if (linear <= 0.0f) {
        return -std::numeric_limits<float>::infinity();
    }
    return 20.0f * std::log10(linear);
}

float measurePeak(const Project& project, const std::vector<GainTarget>& targets) {
    float peak = 0.0f;
    std::vector<float> block(kBlock);
    for (const GainTarget& target : targets) {
        if (target.trackIndex < 0 || target.trackIndex >= static_cast<int>(project.tracks.size())) {
            continue;
        }
        for (const Clip& clip : project.tracks[static_cast<size_t>(target.trackIndex)].clips) {
            forEachOverlap(clip, target, [&](size_t first, size_t count) {
                for (size_t done = 0; done < count; done += kBlock) {
                    const size_t n = std::min(kBlock, count - done);
                    clip.samples.copyTo(first + done, n, block.data());
                    for (size_t i = 0; i < n; ++i) {
                        peak = std::max(peak, std::fabs(block[i]));
                    }
                }
            });
        }
    }
    return peak;
}

void applyGain(Project& project, const std::vector<GainTarget>& targets, float gain) {
    std::vector<float> block(kBlock);
    for (const GainTarget& target : targets) {
        if (target.trackIndex < 0 || target.trackIndex >= static_cast<int>(project.tracks.size())) {
            continue;
        }
        for (Clip& clip : project.tracks[static_cast<size_t>(target.trackIndex)].clips) {
            bool touched = false;
            forEachOverlap(clip, target, [&](size_t first, size_t count) {
                for (size_t done = 0; done < count; done += kBlock) {
                    const size_t n = std::min(kBlock, count - done);
                    clip.samples.copyTo(first + done, n, block.data());
                    for (size_t i = 0; i < n; ++i) {
                        block[i] *= gain;
                    }
                    clip.samples.write(first + done, block.data(), n);
                }
                touched = true;
            });
            if (touched) {
                clip.peaks.build(clip.samples, clip.channels);
            }
        }
    }
}

GainPlan planNormalize(float currentPeak, float targetDb) {
    GainPlan plan;
    plan.currentPeak = currentPeak;
    if (currentPeak > 0.0f) {
        plan.gain = dbToLinear(targetDb) / currentPeak;
    }
    plan.resultingPeak = currentPeak * plan.gain;
    return plan;
}

GainPlan planAmplify(float currentPeak, float gainDb) {
    GainPlan plan;
    plan.currentPeak = currentPeak;
    plan.gain = dbToLinear(gainDb);
    plan.resultingPeak = currentPeak * plan.gain;
    return plan;
}

} // namespace zrecord
