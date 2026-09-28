#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "RingBuffer.h"

namespace zrecord {

// Streaming takes to disk while they are recorded (the idea follows
// Audacity/Tenacity, which write recorded audio into the project as it
// arrives rather than holding it in memory: ideas only, no code).
//
// A take is an append-only 32-bit float WAV. The header's sizes are brought
// up to date every half second, always to no more than what has already
// been written, so a process killed mid-take leaves a valid WAV that any
// reader opens (missing at most the last half second). zrecord's own reader
// (readTakeFile) goes by the file's size instead of the header, so recovery
// gets every whole frame that reached the disk.

// Appends interleaved float frames to a WAV file. Plain POSIX I/O, so a full
// disk or I/O error is reported by the very call that hit it. Not
// thread-safe: one owner at a time (the take writer thread).
class FloatWavAppender {
public:
    static constexpr int64_t kHeaderBytes = 44; // RIFF + fmt (16) + data headers

    FloatWavAppender() = default;
    ~FloatWavAppender();
    FloatWavAppender(const FloatWavAppender&) = delete;
    FloatWavAppender& operator=(const FloatWavAppender&) = delete;

    // Creates (or truncates) `path` and writes a header for an empty take.
    bool open(const std::string& path, int channels, int sampleRate, std::string& error);
    // Appends `frames` frames. On failure (disk full, I/O error) the file is
    // cut back to the last whole frame that was written, the header is
    // brought up to date if possible, and false is returned with error()
    // set; nothing more can be appended. framesWritten() says how much of
    // this call made it (the caller keeps the rest).
    bool append(const float* interleaved, int64_t frames);
    // Rewrites the RIFF and data sizes to match framesWritten().
    bool updateHeader();
    // Flushes written data to the device (fdatasync).
    bool sync();
    // Final header update and close. Safe to call twice.
    bool close();

    bool isOpen() const { return fd_ >= 0; }
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    int64_t framesWritten() const { return framesWritten_; }
    int channels() const { return channels_; }
    const std::string& path() const { return path_; }

    // Tests: every write after `bytes` more bytes of audio fails as if the
    // disk were full (ENOSPC), possibly part-way through a write. < 0: off.
    void setFailAfterBytesForTesting(int64_t bytes) { failAfterBytes_ = bytes; }

private:
    bool fail(const std::string& what, int err);

    int fd_ = -1;
    int channels_ = 1;
    int sampleRate_ = 44100;
    int64_t framesWritten_ = 0;
    int64_t bytesInData_ = 0; // may end mid-frame until a failure cuts it back
    int64_t failAfterBytes_ = -1;
    std::string path_;
    std::string error_;
};

// Reads a take written by FloatWavAppender -- complete, or cut short by a
// crash -- going by the file's size rather than the header's sizes: every
// whole frame on disk is returned. `maxFrames` >= 0 limits how many are
// read (0 reads only the format).
bool readTakeFile(const std::string& path, std::vector<float>& out, int& channels, int& sampleRate,
                  std::string& error, int64_t maxFrames = -1);

// The take writer thread: drains the capture ring (filled by the audio
// callback through CaptureWriter) into a FloatWavAppender. The audio
// callback never touches the disk.
//
// Memory is bounded: the ring's capacity (sized by the engine) plus one
// drain buffer of at most the same size. Nothing grows with the take's
// length.
//
// If a write fails (disk full, I/O error), the file keeps everything up to
// the last whole frame written, and what couldn't be written -- plus
// anything captured until the caller stops the take -- is kept in memory
// (tail()), up to kMaxTailSeconds; beyond that it is counted in
// framesDroppedAfterFailure() and reported, never dropped silently.
class TakeWriter {
public:
    static constexpr double kMaxTailSeconds = 60.0;

    struct Options {
        int drainIntervalMs = 10;   // how often the ring is emptied
        int headerIntervalMs = 500; // how often the header's sizes are refreshed
        int syncIntervalMs = 2000;  // how often written data is fdatasync'd
    };

    TakeWriter() = default;
    ~TakeWriter();
    TakeWriter(const TakeWriter&) = delete;
    TakeWriter& operator=(const TakeWriter&) = delete;

    // Opens `path` and starts the thread. `ring` must outlive stop().
    // `failAfterBytes` >= 0 injects a write failure (tests).
    bool start(RingBuffer* ring, const std::string& path, int channels, int sampleRate, std::string& error,
               Options options, int64_t failAfterBytes = -1);
    bool start(RingBuffer* ring, const std::string& path, int channels, int sampleRate, std::string& error) {
        return start(ring, path, channels, sampleRate, error, Options{});
    }
    // Once the producer has stopped: drains what is left, appends
    // `owedFrames` of silence (losses still owed at the end of the take),
    // finalizes the header and joins the thread.
    void stop(int64_t owedFrames);
    bool running() const { return thread_.joinable(); }

    // Any thread, any time.
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    std::string errorMessage() const;
    int64_t framesOnDisk() const { return framesOnDisk_.load(std::memory_order_relaxed); }
    int64_t tailFrames() const { return tailFrames_.load(std::memory_order_relaxed); }
    int64_t framesDroppedAfterFailure() const { return dropped_.load(std::memory_order_relaxed); }
    // Tests: while paused the thread leaves the ring alone (to make it
    // overrun on purpose). stop() drains regardless.
    void setPausedForTesting(bool paused) { paused_.store(paused, std::memory_order_relaxed); }
    // The largest drain buffer used so far, in floats (for the memory tests).
    size_t drainBufferCapacity() const { return drainCapacity_.load(std::memory_order_relaxed); }

    // The loudest and quietest sample drained since the previous call, for
    // the live waveform; false if nothing new arrived.
    bool consumeLivePeak(float& minValue, float& maxValue);

    // After stop(): the whole take -- the file's frames, then the in-memory
    // tail left by a write failure.
    std::vector<float> readTake(std::string& error) const;
    const std::string& path() const { return path_; }
    int channels() const { return channels_; }

private:
    void run();
    void drainOnce();
    void keep(const float* samples, size_t count); // into the tail, bounded

    RingBuffer* ring_ = nullptr;
    FloatWavAppender file_;
    Options options_;
    std::string path_;
    int channels_ = 1;
    int sampleRate_ = 44100;
    std::vector<float> buffer_; // drain buffer, writer thread only
    std::vector<float> tail_;   // writer thread until stop()
    size_t maxTailSamples_ = 0;

    std::thread thread_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    bool stopRequested_ = false; // under wakeMutex_

    mutable std::mutex infoMutex_; // error text and live peak
    std::string error_;
    bool havePeak_ = false;
    float peakMin_ = 0.0f;
    float peakMax_ = 0.0f;

    std::atomic<bool> failed_{false};
    std::atomic<int64_t> framesOnDisk_{0};
    std::atomic<int64_t> tailFrames_{0};
    std::atomic<int64_t> dropped_{0};
    std::atomic<size_t> drainCapacity_{0};
    std::atomic<bool> paused_{false};
};

} // namespace zrecord
