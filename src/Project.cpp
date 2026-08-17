#include "Project.h"

#include "Filters.h"

#include <algorithm>
#include <cmath>

namespace zrecord {

void PeakCache::build(const std::vector<float>& samples, int channels) {
    blocks_.clear();
    built_ = false;
    if (channels <= 0 || samples.empty()) {
        built_ = true;
        return;
    }
    int64_t frameCount = static_cast<int64_t>(samples.size()) / channels;
    int64_t blockCount = (frameCount + kBlockFrames - 1) / kBlockFrames;
    blocks_.resize(static_cast<size_t>(blockCount));
    for (int64_t b = 0; b < blockCount; ++b) {
        int64_t startFrame = b * kBlockFrames;
        int64_t endFrame = std::min(startFrame + kBlockFrames, frameCount);
        float minValue = 0.0f;
        float maxValue = 0.0f;
        bool first = true;
        for (int64_t f = startFrame; f < endFrame; ++f) {
            for (int c = 0; c < channels; ++c) {
                float s = samples[static_cast<size_t>(f) * channels + c];
                if (first) {
                    minValue = maxValue = s;
                    first = false;
                } else {
                    minValue = std::min(minValue, s);
                    maxValue = std::max(maxValue, s);
                }
            }
        }
        blocks_[static_cast<size_t>(b)] = {minValue, maxValue};
    }
    built_ = true;
}

void PeakCache::invalidate() {
    blocks_.clear();
    built_ = false;
}

PeakCache::MinMax PeakCache::blockAt(int64_t blockIndex) const {
    if (blockIndex < 0 || blockIndex >= static_cast<int64_t>(blocks_.size())) {
        return {};
    }
    return blocks_[static_cast<size_t>(blockIndex)];
}

int64_t Track::endFrame() const {
    int64_t end = 0;
    for (const auto& clip : clips) {
        end = std::max(end, clip.endFrame());
    }
    return end;
}

namespace {
// Finds the (sorted) insertion point for a clip starting at `frame`.
std::vector<Clip>::iterator insertionPoint(std::vector<Clip>& clips, int64_t frame) {
    return std::lower_bound(clips.begin(), clips.end(), frame,
                             [](const Clip& c, int64_t f) { return c.startFrame < f; });
}
} // namespace

void Project::splitClipAt(Track& track, int64_t frame, int channels) {
    for (size_t i = 0; i < track.clips.size(); ++i) {
        Clip& clip = track.clips[i];
        if (frame > clip.startFrame && frame < clip.endFrame()) {
            int64_t splitOffsetFrames = frame - clip.startFrame;
            size_t splitSampleIndex = static_cast<size_t>(splitOffsetFrames) * static_cast<size_t>(channels);

            Clip second;
            second.channels = channels;
            second.startFrame = frame;
            second.samples.assign(clip.samples.begin() + static_cast<long>(splitSampleIndex), clip.samples.end());
            second.peaks.build(second.samples, channels);

            clip.samples.resize(splitSampleIndex);
            clip.peaks.build(clip.samples, channels);

            track.clips.insert(track.clips.begin() + static_cast<long>(i) + 1, std::move(second));
            return;
        }
    }
}

std::vector<float> Project::removeRange(Track& track, int64_t startFrame, int64_t endFrame, int channels) {
    if (endFrame <= startFrame) {
        return {};
    }
    splitClipAt(track, startFrame, channels);
    splitClipAt(track, endFrame, channels);

    int64_t removedFrameCount = endFrame - startFrame;
    std::vector<float> removed(static_cast<size_t>(removedFrameCount) * static_cast<size_t>(channels), 0.0f);

    std::vector<Clip> kept;
    kept.reserve(track.clips.size());
    for (auto& clip : track.clips) {
        if (clip.startFrame >= startFrame && clip.endFrame() <= endFrame) {
            int64_t destFrameOffset = clip.startFrame - startFrame;
            std::copy(clip.samples.begin(), clip.samples.end(),
                      removed.begin() + static_cast<long>(destFrameOffset) * channels);
            continue; // dropped: fully inside the removed range
        }
        if (clip.startFrame >= endFrame) {
            clip.startFrame -= removedFrameCount;
        }
        kept.push_back(std::move(clip));
    }
    track.clips = std::move(kept);
    return removed;
}

void Project::insertRange(Track& track, int64_t atFrame, const std::vector<float>& samples, int channels) {
    if (samples.empty() || channels <= 0) {
        return;
    }
    int64_t frameCount = static_cast<int64_t>(samples.size()) / channels;

    splitClipAt(track, atFrame, channels);
    for (auto& clip : track.clips) {
        if (clip.startFrame >= atFrame) {
            clip.startFrame += frameCount;
        }
    }

    Clip inserted;
    inserted.channels = channels;
    inserted.startFrame = atFrame;
    inserted.samples = samples;
    inserted.peaks.build(inserted.samples, channels);

    track.clips.insert(insertionPoint(track.clips, atFrame), std::move(inserted));
}

void Project::silenceRange(Track& track, int64_t startFrame, int64_t endFrame, int channels) {
    for (auto& clip : track.clips) {
        int64_t overlapStart = std::max(clip.startFrame, startFrame);
        int64_t overlapEnd = std::min(clip.endFrame(), endFrame);
        if (overlapStart >= overlapEnd) {
            continue;
        }
        int64_t localStart = overlapStart - clip.startFrame;
        int64_t localEnd = overlapEnd - clip.startFrame;
        std::fill(clip.samples.begin() + static_cast<long>(localStart) * channels,
                  clip.samples.begin() + static_cast<long>(localEnd) * channels, 0.0f);
        clip.peaks.build(clip.samples, channels);
    }
}

void Project::writeRange(Track& track, int64_t startFrame, int64_t endFrame, const std::vector<float>& samples, int channels) {
    for (auto& clip : track.clips) {
        int64_t overlapStart = std::max(clip.startFrame, startFrame);
        int64_t overlapEnd = std::min(clip.endFrame(), endFrame);
        if (overlapStart >= overlapEnd) {
            continue;
        }
        int64_t clipLocalStart = overlapStart - clip.startFrame;
        int64_t srcLocalStart = overlapStart - startFrame;
        int64_t frames = overlapEnd - overlapStart;
        std::copy(samples.begin() + static_cast<long>(srcLocalStart) * channels,
                  samples.begin() + static_cast<long>(srcLocalStart + frames) * channels,
                  clip.samples.begin() + static_cast<long>(clipLocalStart) * channels);
        clip.peaks.build(clip.samples, channels);
    }
}

std::vector<float> Project::copyRange(const Track& track, int64_t startFrame, int64_t endFrame, int channels) {
    if (endFrame <= startFrame || channels <= 0) {
        return {};
    }
    std::vector<float> out(static_cast<size_t>(endFrame - startFrame) * static_cast<size_t>(channels), 0.0f);
    for (const auto& clip : track.clips) {
        int64_t overlapStart = std::max(clip.startFrame, startFrame);
        int64_t overlapEnd = std::min(clip.endFrame(), endFrame);
        if (overlapStart >= overlapEnd) {
            continue;
        }
        int64_t clipLocalStart = overlapStart - clip.startFrame;
        int64_t outLocalStart = overlapStart - startFrame;
        int64_t frames = overlapEnd - overlapStart;
        std::copy(clip.samples.begin() + static_cast<long>(clipLocalStart) * channels,
                  clip.samples.begin() + static_cast<long>(clipLocalStart + frames) * channels,
                  out.begin() + static_cast<long>(outLocalStart) * channels);
    }
    return out;
}

void Project::appendClip(Track& track, const std::vector<float>& samples, int channels) {
    if (samples.empty() || channels <= 0) {
        return;
    }
    Clip clip;
    clip.channels = channels;
    clip.startFrame = track.endFrame();
    clip.samples = samples;
    clip.peaks.build(clip.samples, channels);
    track.clips.push_back(std::move(clip));
}

bool Project::crossfadeClips(Track& track, int firstClipIndex, int64_t frames, int channels) {
    if (channels <= 0 || frames <= 0 || firstClipIndex < 0) {
        return false;
    }
    if (static_cast<size_t>(firstClipIndex) + 1 >= track.clips.size()) {
        return false;
    }

    Clip& a = track.clips[static_cast<size_t>(firstClipIndex)];
    Clip& b = track.clips[static_cast<size_t>(firstClipIndex) + 1];
    if (a.endFrame() != b.startFrame) {
        return false; // a gap between them makes the overlap ambiguous
    }
    int64_t aLen = a.frameCount();
    int64_t bLen = b.frameCount();
    if (aLen < frames || bLen < frames) {
        return false;
    }

    const size_t ch = static_cast<size_t>(channels);
    Clip merged;
    merged.channels = channels;
    merged.startFrame = a.startFrame;
    merged.samples.resize(static_cast<size_t>(aLen + bLen - frames) * ch);

    // Everything of A before the overlap.
    std::copy(a.samples.begin(), a.samples.begin() + static_cast<long>(aLen - frames) * channels,
              merged.samples.begin());

    // The overlap: A's tail faded out against B's head faded in.
    std::vector<float> overlap(a.samples.begin() + static_cast<long>(aLen - frames) * channels,
                                a.samples.end());
    std::vector<float> incoming(b.samples.begin(),
                                 b.samples.begin() + static_cast<long>(frames) * channels);
    mixEqualPowerCrossfade(overlap, incoming, channels);
    std::copy(overlap.begin(), overlap.end(),
              merged.samples.begin() + static_cast<long>(aLen - frames) * channels);

    // Whatever of B is left after the overlap.
    std::copy(b.samples.begin() + static_cast<long>(frames) * channels, b.samples.end(),
              merged.samples.begin() + static_cast<long>(aLen) * channels);

    merged.peaks.build(merged.samples, channels);

    track.clips.erase(track.clips.begin() + firstClipIndex,
                      track.clips.begin() + firstClipIndex + 2);
    track.clips.insert(track.clips.begin() + firstClipIndex, std::move(merged));

    // The pair now occupies `frames` fewer frames, so everything after shifts.
    for (size_t i = static_cast<size_t>(firstClipIndex) + 1; i < track.clips.size(); ++i) {
        track.clips[i].startFrame -= frames;
    }
    return true;
}

int Project::insertLabel(const Label& label) {
    auto pos = std::lower_bound(labels.begin(), labels.end(), label.startFrame,
                                 [](const Label& l, int64_t f) { return l.startFrame < f; });
    int index = static_cast<int>(pos - labels.begin());
    labels.insert(pos, label);
    return index;
}

void Project::reset() {
    std::lock_guard<std::mutex> lock(mutex);
    sampleRate = 44100.0;
    channels = 2;
    tracks.clear();
    labels.clear();
    selection.clear();
    playheadFrame = 0;
    clipboard.clear();
}

int64_t Project::lengthFrames() const {
    std::lock_guard<std::mutex> lock(mutex);
    int64_t end = 0;
    for (const auto& track : tracks) {
        end = std::max(end, track.endFrame());
    }
    return end;
}

void Project::readMix(int64_t startFrame, int64_t frameCount, std::vector<float>& out) const {
    std::fill(out.begin(), out.end(), 0.0f);
    if (channels <= 0 || frameCount <= 0) {
        return;
    }

    bool anySolo = std::any_of(tracks.begin(), tracks.end(), [](const Track& t) { return t.soloed; });

    for (const auto& track : tracks) {
        bool audible = anySolo ? track.soloed : !track.muted;
        if (!audible) {
            continue;
        }
        float gain = static_cast<float>(std::pow(10.0, track.gainDb / 20.0));

        for (const auto& clip : track.clips) {
            int64_t overlapStart = std::max(clip.startFrame, startFrame);
            int64_t overlapEnd = std::min(clip.endFrame(), startFrame + frameCount);
            if (overlapStart >= overlapEnd) {
                continue;
            }
            int64_t clipLocalStart = overlapStart - clip.startFrame;
            int64_t outLocalStart = overlapStart - startFrame;
            int64_t overlapFrames = overlapEnd - overlapStart;

            for (int64_t f = 0; f < overlapFrames; ++f) {
                for (int c = 0; c < channels; ++c) {
                    size_t srcIndex = static_cast<size_t>(clipLocalStart + f) * channels + c;
                    size_t dstIndex = static_cast<size_t>(outLocalStart + f) * channels + c;
                    if (srcIndex < clip.samples.size() && dstIndex < out.size()) {
                        out[dstIndex] += clip.samples[srcIndex] * gain;
                    }
                }
            }
        }
    }

    for (float& sample : out) {
        sample = std::clamp(sample, -1.0f, 1.0f);
    }
}

std::vector<float> Project::renderMixdown() const {
    int64_t length = lengthFrames();
    std::vector<float> out(static_cast<size_t>(length) * static_cast<size_t>(channels), 0.0f);
    {
        std::lock_guard<std::mutex> lock(mutex);
        readMix(0, length, out);
    }
    return out;
}

} // namespace zrecord
