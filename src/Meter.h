#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace zrecord {

// Level metering, from the audio thread to the meter widget.
//
// The audio callback measures each block it handles: per channel, the peak,
// the sum of squares (so RMS over any span of blocks is exact), whether the
// block clipped, and the peak of the raw input before any gain. Blocks
// travel to the UI through a lock-free single-producer/single-consumer
// queue; the UI turns them into bar levels with MeterBallistics. (Design
// idea from Audacity/Tenacity's meter; no code taken.)

constexpr int kMaxMeterChannels = 8;

struct MeterBlock {
    int channels = 0;    // 0 = empty
    int64_t frames = 0;  // frames measured (the RMS denominator)
    float peak[kMaxMeterChannels] = {};        // what is recorded / played
    double sumSquares[kMaxMeterChannels] = {}; // of the same samples
    float inputPeak[kMaxMeterChannels] = {};   // raw input, before gain (recording only)
    bool clipped[kMaxMeterChannels] = {};
    // Blocks carry consecutive sequence numbers; a merged block covers
    // [firstSequence, lastSequence]. Lets tests (and the curious) prove that
    // nothing was dropped or reordered on the way.
    uint64_t firstSequence = 0;
    uint64_t lastSequence = 0;

    bool empty() const { return channels == 0; }
    double rms(int channel) const;
    // Folds `later` into this block: peaks max, sums add, clips OR.
    void merge(const MeterBlock& later);
};

// Bounded lock-free queue for exactly one producer thread and one consumer
// thread. Storage is allocated by reset() (call it while neither side is
// running); tryPush/tryPop never allocate, lock or block.
template <typename T>
class SpscQueue {
public:
    void reset(size_t capacity) {
        slots_.assign(capacity + 1, T{}); // one spare slot: full != empty
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }
    size_t capacity() const { return slots_.empty() ? 0 : slots_.size() - 1; }

    // Producer. False (and nothing written) when full.
    bool tryPush(const T& item) {
        if (slots_.empty()) return false;
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next = (tail + 1) % slots_.size();
        if (next == head_.load(std::memory_order_acquire)) return false;
        slots_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer. False when empty.
    bool tryPop(T& item) {
        if (slots_.empty()) return false;
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        item = slots_[head];
        head_.store((head + 1) % slots_.size(), std::memory_order_release);
        return true;
    }

private:
    std::vector<T> slots_;
    alignas(64) std::atomic<size_t> head_{0}; // consumer's index
    alignas(64) std::atomic<size_t> tail_{0}; // producer's index
};

using MeterQueue = SpscQueue<MeterBlock>;

// The producer side, owned by one audio callback. Measure a block with
// addInput (raw, before gain; optional) and addSignal (what is recorded or
// played), then publish(). If the queue is full the block is kept and merged
// with the next one instead of dropped, so no peak or clip is ever lost --
// only time resolution, and only while the UI isn't draining.
// Real-time safe: no allocation, no locks.
class MeterFeed {
public:
    void reset(int channels); // before the stream starts
    void addInput(const float* interleaved, size_t frames);
    void addSignal(const float* interleaved, size_t frames);
    void publish(MeterQueue& queue);
    // Retries a block held back by a full queue; true once nothing is held.
    // (publish() does this anyway, so the engine never needs to call it.)
    bool flush(MeterQueue& queue);
    bool hasPending() const { return hasPending_; }
    // Hands over a block still held back (queue full) once the producer has
    // stopped for good -- e.g. after Pa_StopStream -- so it isn't lost.
    // Returns false if there was none.
    bool takePending(MeterBlock& out);

private:
    int stride_ = 1;   // channels in the stream
    int channels_ = 1; // channels metered (at most kMaxMeterChannels)
    int runs_[kMaxMeterChannels] = {}; // full-scale runs, carried across blocks
    MeterBlock current_;
    MeterBlock pending_;
    bool hasPending_ = false;
    uint64_t nextSequence_ = 1;
};

// Drains everything waiting in `queue` into `out` (consumer side).
size_t drainMeterQueue(MeterQueue& queue, std::vector<MeterBlock>& out);

// A block for a steady signal at `level` in every channel: peak `level`, RMS
// `rms`. Handy for tests and fakes.
MeterBlock steadyMeterBlock(int channels, float level, float rms, int64_t frames = 1024);

float linearToDb(double linear); // -infinity for 0

// Turns the blocks arriving at each UI tick into what the meter shows, per
// channel: a peak bar and an RMS bar (both jump up at once and fall at the
// decay rate), a peak-hold level (holds for holdMs, then falls), the raw
// input peak, and a latched clip flag. RMS is over the blocks of one tick
// (~50 ms), like a fast RMS meter.
class MeterBallistics {
public:
    static constexpr float kDefaultFloorDb = -60.0f;
    static constexpr float kDefaultDecayDbPerSecond = 24.0f;
    static constexpr int kDefaultHoldMs = 1500;

    struct Channel {
        float peakDb = kDefaultFloorDb;
        float rmsDb = kDefaultFloorDb;
        float holdDb = kDefaultFloorDb;
        float inputDb = kDefaultFloorDb;
        int64_t holdSinceMs = 0;
    };

    void setFloorDb(float db);
    float floorDb() const { return floorDb_; }
    void setDecayDbPerSecond(float dbPerSecond) { decay_ = std::max(1.0f, dbPerSecond); }
    float decayDbPerSecond() const { return decay_; }
    void setHoldMs(int ms) { holdMs_ = std::max(0, ms); }
    int holdMs() const { return holdMs_; }

    // One UI tick at `nowMs` (any monotonic clock) with the blocks that
    // arrived since the previous tick; none means silence.
    void advance(const MeterBlock* blocks, size_t count, int64_t nowMs);

    int channels() const { return channels_; }
    const Channel& channel(int c) const { return state_[std::clamp(c, 0, kMaxMeterChannels - 1)]; }
    float maxPeakDb() const;
    float maxHoldDb() const;
    bool clipLatched() const { return clip_; }
    void resetClip() { clip_ = false; }
    void resetHold(); // holds drop to the current peaks

private:
    float floorDb_ = kDefaultFloorDb;
    float decay_ = kDefaultDecayDbPerSecond;
    int holdMs_ = kDefaultHoldMs;
    int channels_ = 1;
    Channel state_[kMaxMeterChannels];
    int64_t lastMs_ = -1;
    bool clip_ = false;
};

} // namespace zrecord
