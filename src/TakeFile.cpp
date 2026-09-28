#include "TakeFile.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace zrecord {

namespace {

constexpr uint16_t kWaveFormatIeeeFloat = 3;

void put16(unsigned char* p, uint16_t v) {
    p[0] = static_cast<unsigned char>(v & 0xff);
    p[1] = static_cast<unsigned char>(v >> 8);
}
void put32(unsigned char* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xff);
}
uint16_t get16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// A WAV's sizes are 32-bit; past 4 GiB they stay at the maximum (readers
// that clamp to the file size, and readTakeFile, still get everything).
uint32_t clampSize(int64_t bytes) {
    return static_cast<uint32_t>(std::min<int64_t>(bytes, int64_t(0xFFFFFFFFu)));
}

void makeHeader(unsigned char* h, int channels, int sampleRate, int64_t dataBytes) {
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, clampSize(dataBytes + FloatWavAppender::kHeaderBytes - 8));
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);
    put16(h + 20, kWaveFormatIeeeFloat);
    put16(h + 22, static_cast<uint16_t>(channels));
    put32(h + 24, static_cast<uint32_t>(sampleRate));
    put32(h + 28, static_cast<uint32_t>(sampleRate * channels * 4));
    put16(h + 32, static_cast<uint16_t>(channels * 4));
    put16(h + 34, 32);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, clampSize(dataBytes));
}

bool writeAll(int fd, const void* data, size_t bytes, size_t& written) {
    const char* p = static_cast<const char*>(data);
    written = 0;
    while (written < bytes) {
        const ssize_t n = ::write(fd, p + written, bytes - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            errno = ENOSPC;
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

FloatWavAppender::~FloatWavAppender() { close(); }

bool FloatWavAppender::fail(const std::string& what, int err) {
    error_ = what + ": " + std::strerror(err);
    return false;
}

bool FloatWavAppender::open(const std::string& path, int channels, int sampleRate, std::string& error) {
    close();
    error_.clear();
    path_ = path;
    channels_ = std::max(1, channels);
    sampleRate_ = sampleRate;
    framesWritten_ = 0;
    bytesInData_ = 0;
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        fail("Could not create " + path, errno);
        error = error_;
        return false;
    }
    unsigned char header[kHeaderBytes];
    makeHeader(header, channels_, sampleRate_, 0);
    size_t written = 0;
    if (!writeAll(fd_, header, sizeof header, written)) {
        fail("Could not write " + path, errno);
        error = error_;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

bool FloatWavAppender::append(const float* interleaved, int64_t frames) {
    if (fd_ < 0 || failed()) {
        return false;
    }
    if (frames <= 0) {
        return true;
    }
    const int64_t frameBytes = int64_t(channels_) * 4;
    size_t bytes = static_cast<size_t>(frames * frameBytes);
    bool injected = false;
    if (failAfterBytes_ >= 0 && static_cast<int64_t>(bytes) > failAfterBytes_) {
        bytes = static_cast<size_t>(failAfterBytes_); // the part that "fits"
        injected = true;
    }
    size_t written = 0;
    const bool ok = writeAll(fd_, interleaved, bytes, written);
    const int err = injected && ok ? ENOSPC : errno;
    if (failAfterBytes_ >= 0) {
        failAfterBytes_ -= static_cast<int64_t>(written);
    }
    bytesInData_ += static_cast<int64_t>(written);
    if (ok && !injected) {
        framesWritten_ += frames;
        return true;
    }
    // Keep what's whole: cut a torn frame off the end so the file is a clean
    // prefix of the take, and the caller holds on to the rest.
    framesWritten_ = bytesInData_ / frameBytes;
    bytesInData_ = framesWritten_ * frameBytes;
    if (::ftruncate(fd_, static_cast<off_t>(kHeaderBytes + bytesInData_)) != 0) {
        // Nothing more to do: readTakeFile ignores a torn last frame anyway.
    }
    fail("Writing the take to " + path_ + " failed", err);
    updateHeader(); // best effort: the header may still fit in place
    return false;
}

bool FloatWavAppender::updateHeader() {
    if (fd_ < 0) {
        return false;
    }
    unsigned char header[kHeaderBytes];
    makeHeader(header, channels_, sampleRate_, framesWritten_ * int64_t(channels_) * 4);
    // Only the two size fields change, but rewriting the 44 bytes in one
    // pwrite is simpler and just as atomic in practice (one page).
    return ::pwrite(fd_, header, sizeof header, 0) == static_cast<ssize_t>(sizeof header);
}

bool FloatWavAppender::sync() { return fd_ >= 0 && ::fdatasync(fd_) == 0; }

bool FloatWavAppender::close() {
    if (fd_ < 0) {
        return true;
    }
    bool ok = updateHeader();
    ok = (::fdatasync(fd_) == 0) && ok;
    ok = (::close(fd_) == 0) && ok;
    fd_ = -1;
    return ok;
}

bool readTakeFile(const std::string& path, std::vector<float>& out, int& channels, int& sampleRate,
                  std::string& error, int64_t maxFrames) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "Could not open " + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat st {};
    unsigned char header[FloatWavAppender::kHeaderBytes];
    const bool headerOk = ::fstat(fd, &st) == 0 &&
                          ::pread(fd, header, sizeof header, 0) == static_cast<ssize_t>(sizeof header) &&
                          std::memcmp(header, "RIFF", 4) == 0 && std::memcmp(header + 8, "WAVE", 4) == 0 &&
                          std::memcmp(header + 12, "fmt ", 4) == 0 && get32(header + 16) == 16 &&
                          get16(header + 20) == kWaveFormatIeeeFloat && get16(header + 34) == 32 &&
                          std::memcmp(header + 36, "data", 4) == 0;
    if (!headerOk || get16(header + 22) == 0) {
        ::close(fd);
        error = path + " is not a zrecord take file";
        return false;
    }
    channels = get16(header + 22);
    sampleRate = static_cast<int>(get32(header + 24));
    const int64_t frameBytes = int64_t(channels) * 4;
    int64_t frames = (static_cast<int64_t>(st.st_size) - FloatWavAppender::kHeaderBytes) / frameBytes;
    frames = std::max<int64_t>(0, frames);
    if (maxFrames >= 0) {
        frames = std::min(frames, maxFrames);
    }
    out.resize(static_cast<size_t>(frames * channels));
    const size_t bytes = out.size() * sizeof(float);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = ::pread(fd, reinterpret_cast<char*>(out.data()) + done, bytes - done,
                                  static_cast<off_t>(FloatWavAppender::kHeaderBytes + static_cast<int64_t>(done)));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            error = "Could not read " + path;
            out.clear();
            return false;
        }
        done += static_cast<size_t>(n);
    }
    ::close(fd);
    return true;
}

TakeWriter::~TakeWriter() {
    if (thread_.joinable()) {
        stop(0);
    }
}

bool TakeWriter::start(RingBuffer* ring, const std::string& path, int channels, int sampleRate, std::string& error,
                       Options options, int64_t failAfterBytes) {
    if (thread_.joinable()) {
        error = "A take is already being written";
        return false;
    }
    ring_ = ring;
    path_ = path;
    channels_ = std::max(1, channels);
    sampleRate_ = sampleRate;
    options_ = options;
    if (!file_.open(path, channels_, sampleRate, error)) {
        return false;
    }
    file_.setFailAfterBytesForTesting(failAfterBytes);
    buffer_.clear();
    buffer_.shrink_to_fit();
    tail_.clear();
    maxTailSamples_ = static_cast<size_t>(kMaxTailSeconds * sampleRate) * static_cast<size_t>(channels_);
    stopRequested_ = false;
    error_.clear();
    havePeak_ = false;
    failed_.store(false);
    framesOnDisk_.store(0);
    tailFrames_.store(0);
    dropped_.store(0);
    drainCapacity_.store(0);
    paused_.store(false);
    thread_ = std::thread([this] { run(); });
    return true;
}

void TakeWriter::keep(const float* samples, size_t count) {
    const size_t room = maxTailSamples_ > tail_.size() ? maxTailSamples_ - tail_.size() : 0;
    const size_t kept = std::min(room, count) / static_cast<size_t>(channels_) * static_cast<size_t>(channels_);
    tail_.insert(tail_.end(), samples, samples + kept);
    tailFrames_.store(static_cast<int64_t>(tail_.size() / static_cast<size_t>(channels_)), std::memory_order_relaxed);
    if (count > kept) {
        dropped_.fetch_add(static_cast<int64_t>((count - kept) / static_cast<size_t>(channels_)),
                           std::memory_order_relaxed);
    }
}

void TakeWriter::drainOnce() {
    buffer_.clear(); // keeps its capacity: at most the ring's
    if (ring_ == nullptr || ring_->readAll(buffer_) == 0) {
        return;
    }
    drainCapacity_.store(std::max(drainCapacity_.load(std::memory_order_relaxed), buffer_.capacity()),
                         std::memory_order_relaxed);
    {
        const auto [lo, hi] = std::minmax_element(buffer_.begin(), buffer_.end());
        std::lock_guard<std::mutex> lock(infoMutex_);
        peakMin_ = havePeak_ ? std::min(peakMin_, *lo) : *lo;
        peakMax_ = havePeak_ ? std::max(peakMax_, *hi) : *hi;
        havePeak_ = true;
    }
    const size_t channels = static_cast<size_t>(channels_);
    const int64_t frames = static_cast<int64_t>(buffer_.size() / channels);
    if (failed()) {
        keep(buffer_.data(), buffer_.size());
        return;
    }
    const int64_t before = file_.framesWritten();
    if (file_.append(buffer_.data(), frames)) {
        framesOnDisk_.store(file_.framesWritten(), std::memory_order_relaxed);
        return;
    }
    // The disk refused it: the file holds a clean prefix of the take; the
    // rest of this block, and everything after it, stays in memory.
    const int64_t onDisk = file_.framesWritten() - before;
    framesOnDisk_.store(file_.framesWritten(), std::memory_order_relaxed);
    keep(buffer_.data() + static_cast<size_t>(onDisk) * channels, buffer_.size() - static_cast<size_t>(onDisk) * channels);
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        error_ = file_.error();
    }
    failed_.store(true, std::memory_order_release);
}

void TakeWriter::run() {
    using Clock = std::chrono::steady_clock;
    auto lastHeader = Clock::now();
    auto lastSync = lastHeader;
    std::unique_lock<std::mutex> lock(wakeMutex_);
    while (!stopRequested_) {
        wake_.wait_for(lock, std::chrono::milliseconds(options_.drainIntervalMs), [this] { return stopRequested_; });
        lock.unlock();
        if (!paused_.load(std::memory_order_relaxed)) {
            drainOnce();
        }
        const auto now = Clock::now();
        if (!failed()) {
            if (now - lastSync >= std::chrono::milliseconds(options_.syncIntervalMs)) {
                file_.sync(); // data first, then the header that points at it
                lastSync = now;
            }
            if (now - lastHeader >= std::chrono::milliseconds(options_.headerIntervalMs)) {
                file_.updateHeader();
                lastHeader = now;
            }
        }
        lock.lock();
    }
}

void TakeWriter::stop(int64_t owedFrames) {
    if (!thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        stopRequested_ = true;
    }
    wake_.notify_all();
    thread_.join();
    // The thread is gone and the producer has stopped: finish on this thread.
    drainOnce();
    if (owedFrames > 0) {
        // Silence still owed for losses at the very end, in bounded pieces.
        std::vector<float> zeros(static_cast<size_t>(std::min<int64_t>(owedFrames, 4096)) * static_cast<size_t>(channels_),
                                 0.0f);
        for (int64_t left = owedFrames; left > 0;) {
            const int64_t n = std::min<int64_t>(left, 4096);
            const size_t count = static_cast<size_t>(n) * static_cast<size_t>(channels_);
            if (failed() || !file_.append(zeros.data(), n)) {
                if (!failed()) {
                    std::lock_guard<std::mutex> lock(infoMutex_);
                    error_ = file_.error();
                    failed_.store(true);
                }
                keep(zeros.data(), count);
            }
            left -= n;
        }
        framesOnDisk_.store(file_.framesWritten());
    }
    file_.close();
}

std::string TakeWriter::errorMessage() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return error_;
}

bool TakeWriter::consumeLivePeak(float& minValue, float& maxValue) {
    std::lock_guard<std::mutex> lock(infoMutex_);
    if (!havePeak_) {
        return false;
    }
    minValue = peakMin_;
    maxValue = peakMax_;
    havePeak_ = false;
    return true;
}

std::vector<float> TakeWriter::readTake(std::string& error) const {
    std::vector<float> take;
    int channels = 0, rate = 0;
    if (!path_.empty() && !readTakeFile(path_, take, channels, rate, error, framesOnDisk())) {
        take.clear();
    }
    take.insert(take.end(), tail_.begin(), tail_.end());
    return take;
}

} // namespace zrecord
