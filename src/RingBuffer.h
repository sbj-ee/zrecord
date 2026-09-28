#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace zrecord {

// Single-producer, single-consumer lock-free ring buffer for float samples.
//
// The audio callback writes; the UI thread reads. Neither blocks, so the
// callback can never be held up by the UI — which is the whole point: a
// std::mutex in an audio callback can be held by a thread that gets descheduled,
// and the callback misses its deadline through no fault of its own.
//
// Correctness rests on there being exactly one writer and exactly one reader.
// With that, the two indices are only ever advanced by their own side, and the
// acquire/release pairing below is enough to publish the samples themselves.
class RingBuffer {
public:
    // Must be called before any reading or writing, from the consumer side
    // while the producer is stopped.
    void reset(size_t capacity) {
        data_.assign(capacity + 1, 0.0f); // one slot spare: full and empty must differ
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
        overran_.store(false, std::memory_order_relaxed);
    }

    size_t capacity() const { return data_.empty() ? 0 : data_.size() - 1; }

    // Producer side. Writes all of `count` or none of it, so a block of audio
    // is never torn in half. Returns false and latches the overrun flag when
    // there isn't room.
    bool write(const float* source, size_t count) {
        if (data_.empty()) {
            return false;
        }
        const size_t size = data_.size();
        const size_t writePos = writePos_.load(std::memory_order_relaxed);
        const size_t readPos = readPos_.load(std::memory_order_acquire);

        size_t free = (readPos + size - writePos - 1) % size;
        if (count > free) {
            overran_.store(true, std::memory_order_relaxed);
            return false;
        }

        size_t first = std::min(count, size - writePos);
        std::copy(source, source + first, data_.begin() + static_cast<long>(writePos));
        if (count > first) {
            std::copy(source + first, source + count, data_.begin());
        }
        writePos_.store((writePos + count) % size, std::memory_order_release);
        return true;
    }

    // Consumer side. Appends everything available to `out`.
    size_t readAll(std::vector<float>& out) {
        if (data_.empty()) {
            return 0;
        }
        const size_t size = data_.size();
        const size_t readPos = readPos_.load(std::memory_order_relaxed);
        const size_t writePos = writePos_.load(std::memory_order_acquire);

        size_t available = (writePos + size - readPos) % size;
        if (available == 0) {
            return 0;
        }

        size_t first = std::min(available, size - readPos);
        // One exact reservation: two inserts into a vector growing by
        // doubling could otherwise leave it with room for twice the ring.
        out.reserve(out.size() + available);
        out.insert(out.end(), data_.begin() + static_cast<long>(readPos),
                    data_.begin() + static_cast<long>(readPos + first));
        if (available > first) {
            out.insert(out.end(), data_.begin(), data_.begin() + static_cast<long>(available - first));
        }
        readPos_.store((readPos + available) % size, std::memory_order_release);
        return available;
    }

    size_t available() const {
        if (data_.empty()) {
            return 0;
        }
        const size_t size = data_.size();
        return (writePos_.load(std::memory_order_acquire) + size -
                readPos_.load(std::memory_order_relaxed)) % size;
    }

    // Latched: true if any write was ever refused for lack of room. That means
    // audio was dropped, which the caller should surface rather than hide.
    bool overran() const { return overran_.load(std::memory_order_relaxed); }

private:
    std::vector<float> data_;
    std::atomic<size_t> writePos_{0};
    std::atomic<size_t> readPos_{0};
    std::atomic<bool> overran_{false};
};

} // namespace zrecord
