#include "SampleBuffer.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace zrecord {

namespace {
constexpr size_t kMask = SampleBuffer::kChunkSize - 1;
} // namespace

uint64_t SampleBuffer::nextId() {
    static std::atomic<uint64_t> counter{0};
    return ++counter;
}

SampleBuffer::SampleBuffer(const std::vector<float>& samples) {
    if (samples.empty()) {
        return;
    }
    auto list = std::make_shared<ChunkList>();
    list->reserve((samples.size() + kMask) >> kChunkShift);
    for (size_t pos = 0; pos < samples.size(); pos += kChunkSize) {
        size_t n = std::min(kChunkSize, samples.size() - pos);
        list->push_back(std::make_shared<const Chunk>(samples.begin() + static_cast<long>(pos),
                                                      samples.begin() + static_cast<long>(pos + n)));
    }
    chunks_ = std::move(list);
    size_ = samples.size();
    id_ = nextId();
}

SampleBuffer::SampleBuffer(std::initializer_list<float> samples)
    : SampleBuffer(std::vector<float>(samples)) {}

SampleBuffer::SampleBuffer(size_t count, float value) {
    if (count == 0) {
        return;
    }
    auto list = std::make_shared<ChunkList>();
    for (size_t pos = 0; pos < count; pos += kChunkSize) {
        list->push_back(std::make_shared<const Chunk>(std::min(kChunkSize, count - pos), value));
    }
    chunks_ = std::move(list);
    size_ = count;
    id_ = nextId();
}

void SampleBuffer::copyTo(size_t start, size_t count, float* dst) const {
    count = std::min(count, start < size_ ? size_ - start : 0);
    size_t global = offset_ + start;
    while (count > 0) {
        const Chunk& chunk = *(*chunks_)[global >> kChunkShift];
        size_t within = global & kMask;
        size_t n = std::min(count, chunk.size() - within);
        std::memcpy(dst, chunk.data() + within, n * sizeof(float));
        dst += n;
        global += n;
        count -= n;
    }
}

std::vector<float> SampleBuffer::toVector() const {
    std::vector<float> out(size_);
    copyTo(0, size_, out.data());
    return out;
}

SampleBuffer SampleBuffer::slice(size_t start, size_t count) const {
    SampleBuffer out;
    if (start >= size_) {
        return out;
    }
    count = std::min(count, size_ - start);
    if (count == 0) {
        return out;
    }
    size_t firstGlobal = offset_ + start;
    size_t lastGlobal = firstGlobal + count - 1;
    size_t firstChunk = firstGlobal >> kChunkShift;
    size_t lastChunk = lastGlobal >> kChunkShift;
    if (firstChunk == 0 && lastChunk + 1 == chunks_->size()) {
        out.chunks_ = chunks_; // covers every chunk: share the list itself
    } else {
        out.chunks_ = std::make_shared<const ChunkList>(chunks_->begin() + static_cast<long>(firstChunk),
                                                        chunks_->begin() + static_cast<long>(lastChunk + 1));
    }
    out.offset_ = firstGlobal & kMask;
    out.size_ = count;
    out.id_ = (out.chunks_ == chunks_ && out.offset_ == offset_ && count == size_) ? id_ : nextId();
    return out;
}

template <typename Fn>
void SampleBuffer::mutateRange(size_t start, size_t count, Fn&& fn) {
    count = std::min(count, start < size_ ? size_ - start : 0);
    if (count == 0) {
        return;
    }
    auto list = std::make_shared<ChunkList>(*chunks_); // pointer copies only
    size_t global = offset_ + start;
    size_t done = 0;
    while (done < count) {
        size_t index = global >> kChunkShift;
        size_t within = global & kMask;
        auto fresh = std::make_shared<Chunk>(*(*list)[index]);
        size_t n = std::min(count - done, fresh->size() - within);
        fn(fresh->data() + within, done, n);
        (*list)[index] = std::move(fresh);
        global += n;
        done += n;
    }
    chunks_ = std::move(list);
    id_ = nextId();
}

void SampleBuffer::write(size_t start, const float* src, size_t count) {
    mutateRange(start, count, [src](float* dst, size_t done, size_t n) {
        std::memcpy(dst, src + done, n * sizeof(float));
    });
}

void SampleBuffer::fill(size_t start, size_t count, float value) {
    mutateRange(start, count, [value](float* dst, size_t, size_t n) { std::fill(dst, dst + n, value); });
}

bool SampleBuffer::sharesChunkAt(const SampleBuffer& other, size_t index) const {
    if (index >= size_ || index >= other.size_) {
        return false;
    }
    size_t a = offset_ + index;
    size_t b = other.offset_ + index;
    return (*chunks_)[a >> kChunkShift] == (*other.chunks_)[b >> kChunkShift];
}

bool SampleBuffer::operator==(const SampleBuffer& other) const {
    if (size_ != other.size_) {
        return false;
    }
    for (size_t i = 0; i < size_; ++i) {
        if ((*this)[i] != other[i]) {
            return false;
        }
    }
    return true;
}

bool SampleBuffer::operator==(const std::vector<float>& other) const {
    if (size_ != other.size()) {
        return false;
    }
    for (size_t i = 0; i < size_; ++i) {
        if ((*this)[i] != other[i]) {
            return false;
        }
    }
    return true;
}

} // namespace zrecord
