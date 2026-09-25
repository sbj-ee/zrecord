#pragma once

#include <vector>

#include "Project.h"

namespace zrecord {

// A span of one track that a gain change applies to: a time selection, or a
// whole clip (startFrame..endFrame of that clip).
struct GainTarget {
    int trackIndex = -1;
    int64_t startFrame = 0;
    int64_t endFrame = 0;
};

float dbToLinear(float db);
// -inf for silence.
float linearToDb(float linear);

// The largest |sample| inside the targets (0 for silence or nothing).
float measurePeak(const Project& project, const std::vector<GainTarget>& targets);

// Multiplies every sample inside the targets by `gain`, clip by clip, in
// place (copy-on-write, so only touched chunks are duplicated). Peak caches
// of touched clips are rebuilt.
void applyGain(Project& project, const std::vector<GainTarget>& targets, float gain);

// What a Normalize or Amplify would do, worked out before doing it, so the
// dialog can show the result and warn when it would clip.
struct GainPlan {
    float currentPeak = 0.0f;   // linear
    float gain = 1.0f;          // linear factor to apply
    float resultingPeak = 0.0f; // linear
    // True when the result exceeds full scale. Clips store float, so nothing
    // is lost in the project itself, but the exported mixdown clamps there.
    bool clips() const { return resultingPeak > 1.0f + 1e-6f; }
};

// Scale so the loudest sample lands on `targetDb` dBFS. Silence can't be
// normalized: the plan keeps a gain of 1.
GainPlan planNormalize(float currentPeak, float targetDb);
// Scale by `gainDb`.
GainPlan planAmplify(float currentPeak, float gainDb);

} // namespace zrecord
