#include "Meter.h"

#include "Capture.h"

#include <cmath>
#include <limits>

namespace zrecord {

double MeterBlock::rms(int channel) const {
    if (frames <= 0 || channel < 0 || channel >= kMaxMeterChannels) return 0.0;
    return std::sqrt(sumSquares[channel] / static_cast<double>(frames));
}

void MeterBlock::merge(const MeterBlock& later) {
    if (later.empty()) return;
    if (empty()) {
        *this = later;
        return;
    }
    const int n = std::max(channels, later.channels);
    for (int c = 0; c < n; ++c) {
        peak[c] = std::max(peak[c], later.peak[c]);
        sumSquares[c] += later.sumSquares[c];
        inputPeak[c] = std::max(inputPeak[c], later.inputPeak[c]);
        clipped[c] = clipped[c] || later.clipped[c];
    }
    channels = n;
    frames += later.frames;
    lastSequence = later.lastSequence;
}

void MeterFeed::reset(int channels) {
    stride_ = std::max(1, channels);
    channels_ = std::min(stride_, kMaxMeterChannels);
    std::fill(std::begin(runs_), std::end(runs_), 0);
    current_ = MeterBlock{};
    pending_ = MeterBlock{};
    hasPending_ = false;
    nextSequence_ = 1;
}

void MeterFeed::addInput(const float* interleaved, size_t frames) {
    // Channels beyond kMaxMeterChannels are skipped (the stride stays right).
    const size_t stride = static_cast<size_t>(stride_);
    for (size_t f = 0; f < frames; ++f) {
        for (int c = 0; c < channels_; ++c) {
            current_.inputPeak[c] = std::max(current_.inputPeak[c], std::fabs(interleaved[f * stride + c]));
        }
    }
    current_.channels = channels_;
}

void MeterFeed::addSignal(const float* interleaved, size_t frames) {
    const size_t stride = static_cast<size_t>(stride_);
    for (size_t f = 0; f < frames; ++f) {
        for (int c = 0; c < channels_; ++c) {
            const float s = interleaved[f * stride + c];
            const float a = std::fabs(s);
            current_.peak[c] = std::max(current_.peak[c], a);
            current_.sumSquares[c] += static_cast<double>(s) * static_cast<double>(s);
            // Clipped: beyond full scale (it will be cut off on playback or
            // export), or a run of full-scale samples as in Capture.h. A
            // single sample touching full scale is only a peak.
            if (!isFullScale(s)) {
                runs_[c] = 0;
            } else {
                runs_[c] = std::min(runs_[c] + 1, kClipRunLength);
                if (runs_[c] >= kClipRunLength || a > 1.0f) {
                    current_.clipped[c] = true;
                }
            }
        }
    }
    current_.frames += static_cast<int64_t>(frames);
    current_.channels = channels_;
}

void MeterFeed::publish(MeterQueue& queue) {
    if (current_.empty()) return;
    current_.firstSequence = current_.lastSequence = nextSequence_++;
    if (hasPending_) {
        pending_.merge(current_);
    } else {
        pending_ = current_;
    }
    hasPending_ = !queue.tryPush(pending_);
    current_ = MeterBlock{};
}

bool MeterFeed::flush(MeterQueue& queue) {
    if (hasPending_ && queue.tryPush(pending_)) {
        hasPending_ = false;
    }
    return !hasPending_;
}

bool MeterFeed::takePending(MeterBlock& out) {
    if (!hasPending_) return false;
    out = pending_;
    hasPending_ = false;
    return true;
}

size_t drainMeterQueue(MeterQueue& queue, std::vector<MeterBlock>& out) {
    size_t n = 0;
    MeterBlock block;
    while (queue.tryPop(block)) {
        out.push_back(block);
        ++n;
    }
    return n;
}

MeterBlock steadyMeterBlock(int channels, float level, float rms, int64_t frames) {
    MeterBlock b;
    b.channels = std::clamp(channels, 1, kMaxMeterChannels);
    b.frames = frames;
    for (int c = 0; c < b.channels; ++c) {
        b.peak[c] = std::fabs(level);
        b.sumSquares[c] = static_cast<double>(rms) * rms * static_cast<double>(frames);
        b.inputPeak[c] = std::fabs(level);
        // A steady level held for many samples: at full scale that is a run.
        b.clipped[c] = std::fabs(level) >= kFullScalePositive;
    }
    return b;
}

float linearToDb(double linear) {
    return linear > 0.0 ? static_cast<float>(20.0 * std::log10(linear)) : -std::numeric_limits<float>::infinity();
}

void MeterBallistics::setFloorDb(float db) {
    floorDb_ = std::min(db, -6.0f);
    for (Channel& ch : state_) {
        ch.peakDb = std::max(ch.peakDb, floorDb_);
        ch.rmsDb = std::max(ch.rmsDb, floorDb_);
        ch.holdDb = std::max(ch.holdDb, floorDb_);
        ch.inputDb = std::max(ch.inputDb, floorDb_);
    }
}

void MeterBallistics::advance(const MeterBlock* blocks, size_t count, int64_t nowMs) {
    MeterBlock sum;
    for (size_t i = 0; i < count; ++i) {
        sum.merge(blocks[i]);
    }
    if (!sum.empty()) {
        channels_ = sum.channels;
    }
    const int64_t elapsed = lastMs_ < 0 ? 0 : std::max<int64_t>(0, nowMs - lastMs_);
    lastMs_ = nowMs;
    const float fall = decay_ * static_cast<float>(elapsed) / 1000.0f;

    for (int c = 0; c < kMaxMeterChannels; ++c) {
        Channel& ch = state_[c];
        const float peakDb = std::max(floorDb_, linearToDb(sum.peak[c]));
        const float rmsDb = std::max(floorDb_, linearToDb(sum.rms(c)));
        const float inputDb = std::max(floorDb_, linearToDb(sum.inputPeak[c]));
        // Instant attack, steady fall.
        ch.peakDb = std::max({peakDb, ch.peakDb - fall, floorDb_});
        ch.rmsDb = std::max({rmsDb, ch.rmsDb - fall, floorDb_});
        ch.inputDb = std::max({inputDb, ch.inputDb - fall, floorDb_});
        if (peakDb >= ch.holdDb) {
            ch.holdDb = peakDb;
            ch.holdSinceMs = nowMs;
        } else if (nowMs - ch.holdSinceMs > holdMs_) {
            ch.holdDb = std::max({ch.peakDb, ch.holdDb - fall, floorDb_});
        }
        if (sum.clipped[c]) clip_ = true;
    }
}

float MeterBallistics::maxPeakDb() const {
    float m = floorDb_;
    for (int c = 0; c < channels_; ++c) m = std::max(m, state_[c].peakDb);
    return m;
}

float MeterBallistics::maxHoldDb() const {
    float m = floorDb_;
    for (int c = 0; c < channels_; ++c) m = std::max(m, state_[c].holdDb);
    return m;
}

void MeterBallistics::resetHold() {
    for (Channel& ch : state_) ch.holdDb = ch.peakDb;
}

} // namespace zrecord
