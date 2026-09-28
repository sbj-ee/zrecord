#include "Dropouts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace zrecord {

void DropoutLog::reset(size_t capacity) {
    slots_.assign(std::max<size_t>(1, capacity), LostInterval{});
    count_.store(0, std::memory_order_relaxed);
    unrecorded_.store(0, std::memory_order_relaxed);
    open_ = LostInterval{};
    hasOpen_ = false;
}

void DropoutLog::publish(const LostInterval& interval) {
    const size_t n = count_.load(std::memory_order_relaxed);
    if (n >= slots_.size()) {
        unrecorded_.fetch_add(interval.events, std::memory_order_relaxed);
        return;
    }
    slots_[n] = interval;
    // Release: the slot is written before the reader can see the new count.
    count_.store(n + 1, std::memory_order_release);
}

void DropoutLog::add(const LostInterval& interval) {
    if (hasOpen_ && open_.endFrame() == interval.startFrame) {
        // It runs straight on from the last loss: the same outage, longer.
        open_.frames += interval.frames;
        open_.causes |= interval.causes;
        return;
    }
    if (hasOpen_) {
        publish(open_);
    }
    open_ = interval;
    hasOpen_ = true;
}

void DropoutLog::flush() {
    if (hasOpen_) {
        publish(open_);
        hasOpen_ = false;
    }
}

std::vector<LostInterval> DropoutLog::intervals() const {
    const size_t n = size();
    return std::vector<LostInterval>(slots_.begin(), slots_.begin() + static_cast<long>(n));
}

void CaptureWriter::reset(RingBuffer* ring, int channels, double sampleRate, size_t logCapacity,
                          size_t silenceChunkFrames) {
    ring_ = ring;
    channels_ = std::max(1, channels);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    silence_.assign(std::max<size_t>(1, silenceChunkFrames) * static_cast<size_t>(channels_), 0.0f);
    log_.reset(logCapacity);
    timeline_.store(0, std::memory_order_relaxed);
    lost_.store(0, std::memory_order_relaxed);
    owed_.store(0, std::memory_order_relaxed);
    prevAdc_ = 0.0;
    prevFrames_ = 0;
    havePrev_ = false;
}

void CaptureWriter::beginCallback(size_t frames, bool overflow, double adcTime) {
    const bool timed = adcTime > 0.0;
    if (overflow) {
        int64_t lost = 0; // unknown unless the timestamps can measure it
        if (timed && havePrev_) {
            const double expected = prevAdc_ + static_cast<double>(prevFrames_) / sampleRate_;
            const double gap = adcTime - expected;
            if (gap >= kMinHostGapSeconds && gap <= kMaxHostGapSeconds) {
                lost = std::llround(gap * sampleRate_);
            }
        }
        noteHostLoss(lost);
    }
    prevAdc_ = adcTime;
    prevFrames_ = frames;
    havePrev_ = timed;
}

void CaptureWriter::noteHostLoss(int64_t frames) {
    if (frames <= 0) {
        // Lost, but nobody can say how much: mark the spot, pad nothing.
        log_.add({timeline_.load(std::memory_order_relaxed), 0, kDropoutInputOverflow, 1});
        return;
    }
    loseHere(frames, kDropoutInputOverflow);
}

void CaptureWriter::loseHere(int64_t frames, uint32_t cause) {
    const int64_t at = timeline_.load(std::memory_order_relaxed);
    log_.add({at, frames, cause, 1});
    owed_.store(owed_.load(std::memory_order_relaxed) + frames, std::memory_order_relaxed);
    lost_.store(lost_.load(std::memory_order_relaxed) + frames, std::memory_order_relaxed);
    timeline_.store(at + frames, std::memory_order_relaxed);
}

bool CaptureWriter::payOwed() {
    const int64_t chunk = static_cast<int64_t>(silence_.size() / static_cast<size_t>(channels_));
    int64_t owed = owed_.load(std::memory_order_relaxed);
    bool ok = true;
    while (owed > 0) {
        const int64_t n = std::min(owed, chunk);
        if (!ring_->write(silence_.data(), static_cast<size_t>(n) * static_cast<size_t>(channels_))) {
            ok = false;
            break;
        }
        owed -= n;
    }
    owed_.store(owed, std::memory_order_relaxed);
    return ok;
}

bool CaptureWriter::write(const float* interleaved, size_t frames) {
    if (frames == 0 || ring_ == nullptr) {
        return ring_ != nullptr;
    }
    // Silence owed for earlier losses goes first, or this block would land
    // early. If it can't all be written, the block can't either.
    if (!payOwed() || !ring_->write(interleaved, frames * static_cast<size_t>(channels_))) {
        loseHere(static_cast<int64_t>(frames), kDropoutRingOverrun);
        return false;
    }
    timeline_.store(timeline_.load(std::memory_order_relaxed) + static_cast<int64_t>(frames),
                    std::memory_order_relaxed);
    return true;
}

int64_t CaptureWriter::finish() {
    log_.flush();
    return owed_.exchange(0, std::memory_order_relaxed);
}

std::vector<DropoutSpan> mergeDropouts(const std::vector<LostInterval>& intervals, int64_t joinFrames) {
    std::vector<DropoutSpan> spans;
    for (const LostInterval& iv : intervals) {
        if (!spans.empty() && iv.startFrame - spans.back().endFrame < joinFrames) {
            DropoutSpan& s = spans.back();
            s.endFrame = std::max(s.endFrame, iv.endFrame());
            s.lostFrames += iv.frames;
            s.events += iv.events;
            s.causes |= iv.causes;
            s.lengthUnknown = s.lengthUnknown || iv.lengthUnknown();
            continue;
        }
        DropoutSpan s;
        s.startFrame = iv.startFrame;
        s.endFrame = iv.endFrame();
        s.lostFrames = iv.frames;
        s.events = iv.events;
        s.causes = iv.causes;
        s.lengthUnknown = iv.lengthUnknown();
        spans.push_back(s);
    }
    return spans;
}

int64_t dropoutJoinFrames(double sampleRate) {
    return std::llround(0.100 * sampleRate);
}

std::string formatLostDuration(int64_t frames, double sampleRate) {
    const double ms = sampleRate > 0.0 ? 1000.0 * static_cast<double>(frames) / sampleRate : 0.0;
    char text[32];
    if (frames > 0 && ms < 0.5) {
        return "<1 ms";
    }
    if (ms < 999.5) {
        std::snprintf(text, sizeof text, "%d ms", static_cast<int>(std::lround(ms)));
    } else {
        std::snprintf(text, sizeof text, "%.2f s", ms / 1000.0);
    }
    return text;
}

std::string dropoutLabelText(const DropoutSpan& span, double sampleRate) {
    if (span.lostFrames == 0) {
        return span.events > 1 ? "Dropout \u00d7" + std::to_string(span.events) + " (length unknown)"
                               : "Dropout (length unknown)";
    }
    std::string text = "Dropout ";
    if (span.events > 1) {
        text += "\u00d7" + std::to_string(span.events) + ", ";
    }
    text += formatLostDuration(span.lostFrames, sampleRate);
    if (span.lengthUnknown) {
        text += " + unknown";
    }
    return text;
}

std::string dropoutSummary(const std::vector<DropoutSpan>& spans, double sampleRate) {
    int events = 0;
    int64_t lost = 0;
    bool unknown = false;
    for (const DropoutSpan& s : spans) {
        events += s.events;
        lost += s.lostFrames;
        unknown = unknown || s.lengthUnknown;
    }
    if (events == 0) {
        return {};
    }
    std::string text = std::to_string(events) + (events == 1 ? " dropout, " : " dropouts, ") +
                       formatLostDuration(lost, sampleRate) + " lost";
    if (unknown) {
        text += " (some of unknown length)";
    }
    return text;
}

} // namespace zrecord
