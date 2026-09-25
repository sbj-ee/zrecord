#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

namespace zrecord {

// Interleaved clip audio, stored as a list of fixed-size, immutable,
// reference-counted chunks.
//
// Copying a SampleBuffer copies one pointer, so an undo snapshot of a track
// costs its clip headers rather than its audio. Writing to a range replaces
// only the chunks it touches (copy-on-write), and slicing (splitting a clip)
// shares the chunks outright. A 0.1 s edit on an hour-long take therefore
// allocates a chunk or two, not another copy of the take.
//
// Chunks are never modified once published, so a copy handed to another
// thread (the playback snapshot) stays valid while the UI edits the original.
class SampleBuffer {
public:
    static constexpr size_t kChunkShift = 16;
    static constexpr size_t kChunkSize = size_t(1) << kChunkShift; // floats per chunk (256 KiB)

    SampleBuffer() = default;
    SampleBuffer(const std::vector<float>& samples); // NOLINT: implicit on purpose
    SampleBuffer(std::initializer_list<float> samples);
    SampleBuffer(size_t count, float value);

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    float operator[](size_t index) const {
        size_t global = offset_ + index;
        return (*(*chunks_)[global >> kChunkShift])[global & (kChunkSize - 1)];
    }

    float front() const { return (*this)[0]; }
    float back() const { return (*this)[size_ - 1]; }

    // Copies [start, start+count) to `dst`.
    void copyTo(size_t start, size_t count, float* dst) const;
    std::vector<float> toVector() const;

    // [start, start+count) sharing this buffer's chunks; no audio is copied.
    SampleBuffer slice(size_t start, size_t count) const;

    // Copy-on-write range updates: only the chunks overlapping the range are
    // duplicated; every other chunk stays shared with existing copies.
    void write(size_t start, const float* src, size_t count);
    void fill(size_t start, size_t count, float value);

    // Replace the contents (convenience for tests and builders).
    void assign(size_t count, float value) { *this = SampleBuffer(count, value); }

    // Identifies this exact content: copies share it, and every write, slice
    // or new buffer gets a fresh one (never reused), so caches can key on it.
    uint64_t contentId() const { return id_; }

    // True when both refer to the very same chunk list (a cheap copy).
    bool sharesStorageWith(const SampleBuffer& other) const {
        return chunks_ != nullptr && chunks_ == other.chunks_;
    }
    // True when the chunk at `index` (in floats) is the same object in both.
    bool sharesChunkAt(const SampleBuffer& other, size_t index) const;

    bool operator==(const SampleBuffer& other) const;
    bool operator!=(const SampleBuffer& other) const { return !(*this == other); }
    bool operator==(const std::vector<float>& other) const;
    bool operator!=(const std::vector<float>& other) const { return !(*this == other); }

private:
    using Chunk = std::vector<float>;
    using ChunkList = std::vector<std::shared_ptr<const Chunk>>;

    // Every chunk holds kChunkSize floats except possibly the last one;
    // offset_ < kChunkSize indexes into the first.
    std::shared_ptr<const ChunkList> chunks_;
    size_t offset_ = 0;
    size_t size_ = 0;
    uint64_t id_ = 0; // 0 = empty

    static uint64_t nextId();

    template <typename Fn>
    void mutateRange(size_t start, size_t count, Fn&& fn);
};

} // namespace zrecord
