#include <QtTest>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <thread>

#include <sys/types.h>
#include <unistd.h>

#include "AudioFileReader.h"
#include "Dropouts.h"
#include "RingBuffer.h"
#include "TakeFile.h"

using namespace zrecord;

namespace {

// A deterministic, non-repeating "signal": sample i of the take. Any lost,
// duplicated or reordered sample shows.
float patternAt(int64_t i) {
    const uint32_t h = static_cast<uint32_t>(i) * 2654435761u;
    return static_cast<float>((h >> 8) & 0xFFFF) / 65536.0f - 0.5f;
}

std::vector<float> pattern(int64_t from, int64_t count) {
    std::vector<float> v(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) v[static_cast<size_t>(i)] = patternAt(from + i);
    return v;
}

// Resident memory of this process, in bytes (VmRSS).
int64_t residentBytes() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            int64_t kb = 0;
            status >> kb;
            return kb * 1024;
        }
        std::string rest;
        std::getline(status, rest);
    }
    return 0;
}

// Feeds `frames` of the pattern (interleaved, `channels` wide, starting at
// frame `from`) through a CaptureWriter in blocks, waiting for room in the
// ring instead of losing blocks: the audio callback's side of a take.
void produce(CaptureWriter& writer, RingBuffer& ring, int channels, int64_t from, int64_t frames,
             size_t blockFrames = 512) {
    std::vector<float> block(blockFrames * static_cast<size_t>(channels));
    for (int64_t done = 0; done < frames;) {
        const size_t n = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(blockFrames), frames - done));
        const size_t count = n * static_cast<size_t>(channels);
        while (ring.capacity() - ring.available() < count + static_cast<size_t>(channels) * 4096) {
            std::this_thread::yield(); // the writer thread will make room
        }
        for (size_t s = 0; s < count; ++s) block[s] = patternAt((from + done) * channels + static_cast<int64_t>(s));
        writer.beginCallback(n, false, 0.0);
        QVERIFY(writer.write(block.data(), n));
        done += static_cast<int64_t>(n);
    }
}

constexpr char kChildEnv[] = "ZRECORD_TAKEFILE_CHILD";

// The child process for the kill test: streams an endless take into the
// file named by the environment, says READY once a second of it is on disk,
// and keeps going until it is killed.
int crashChild(const QString& path) {
    RingBuffer ring;
    ring.reset(44100 * 2 * 10);
    CaptureWriter capture;
    capture.reset(&ring, 2, 44100.0);
    TakeWriter writer;
    std::string error;
    if (!writer.start(&ring, path.toStdString(), 2, 44100, error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    bool said = false;
    for (int64_t frame = 0;; frame += 441) {
        produce(capture, ring, 2, frame, 441, 441);
        if (!said && writer.framesOnDisk() >= 44100) {
            std::printf("READY\n");
            std::fflush(stdout);
            said = true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500)); // ~20x real time
    }
}

} // namespace

class TestTakeFile : public QObject {
    Q_OBJECT

private slots:
    void init();
    void appenderWritesAFloatWavEveryReaderOpens();
    void headerStaysValidWhileTheTakeRuns();
    void streamedFileEqualsTheCapturedSamples();
    void dropoutPaddingIsStreamedInPlace();
    void killedMidTakeLeavesAPlayablePartialTake();
    void writeFailureKeepsThePartialTakeAndTheRest();
    void tailAfterAFailureIsBoundedAndCounted();
    void tenMinuteTakeUsesBoundedMemory();
    void livePeakFollowsTheDrainedAudio();
    void readTakeFileRejectsOtherFiles();

private:
    QTemporaryDir dir_;
    QString path(const QString& name) const { return dir_.filePath(name); }
};

void TestTakeFile::init() { QVERIFY(dir_.isValid()); }

void TestTakeFile::appenderWritesAFloatWavEveryReaderOpens() {
    FloatWavAppender file;
    std::string error;
    QVERIFY(file.open(path("a.wav").toStdString(), 2, 48000, error));
    const std::vector<float> samples = pattern(0, 2 * 3000);
    QVERIFY(file.append(samples.data(), 1000));
    QVERIFY(file.append(samples.data() + 2000, 2000));
    QCOMPARE(file.framesWritten(), int64_t(3000));
    QVERIFY(file.close());

    // libsndfile (what import and project loading use) and our own reader
    // both get the samples back exactly: 32-bit float, no conversion.
    std::vector<float> read;
    int rate = 0, channels = 0;
    QVERIFY(AudioFileReader::read(path("a.wav").toStdString(), read, rate, channels, error));
    QCOMPARE(rate, 48000);
    QCOMPARE(channels, 2);
    QVERIFY(read == samples);
    QVERIFY(readTakeFile(path("a.wav").toStdString(), read, channels, rate, error));
    QVERIFY(read == samples);
    QVERIFY(readTakeFile(path("a.wav").toStdString(), read, channels, rate, error, 10));
    QCOMPARE(read.size(), size_t(20));
}

void TestTakeFile::headerStaysValidWhileTheTakeRuns() {
    // Mid-take, with the writer still running, the file on disk is already a
    // valid WAV that any reader opens, holding a prefix of the take.
    RingBuffer ring;
    ring.reset(44100 * 10);
    CaptureWriter capture;
    capture.reset(&ring, 1, 44100.0);
    TakeWriter writer;
    std::string error;
    TakeWriter::Options options;
    options.headerIntervalMs = 30;
    QVERIFY(writer.start(&ring, path("live.wav").toStdString(), 1, 44100, error, options));
    produce(capture, ring, 1, 0, 44100);
    QTRY_COMPARE(writer.framesOnDisk(), int64_t(44100));
    QTest::qWait(100); // at least one header refresh after the last write
    std::vector<float> read;
    int rate = 0, channels = 0;
    QVERIFY(AudioFileReader::read(path("live.wav").toStdString(), read, rate, channels, error));
    QCOMPARE(read.size(), size_t(44100));
    QVERIFY(read == pattern(0, 44100));
    QVERIFY(writer.running());
    writer.stop(capture.finish());
}

void TestTakeFile::streamedFileEqualsTheCapturedSamples() {
    for (int channels : {1, 2}) {
        RingBuffer ring;
        ring.reset(static_cast<size_t>(44100 * channels)); // 1 s: the writer must keep up
        CaptureWriter capture;
        capture.reset(&ring, channels, 44100.0);
        TakeWriter writer;
        std::string error;
        const QString file = path(QString("take%1.wav").arg(channels));
        QVERIFY(writer.start(&ring, file.toStdString(), channels, 44100, error));
        std::thread producer([&] { produce(capture, ring, channels, 0, 5 * 44100 + 17, 333); });
        producer.join();
        writer.stop(capture.finish());
        QVERIFY(!writer.failed());
        QCOMPARE(capture.lostFrames(), int64_t(0));
        QCOMPARE(writer.framesOnDisk(), int64_t(5 * 44100 + 17));
        const std::vector<float> expected = pattern(0, (5 * 44100 + 17) * channels);
        std::vector<float> read;
        int rate = 0, ch = 0;
        QVERIFY(readTakeFile(file.toStdString(), read, ch, rate, error));
        QCOMPARE(ch, channels);
        QVERIFY(read == expected);
        QVERIFY(writer.readTake(error) == expected);
        QVERIFY(AudioFileReader::read(file.toStdString(), read, rate, ch, error));
        QVERIFY(read == expected); // the final header is exact
    }
}

void TestTakeFile::dropoutPaddingIsStreamedInPlace() {
    // Input the host lost is padded with silence of the same length (so the
    // rest of the take stays in time) -- in the file too -- and silence
    // still owed when the take stops is appended at the end.
    RingBuffer ring;
    ring.reset(44100 * 4);
    CaptureWriter capture;
    capture.reset(&ring, 1, 44100.0);
    TakeWriter writer;
    std::string error;
    QVERIFY(writer.start(&ring, path("gaps.wav").toStdString(), 1, 44100, error));
    produce(capture, ring, 1, 0, 1000);
    capture.noteHostLoss(250);
    produce(capture, ring, 1, 1000, 1000);
    capture.noteHostLoss(300); // at the very end: owed at Stop
    writer.stop(capture.finish());
    const std::vector<LostInterval> lost = capture.log().intervals();
    QCOMPARE(lost.size(), size_t(2));
    QCOMPARE(lost[0].startFrame, int64_t(1000));
    QCOMPARE(lost[0].frames, int64_t(250));
    QCOMPARE(lost[1].startFrame, int64_t(2250));

    std::vector<float> expected = pattern(0, 1000);
    expected.insert(expected.end(), 250, 0.0f);
    const std::vector<float> second = pattern(1000, 1000);
    expected.insert(expected.end(), second.begin(), second.end());
    expected.insert(expected.end(), 300, 0.0f);
    std::vector<float> read;
    int rate = 0, ch = 0;
    QVERIFY(readTakeFile(path("gaps.wav").toStdString(), read, ch, rate, error));
    QCOMPARE(read.size(), expected.size());
    QVERIFY(read == expected);
}

void TestTakeFile::killedMidTakeLeavesAPlayablePartialTake() {
    // A real crash: a child process streams a take and is killed with
    // SIGKILL mid-take (no destructors, no final header). What it leaves is
    // a WAV that libsndfile opens, and readTakeFile recovers every whole
    // frame that reached the disk -- exactly the start of the take.
    const QString file = path("killed.wav");
    QProcess child;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(kChildEnv, file);
    child.setProcessEnvironment(env);
    child.start(QCoreApplication::applicationFilePath(), {});
    QVERIFY(child.waitForStarted());
    QVERIFY2(child.waitForReadyRead(20000), qPrintable(child.readAllStandardError()));
    QCOMPARE(child.readLine().trimmed(), QByteArray("READY"));
    // Past the first header refresh (every 500 ms), and then some: the kill
    // lands mid-take, somewhere between two refreshes.
    QTest::qWait(800);
    ::kill(static_cast<pid_t>(child.processId()), SIGKILL);
    QVERIFY(child.waitForFinished(5000));
    QCOMPARE(child.exitStatus(), QProcess::CrashExit);

    std::vector<float> recovered;
    int rate = 0, channels = 0;
    std::string error;
    QVERIFY2(readTakeFile(file.toStdString(), recovered, channels, rate, error), error.c_str());
    QCOMPARE(channels, 2);
    QCOMPARE(rate, 44100);
    const int64_t frames = static_cast<int64_t>(recovered.size()) / 2;
    QVERIFY2(frames >= 44100, qPrintable(QString::number(frames)));
    QVERIFY(recovered == pattern(0, frames * 2));

    // Any player gets a valid file too, at most the last header interval short.
    std::vector<float> played;
    QVERIFY2(AudioFileReader::read(file.toStdString(), played, rate, channels, error), error.c_str());
    QVERIFY(played.size() <= recovered.size());
    QVERIFY(played.size() >= size_t(2 * 44100));
    QVERIFY(std::equal(played.begin(), played.end(), recovered.begin()));
    qInfo("recovered %lld frames; a plain WAV reader sees %zu", static_cast<long long>(frames), played.size() / 2);
}

void TestTakeFile::writeFailureKeepsThePartialTakeAndTheRest() {
    // The disk fills up part-way through a frame: the file is cut back to
    // the last whole frame (a clean prefix of the take), everything else --
    // the rest of that block and all audio until the take is stopped -- is
    // kept in memory, and the failure is reported with the reason.
    RingBuffer ring;
    ring.reset(44100 * 2 * 4);
    CaptureWriter capture;
    capture.reset(&ring, 2, 44100.0);
    TakeWriter writer;
    std::string error;
    const int64_t goodFrames = 10000;
    QVERIFY(writer.start(&ring, path("full.wav").toStdString(), 2, 44100, error, TakeWriter::Options{},
                         goodFrames * 8 + 6));
    produce(capture, ring, 2, 0, 44100);
    QTRY_VERIFY(writer.failed());
    QVERIFY2(QString::fromStdString(writer.errorMessage()).contains("No space left on device"),
             writer.errorMessage().c_str());
    produce(capture, ring, 2, 44100, 4410); // still arriving until the UI stops the take
    writer.stop(capture.finish());

    QCOMPARE(writer.framesOnDisk(), goodFrames);
    std::vector<float> onDisk;
    int rate = 0, channels = 0;
    QVERIFY(readTakeFile(path("full.wav").toStdString(), onDisk, channels, rate, error));
    QVERIFY(onDisk == pattern(0, goodFrames * 2));
    QCOMPARE(QFileInfo(path("full.wav")).size(), FloatWavAppender::kHeaderBytes + goodFrames * 8); // torn frame cut
    QVERIFY(AudioFileReader::read(path("full.wav").toStdString(), onDisk, rate, channels, error));
    QCOMPARE(onDisk.size(), size_t(goodFrames * 2)); // and the header says so

    QCOMPARE(writer.tailFrames(), int64_t(44100 + 4410) - goodFrames);
    QCOMPARE(writer.framesDroppedAfterFailure(), int64_t(0));
    QVERIFY(writer.readTake(error) == pattern(0, (44100 + 4410) * 2)); // nothing lost
}

void TestTakeFile::tailAfterAFailureIsBoundedAndCounted() {
    // If the take somehow kept running long after the disk failed, memory
    // still stays bounded: at most kMaxTailSeconds are kept, and the rest
    // is counted so it can be reported (never dropped silently).
    const int rate = 100; // 60 s = 6000 frames
    RingBuffer ring;
    ring.reset(rate * 100);
    CaptureWriter capture;
    capture.reset(&ring, 1, rate);
    TakeWriter writer;
    std::string error;
    QVERIFY(writer.start(&ring, path("bounded.wav").toStdString(), 1, rate, error, TakeWriter::Options{}, 0));
    for (int i = 0; i < 10; ++i) {
        produce(capture, ring, 1, i * 1000, 1000, 100);
        QTest::qWait(30);
    }
    writer.stop(capture.finish());
    QVERIFY(writer.failed());
    QCOMPARE(writer.framesOnDisk(), int64_t(0));
    QCOMPARE(writer.tailFrames(), int64_t(6000));
    QCOMPARE(writer.framesDroppedAfterFailure(), int64_t(4000));
}

void TestTakeFile::tenMinuteTakeUsesBoundedMemory() {
    // Ten minutes of 48 kHz stereo is 230 MB of float audio. Streamed, the
    // process holds only the 10 s ring and one drain buffer, however long
    // the take: resident memory barely moves while the whole take lands on
    // disk intact.
    const int rate = 48000, channels = 2;
    const int64_t frames = int64_t(10) * 60 * rate;
    RingBuffer ring;
    ring.reset(static_cast<size_t>(rate * 10 * channels));
    CaptureWriter capture;
    capture.reset(&ring, channels, rate);
    TakeWriter writer;
    std::string error;
    const QString file = path("ten-minutes.wav");
    const int64_t baseline = residentBytes();
    QVERIFY(writer.start(&ring, file.toStdString(), channels, rate, error));
    std::atomic<int64_t> peak{baseline};
    std::atomic<bool> done{false};
    std::thread sampler([&] {
        while (!done.load()) {
            peak.store(std::max(peak.load(), residentBytes()));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    produce(capture, ring, channels, 0, frames, 1024);
    writer.stop(capture.finish());
    done.store(true);
    sampler.join();
    QVERIFY(!writer.failed());
    QCOMPARE(writer.framesOnDisk(), frames);
    QCOMPARE(capture.lostFrames(), int64_t(0));
    const int64_t growth = peak.load() - baseline;
    const int64_t audioBytes = frames * channels * 4;
    qInfo("10-minute take: %lld MB of audio, resident memory grew by %lld MB (ring %zu MB, drain buffer %zu MB)",
          static_cast<long long>(audioBytes >> 20), static_cast<long long>(growth >> 20),
          ring.capacity() * 4 >> 20, writer.drainBufferCapacity() * 4 >> 20);
    QVERIFY(writer.drainBufferCapacity() <= ring.capacity());
    QVERIFY2(growth < 48 * 1024 * 1024, qPrintable(QString("grew by %1 MB").arg(growth >> 20)));

    // The file holds every sample, checked a chunk at a time (reading it
    // whole would be the very allocation this test is about).
    QCOMPARE(QFileInfo(file).size(), FloatWavAppender::kHeaderBytes + audioBytes);
    QFile in(file);
    QVERIFY(in.open(QIODevice::ReadOnly));
    in.seek(FloatWavAppender::kHeaderBytes);
    std::vector<float> chunk(1 << 20);
    for (int64_t index = 0; index < frames * channels;) {
        const qint64 got = in.read(reinterpret_cast<char*>(chunk.data()), qint64(chunk.size() * 4));
        QVERIFY(got > 0);
        for (qint64 s = 0; s < got / 4; ++s, ++index) {
            if (chunk[static_cast<size_t>(s)] != patternAt(index)) {
                QFAIL(qPrintable(QString("sample %1 differs").arg(index)));
            }
        }
    }
    in.close();
    QFile::remove(file);
}

void TestTakeFile::livePeakFollowsTheDrainedAudio() {
    RingBuffer ring;
    ring.reset(4096);
    TakeWriter writer;
    std::string error;
    QVERIFY(writer.start(&ring, path("peak.wav").toStdString(), 1, 44100, error));
    float lo = 0.0f, hi = 0.0f;
    QVERIFY(!writer.consumeLivePeak(lo, hi));
    const std::vector<float> block{0.1f, -0.7f, 0.4f, 0.2f};
    QVERIFY(ring.write(block.data(), block.size()));
    bool got = false;
    for (int i = 0; i < 200 && !got; ++i) { // (QTRY_VERIFY would consume it twice)
        got = writer.consumeLivePeak(lo, hi);
        if (!got) QTest::qWait(10);
    }
    QVERIFY(got);
    QCOMPARE(lo, -0.7f);
    QCOMPARE(hi, 0.4f);
    QVERIFY(!writer.consumeLivePeak(lo, hi)); // taken
    writer.stop(0);
}

void TestTakeFile::readTakeFileRejectsOtherFiles() {
    QFile junk(path("junk.wav"));
    QVERIFY(junk.open(QIODevice::WriteOnly));
    junk.write(QByteArray(100, 'x'));
    junk.close();
    std::vector<float> out;
    int channels = 0, rate = 0;
    std::string error;
    QVERIFY(!readTakeFile(path("junk.wav").toStdString(), out, channels, rate, error));
    QVERIFY(!readTakeFile(path("missing.wav").toStdString(), out, channels, rate, error));
    // A crash before any audio: the bare header is an empty take, not an error.
    FloatWavAppender file;
    QVERIFY(file.open(path("empty.wav").toStdString(), 2, 44100, error));
    QVERIFY(readTakeFile(path("empty.wav").toStdString(), out, channels, rate, error));
    QVERIFY(out.empty());
    QCOMPARE(channels, 2);
}

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsSet(kChildEnv)) {
        return crashChild(qEnvironmentVariable(kChildEnv));
    }
    QCoreApplication app(argc, argv);
    TestTakeFile test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_takefile.moc"
