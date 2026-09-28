#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "RingBuffer.h"

namespace zrecord {

// Dropout detection for the capture path (the idea follows Audacity/Tenacity,
// which mark lost input on the timeline: ideas only, no code).
//
// Input can be lost in two places:
//  - before zrecord sees it: the host or driver could not deliver the input
//    in time and dropped some (PortAudio sets paInputOverflow on the next
//    callback, and the gap shows in the callback's ADC timestamps);
//  - in zrecord: the capture ring was full because the UI thread fell behind,
//    so a block could not be stored.
//
// Either way the lost stretch is replaced with silence of the same length,
// so everything recorded after it stays at the right time (in sync with
// other tracks and with the clock). Each stretch is recorded as an interval
// of the take (frame position and length), and after Stop each becomes a
// timeline label such as "Dropout 12 ms".

enum DropoutCause : uint32_t {
    kDropoutInputOverflow = 1u << 0, // the host/driver dropped input (paInputOverflow)
    kDropoutRingOverrun = 1u << 1,   // the capture ring was full; zrecord dropped it
};

struct LostInterval {
    int64_t startFrame = 0; // frame in the take where the loss starts
    int64_t frames = 0;     // how much was lost (and padded); 0 = unknown length
    uint32_t causes = 0;    // DropoutCause bits
    int events = 1;         // separate losses merged into this interval

    int64_t endFrame() const { return startFrame + frames; }
    bool lengthUnknown() const { return frames == 0; }
};

// Append-only, single-producer/single-consumer record of lost intervals.
// Storage is allocated by reset(); add() never allocates or locks. Losses
// that run on from the previous one (the next block lost too) extend it
// instead of taking a new slot, so a long outage is one interval. An
// interval is published (visible to the reader) once the next,
// non-contiguous loss starts, or at flush().
class DropoutLog {
public:
    // Consumer side, with the producer stopped.
    void reset(size_t capacity);

    // Producer side. Real-time safe.
    void add(const LostInterval& interval);
    // Publishes the interval still being extended. Producer side, or any
    // thread once the producer has stopped.
    void flush();

    // Consumer side, any time: the published intervals, oldest first.
    size_t size() const { return count_.load(std::memory_order_acquire); }
    const LostInterval& at(size_t index) const { return slots_[index]; }
    std::vector<LostInterval> intervals() const;
    // Losses that did not fit (the log was full). Their frames are still
    // padded and counted by the writer; they just get no interval.
    int64_t unrecorded() const { return unrecorded_.load(std::memory_order_relaxed); }
    size_t capacity() const { return slots_.size(); }

private:
    void publish(const LostInterval& interval);
    std::vector<LostInterval> slots_;
    std::atomic<size_t> count_{0};
    std::atomic<int64_t> unrecorded_{0};
    LostInterval open_; // producer only
    bool hasOpen_ = false;
};

// The end of the capture callback: writes blocks into the capture ring,
// keeps the take's timeline intact across losses, and logs them.
//
// Every frame of the take has a place on the timeline, including lost ones.
// A block the ring can't take is lost; its length is owed as silence and
// paid (written as zeros) ahead of the next block that fits, so later audio
// lands where it belongs. The same happens for input the host reports lost.
// Whatever is still owed at Stop is added by the consumer (finish()).
// Real-time safe after reset(): no allocation, no locks, no logging.
class CaptureWriter {
public:
    // How long a host-reported gap may be before it is treated as a bogus
    // timestamp (logged with unknown length, not padded).
    static constexpr double kMaxHostGapSeconds = 10.0;
    // A host gap shorter than this is timestamp jitter, not a measurement.
    static constexpr double kMinHostGapSeconds = 0.001;

    // Consumer side, before the stream starts.
    void reset(RingBuffer* ring, int channels, double sampleRate, size_t logCapacity = 4096,
               size_t silenceChunkFrames = 4096);

    // Producer: call once per callback, before its blocks are written.
    // `overflow` is paInputOverflow; `adcTime` is the callback's
    // inputBufferAdcTime (<= 0 if the host doesn't provide one). When the
    // flag is set, the lost length is measured from the jump in ADC time
    // against the previous callback. Timestamps are only used to measure a
    // loss the host reported: on their own they jitter too much on common
    // hosts (ALSA, PulseAudio) to prove one.
    void beginCallback(size_t frames, bool overflow, double adcTime);
    // Producer: `frames` of input known to be lost right here (0 = unknown
    // length). Used by beginCallback and by fault injection in tests.
    void noteHostLoss(int64_t frames);
    // Producer: stores one block, or logs it as lost if the ring is full.
    // Returns true if it was stored.
    bool write(const float* interleaved, size_t frames);

    // Consumer, once the producer has stopped: publishes the last interval
    // and returns the frames still owed as silence (append them to the take).
    int64_t finish();

    const DropoutLog& log() const { return log_; }
    // Timeline length of the take so far: stored + owed frames.
    int64_t timelineFrames() const { return timeline_.load(std::memory_order_relaxed); }
    int64_t lostFrames() const { return lost_.load(std::memory_order_relaxed); }
    int64_t owedFrames() const { return owed_.load(std::memory_order_relaxed); }

private:
    void loseHere(int64_t frames, uint32_t cause);
    bool payOwed();

    RingBuffer* ring_ = nullptr;
    int channels_ = 1;
    double sampleRate_ = 44100.0;
    std::vector<float> silence_; // zeros, preallocated
    DropoutLog log_;
    std::atomic<int64_t> timeline_{0};
    std::atomic<int64_t> lost_{0};
    std::atomic<int64_t> owed_{0};
    double prevAdc_ = 0.0;      // producer only
    size_t prevFrames_ = 0;
    bool havePrev_ = false;
};

// A dropout as shown to the user: one or more lost intervals close enough
// together to read as one event.
struct DropoutSpan {
    int64_t startFrame = 0;
    int64_t endFrame = 0;   // end of the last merged interval
    int64_t lostFrames = 0; // sum of the merged intervals (not the span)
    int events = 0;
    uint32_t causes = 0;
    bool lengthUnknown = false; // some merged loss had no measurable length
};

// Joins intervals (sorted by start) that are closer than `joinFrames` of
// good audio into one span.
std::vector<DropoutSpan> mergeDropouts(const std::vector<LostInterval>& intervals, int64_t joinFrames);
// Default join distance: 100 ms. Losses nearer than that are one glitch to
// the listener, and one label is easier to find than a cluster.
int64_t dropoutJoinFrames(double sampleRate);

// "12 ms", "<1 ms", "1.25 s".
std::string formatLostDuration(int64_t frames, double sampleRate);
// The label text: "Dropout 12 ms", "Dropout ×3, 36 ms", "Dropout (length
// unknown)".
std::string dropoutLabelText(const DropoutSpan& span, double sampleRate);
// For the status bar: "3 dropouts, 36 ms lost" (empty when there were none).
std::string dropoutSummary(const std::vector<DropoutSpan>& spans, double sampleRate);

} // namespace zrecord
