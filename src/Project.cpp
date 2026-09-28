#include "Project.h"

#include "Capture.h"
#include "Filters.h"

#include <algorithm>
#include <cmath>

namespace zrecord {

void PeakCache::build(const SampleBuffer& samples, int channels) {
    blocks_.reset();
    built_ = false;
    if (channels <= 0 || samples.empty()) {
        built_ = true;
        return;
    }
    int64_t frameCount = static_cast<int64_t>(samples.size()) / channels;
    int64_t blockCount = (frameCount + kBlockFrames - 1) / kBlockFrames;
    auto blocks = std::make_shared<std::vector<MinMax>>(static_cast<size_t>(blockCount));
    std::vector<float> block(static_cast<size_t>(kBlockFrames) * static_cast<size_t>(channels));
    // Per channel: the current run of full-scale samples and where it began,
    // carried from block to block.
    std::vector<int64_t> run(static_cast<size_t>(channels), 0);
    std::vector<int64_t> runStart(static_cast<size_t>(channels), 0);
    for (int64_t b = 0; b < blockCount; ++b) {
        int64_t startFrame = b * kBlockFrames;
        int64_t endFrame = std::min(startFrame + kBlockFrames, frameCount);
        samples.copyTo(static_cast<size_t>(startFrame) * channels,
                       static_cast<size_t>(endFrame - startFrame) * channels, block.data());
        float minValue = 0.0f;
        float maxValue = 0.0f;
        bool first = true;
        for (int64_t f = 0; f < endFrame - startFrame; ++f) {
            for (int c = 0; c < channels; ++c) {
                float s = block[static_cast<size_t>(f) * channels + c];
                int64_t& r = run[static_cast<size_t>(c)];
                if (!isFullScale(s)) {
                    r = 0;
                } else if (++r == 1) {
                    runStart[static_cast<size_t>(c)] = startFrame + f;
                } else if (r == kClipRunLength) {
                    // Now known to be a clip: flag back to where the run began,
                    // which may be in an earlier block.
                    for (int64_t k = runStart[static_cast<size_t>(c)] / kBlockFrames; k <= b; ++k) {
                        (*blocks)[static_cast<size_t>(k)].clipped = true;
                    }
                } else if (r > kClipRunLength) {
                    (*blocks)[static_cast<size_t>(b)].clipped = true;
                }
                if (first) {
                    minValue = maxValue = s;
                    first = false;
                } else {
                    minValue = std::min(minValue, s);
                    maxValue = std::max(maxValue, s);
                }
            }
        }
        (*blocks)[static_cast<size_t>(b)].minValue = minValue;
        (*blocks)[static_cast<size_t>(b)].maxValue = maxValue;
    }
    blocks_ = std::move(blocks);
    built_ = true;
}

void PeakCache::invalidate() {
    blocks_.reset();
    built_ = false;
}

PeakCache::MinMax PeakCache::blockAt(int64_t blockIndex) const {
    if (!blocks_ || blockIndex < 0 || blockIndex >= static_cast<int64_t>(blocks_->size())) {
        return {};
    }
    return (*blocks_)[static_cast<size_t>(blockIndex)];
}

bool isInClipRun(const SampleBuffer& samples, int channels, int64_t frame, int channel) {
    if (channels <= 0 || channel < 0 || channel >= channels || frame < 0) {
        return false;
    }
    const int64_t frames = static_cast<int64_t>(samples.size()) / channels;
    auto fullScaleAt = [&](int64_t f) {
        return f >= 0 && f < frames &&
               isFullScale(samples[static_cast<size_t>(f) * static_cast<size_t>(channels) + static_cast<size_t>(channel)]);
    };
    if (!fullScaleAt(frame)) {
        return false;
    }
    int64_t length = 1;
    for (int64_t f = frame - 1; length < kClipRunLength && fullScaleAt(f); --f) ++length;
    for (int64_t f = frame + 1; length < kClipRunLength && fullScaleAt(f); ++f) ++length;
    return length >= kClipRunLength;
}

int64_t Track::endFrame() const {
    int64_t end = 0;
    for (const auto& clip : clips) {
        end = std::max(end, clip.endFrame());
    }
    return end;
}

float Track::envelopeGainAt(int64_t frame) const {
    if (envelope.empty()) {
        return 1.0f;
    }
    if (frame <= envelope.front().frame) {
        return envelope.front().gain;
    }
    if (frame >= envelope.back().frame) {
        return envelope.back().gain;
    }

    auto upper = std::lower_bound(envelope.begin(), envelope.end(), frame,
                                   [](const EnvelopePoint& p, int64_t f) { return p.frame < f; });
    if (upper == envelope.begin()) {
        return upper->gain;
    }
    auto lower = upper - 1;
    int64_t span = upper->frame - lower->frame;
    if (span <= 0) {
        return upper->gain;
    }
    double t = static_cast<double>(frame - lower->frame) / static_cast<double>(span);
    return static_cast<float>(lower->gain + t * (upper->gain - lower->gain));
}

int Track::insertEnvelopePoint(const EnvelopePoint& point) {
    auto pos = std::lower_bound(envelope.begin(), envelope.end(), point.frame,
                                 [](const EnvelopePoint& p, int64_t f) { return p.frame < f; });
    if (pos != envelope.end() && pos->frame == point.frame) {
        pos->gain = point.gain; // one point per frame
        return static_cast<int>(pos - envelope.begin());
    }
    int index = static_cast<int>(pos - envelope.begin());
    envelope.insert(pos, point);
    return index;
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
            // Both halves share the original chunks; nothing is copied.
            second.samples = clip.samples.slice(splitSampleIndex, clip.samples.size() - splitSampleIndex);
            second.peaks.build(second.samples, channels);

            clip.samples = clip.samples.slice(0, splitSampleIndex);
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
            clip.samples.copyTo(0, clip.samples.size(),
                                removed.data() + static_cast<size_t>(destFrameOffset) * channels);
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

void Project::insertRange(Track& track, int64_t atFrame, const SampleBuffer& samples, int channels) {
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
        clip.samples.fill(static_cast<size_t>(localStart) * channels,
                          static_cast<size_t>(localEnd - localStart) * channels, 0.0f);
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
        clip.samples.write(static_cast<size_t>(clipLocalStart) * channels,
                           samples.data() + static_cast<size_t>(srcLocalStart) * channels,
                           static_cast<size_t>(frames) * channels);
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
        clip.samples.copyTo(static_cast<size_t>(clipLocalStart) * channels, static_cast<size_t>(frames) * channels,
                            out.data() + static_cast<size_t>(outLocalStart) * channels);
    }
    return out;
}

void Project::appendClip(Track& track, const SampleBuffer& samples, int channels) {
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
    const size_t headSamples = static_cast<size_t>(aLen - frames) * ch;
    const size_t overlapSamples = static_cast<size_t>(frames) * ch;

    // The overlap: A's tail faded out against B's head faded in.
    std::vector<float> overlap(overlapSamples);
    a.samples.copyTo(headSamples, overlapSamples, overlap.data());
    std::vector<float> incoming(overlapSamples);
    b.samples.copyTo(0, overlapSamples, incoming.data());
    mixEqualPowerCrossfade(overlap, incoming, channels);

    // Crossfading is rare and one-off, so the merged clip is simply built
    // fresh rather than stitched from shared chunks.
    std::vector<float> joined(static_cast<size_t>(aLen + bLen - frames) * ch);
    a.samples.copyTo(0, headSamples, joined.data());
    std::copy(overlap.begin(), overlap.end(), joined.begin() + static_cast<long>(headSamples));
    b.samples.copyTo(overlapSamples, b.samples.size() - overlapSamples,
                     joined.data() + headSamples + overlapSamples);
    merged.samples = SampleBuffer(joined);

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
    // No lock: callers are on the UI thread, which is where every mutation
    // happens. Locking here made the timeline's layout (scroll range, zoom
    // to fit) wait on anyone holding the mutex, e.g. a snapshot or a save.
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
    frameCount = std::min<int64_t>(frameCount, static_cast<int64_t>(out.size() / static_cast<size_t>(channels)));
    mixTracks(tracks, channels, startFrame, frameCount, out.data());
}

void Project::mixTracks(const std::vector<Track>& tracks, int channels, int64_t startFrame,
                        int64_t frameCount, float* out) {
    if (channels <= 0 || frameCount <= 0) {
        return;
    }
    const size_t outSize = static_cast<size_t>(frameCount) * static_cast<size_t>(channels);
    std::fill(out, out + outSize, 0.0f);

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
                // The envelope is sampled per frame rather than per block, so
                // a steep curve doesn't step. Points are few, so the lookup is
                // a short binary search.
                float frameGain = gain * track.envelopeGainAt(overlapStart + f);
                for (int c = 0; c < channels; ++c) {
                    size_t srcIndex = static_cast<size_t>(clipLocalStart + f) * channels + c;
                    size_t dstIndex = static_cast<size_t>(outLocalStart + f) * channels + c;
                    if (srcIndex < clip.samples.size() && dstIndex < outSize) {
                        out[dstIndex] += clip.samples[srcIndex] * frameGain;
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < outSize; ++i) {
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
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
