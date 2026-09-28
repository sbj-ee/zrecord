#include <QtTest>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "AudioEngine.h"
#include "Capture.h"
#include "Meter.h"
#include "AudioFileReader.h"
#include "TakeFile.h"

using namespace zrecord;

namespace {

constexpr double kRate = 44100.0;

float dbfs(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

std::vector<float> sine(float amplitude, size_t frames, int channels, double hz = 440.0) {
    std::vector<float> s(frames * static_cast<size_t>(channels));
    for (size_t i = 0; i < frames; ++i) {
        const float v = amplitude * static_cast<float>(std::sin(2.0 * M_PI * hz * double(i) / kRate));
        for (int c = 0; c < channels; ++c) {
            s[i * static_cast<size_t>(channels) + static_cast<size_t>(c)] = v;
        }
    }
    return s;
}

float peakOf(const std::vector<float>& v) {
    float p = 0.0f;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}

} // namespace

// The capture path: input gain (the only processing), then the take.
// Unit tests drive the per-block function the audio callback uses; the end
// to end tests run the real PortAudio engine on an ALSA device that plays a
// known sine from a file (skipped where ALSA can't be set up that way).
class TestCapture : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void unityGainAtDefaults_data();
    void unityGainAtDefaults();
    void inputGainScalesExactly();
    void inputGainIsClamped();
    void meterPeakFollowsTheInputGain();
    void int16FullScaleDoesNotOverflow();
    void singleFullScalePeakIsNotAClip();
    void clipRunCarriesAcrossBuffers();
    void int16ExtremesAreTheFullScale();
    void clipCountsAreExact();
    void captureBlockChecksTheRawInputForClips();
    void peakCacheFlagsClippedBlocks();
    void realEngineCapturesAtUnityGain_data();
    void realEngineCapturesAtUnityGain();
    void realEngineAppliesInputGain();
    void realEngineFlagsAFullScaleInput();
    void realEngineCleanFullScaleSineIsNotAClip();
    void realEngineMeterShowsTheRecordedLevel();
    void realEngineTakeIsTheRawInputBitExact();
    void realEngineMeterBlocksCarryRmsAndEveryFrame();
    void realEngineInjectedOverflowIsPaddedInPlace();
    void realEngineRingOverrunIsPaddedInPlace();
    void realEngineStreamsTheTakeToItsFile();
    void realEngineDiskFullKeepsThePartialTake();

private:
    // Records a short take of `infile` (S16 stereo at 44.1 kHz) through the
    // real engine. Returns false (and skips) if the device can't be opened.
    bool captureFromFile(const std::vector<int16_t>& samples, std::vector<float>& take, float& meterPeak,
                         float& inputPeak);
    MeterBlock meter_; // everything the meter got during the last capture
    QTemporaryDir home_;
    QString infile_;
    std::unique_ptr<AudioEngine> engine_;
    int device_ = -1; // the ALSA "default" defined above, once enumerated
};

void TestCapture::initTestCase() {
    QVERIFY(home_.isValid());
    infile_ = home_.filePath("input.raw");
    // ALSA's "file" plugin reads capture data from `infile` (in the slave's
    // format, here S16_LE); "null" underneath supplies the clock-less
    // device. `plug` converts to the float32 PortAudio asks for, the same
    // integer-to-float step a real interface goes through.
    QFile asoundrc(home_.filePath(".asoundrc"));
    QVERIFY(asoundrc.open(QIODevice::WriteOnly));
    asoundrc.write(QString("pcm.!default { type plug; slave { pcm \"cap\"; format S16_LE; rate 44100; channels 2 } }\n"
                           "pcm.cap { type file; slave { pcm \"nul\" } file \"/dev/null\" infile \"%1\" format \"raw\" }\n"
                           "pcm.nul { type null }\n")
                       .arg(infile_)
                       .toUtf8());
    asoundrc.close();
    // PortAudio opens every device while it enumerates them in Pa_Initialize,
    // and "default"/"cap" only open once `infile` exists. Without it only
    // "nul" is listed, and capturing from that reads the raw S16 bytes as
    // floats.
    QFile raw(infile_);
    QVERIFY(raw.open(QIODevice::WriteOnly));
    raw.write(QByteArray(4096, '\0'));
    raw.close();
    qputenv("HOME", home_.path().toUtf8());
    engine_ = std::make_unique<AudioEngine>(); // Pa_Initialize reads the config
    // The null device captures hundreds of times faster than real time and
    // fills the 10 s ring in a few milliseconds; drain it that often.
    TakeWriter::Options writerOptions;
    writerOptions.drainIntervalMs = 1;
    engine_->setTakeWriterOptionsForTesting(writerOptions);
    for (const auto& device : engine_->listInputDevices()) {
        if (device.name == "default") {
            device_ = device.index;
        }
    }
}

void TestCapture::unityGainAtDefaults_data() {
    QTest::addColumn<double>("db");
    QTest::addColumn<int>("channels");
    for (double db : {-12.0, -6.0, 0.0}) {
        for (int ch : {1, 2}) {
            QTest::newRow(qPrintable(QString("%1 dBFS, %2 ch").arg(db).arg(ch))) << db << ch;
        }
    }
}

void TestCapture::unityGainAtDefaults() {
    // 0 dB input gain must record exactly what arrives (there is no other
    // processing on the capture path): peak in == peak out, sample for sample, nothing scaled.
    QFETCH(double, db);
    QFETCH(int, channels);
    const std::vector<float> in = sine(dbfs(db), 4410, channels);
    std::vector<float> block = in;
    const CapturePeaks peaks =
        processCaptureBlock(block, in.size() / size_t(channels), channels, float(inputGainToLinear(0.0)));
    QVERIFY(block == in);
    QCOMPARE(peaks.input, peakOf(in));
    QCOMPARE(peaks.recorded, peakOf(in));
    QVERIFY(std::fabs(peaks.recorded - dbfs(db)) < 1e-4f);
    QVERIFY(peaks.recorded <= 1.0f);
}

void TestCapture::inputGainScalesExactly() {
    const std::vector<float> in = sine(dbfs(-12.0), 4410, 2);
    for (double gainDb : {-12.0, -6.0, 6.0, 12.0}) {
        std::vector<float> block = in;
        const CapturePeaks peaks = processCaptureBlock(block, 4410, 2, float(inputGainToLinear(gainDb)));
        QCOMPARE(peaks.input, peakOf(in)); // measured before the gain
        QVERIFY2(std::fabs(20.0 * std::log10(peaks.recorded) - (-12.0 + gainDb)) < 0.01,
                 qPrintable(QString("%1 dB gain gave %2 dBFS").arg(gainDb).arg(20.0 * std::log10(peaks.recorded))));
        QCOMPARE(peaks.recorded, peakOf(block));
    }
}

void TestCapture::inputGainIsClamped() {
    QCOMPARE(inputGainToLinear(0.0), 1.0);
    QVERIFY(std::fabs(inputGainToLinear(100.0) - std::pow(10.0, kInputGainMaxDb / 20.0)) < 1e-12);
    QVERIFY(std::fabs(inputGainToLinear(-100.0) - std::pow(10.0, kInputGainMinDb / 20.0)) < 1e-12);
}

void TestCapture::meterPeakFollowsTheInputGain() {
    // Regression: the meter measured the raw input, before the gain, so +12 dB
    // recorded a -6 dBFS input at +6 dBFS while the meter read -6 and CLIP
    // stayed dark. (This used the capture-side Gain effect, which no longer
    // exists: effects are per-track playback now, and input gain is the one
    // gain on the way in.)
    std::vector<float> block = sine(dbfs(-6.0), 4410, 2);
    const CapturePeaks peaks = processCaptureBlock(block, 4410, 2, float(inputGainToLinear(12.0)));
    QVERIFY(std::fabs(20.0 * std::log10(peaks.input) - -6.0) < 0.01);
    QVERIFY(std::fabs(20.0 * std::log10(peaks.recorded) - 6.0) < 0.01);
    QCOMPARE(peaks.recorded, peakOf(block));
}

void TestCapture::int16FullScaleDoesNotOverflow() {
    // Full-scale 16-bit input, converted the way PortAudio/ALSA do
    // (x / 32768), stays within +/-1 and counts as input clipping.
    std::vector<float> block;
    for (int v : {32767, -32768, 16384, -16384, 0}) block.push_back(float(v) / 32768.0f);
    const CapturePeaks peaks = processCaptureBlock(block, block.size(), 1, 1.0f);
    QCOMPARE(peaks.recorded, 1.0f);
    QVERIFY(peakOf(block) <= 1.0f);
    QVERIFY(isFullScale(block[0]) && isFullScale(block[1]));
    QVERIFY(!isFullScale(block[2]));
}

namespace {
std::vector<float> ramp(std::initializer_list<float> values) { return std::vector<float>(values); }
} // namespace

void TestCapture::singleFullScalePeakIsNotAClip() {
    // A loud transient that touches full scale once (or twice) is a peak,
    // not clipping; three in a row is a flattened waveform.
    ClipDetector d;
    d.reset(1);
    auto one = ramp({0.5f, 1.0f, 0.5f, -1.0f, 0.2f});
    QCOMPARE(d.feed(one.data(), one.size()), int64_t(0));
    auto two = ramp({0.5f, 1.0f, 1.0f, 0.5f, -1.0f, -1.0f, 0.0f});
    QCOMPARE(d.feed(two.data(), two.size()), int64_t(0));
    QCOMPARE(d.events(), int64_t(0));
    QCOMPARE(d.clippedSamples(), int64_t(0));
    auto three = ramp({0.5f, 1.0f, 1.0f, 1.0f, 0.5f});
    QCOMPARE(d.feed(three.data(), three.size()), int64_t(1));
    QCOMPARE(d.clippedSamples(), int64_t(3));
}

void TestCapture::clipRunCarriesAcrossBuffers() {
    // Two full-scale samples end one callback buffer and one starts the
    // next: one clip, found when the third arrives.
    ClipDetector d;
    d.reset(1);
    auto a = ramp({0.1f, 0.2f, 1.0f, 1.0f});
    auto b = ramp({1.0f, 0.3f});
    QCOMPARE(d.feed(a.data(), a.size()), int64_t(0));
    QCOMPARE(d.feed(b.data(), b.size()), int64_t(1));
    QCOMPARE(d.clippedSamples(), int64_t(3));

    // One long run over three buffers (including a one-sample buffer) is
    // still one clip, with every sample counted.
    d.reset(1);
    auto c = ramp({0.0f, -1.0f});
    auto e = ramp({-1.0f});
    auto f = ramp({-1.0f, -1.0f, 0.0f});
    d.feed(c.data(), c.size());
    d.feed(e.data(), e.size());
    d.feed(f.data(), f.size());
    QCOMPARE(d.events(), int64_t(1));
    QCOMPARE(d.clippedSamples(), int64_t(4));

    // The run is broken by an empty-handed buffer boundary only if a sample
    // below full scale arrives: an empty buffer changes nothing.
    d.reset(1);
    auto g = ramp({1.0f, 1.0f});
    d.feed(g.data(), g.size());
    d.feed(g.data(), 0);
    auto h = ramp({1.0f});
    d.feed(h.data(), h.size());
    QCOMPARE(d.events(), int64_t(1));
}

void TestCapture::int16ExtremesAreTheFullScale() {
    // For 16-bit input the test is the integer extremes, before conversion.
    QVERIFY(isFullScale(int16_t(32767)));
    QVERIFY(isFullScale(int16_t(-32768)));
    QVERIFY(!isFullScale(int16_t(32766)));
    QVERIFY(!isFullScale(int16_t(-32767)));
    QVERIFY(!isFullScale(int16_t(0)));

    // zrecord receives float32 converted as x / 32768; the float thresholds
    // are exactly that image of the integer test, for every int16 value.
    for (int v = -32768; v <= 32767; ++v) {
        const bool asInt = isFullScale(int16_t(v));
        const bool asFloat = isFullScale(float(v) / 32768.0f);
        if (asInt != asFloat) {
            QFAIL(qPrintable(QString("int16 %1: integer test %2, float test %3").arg(v).arg(asInt).arg(asFloat)));
        }
    }
    // Float sources: at or beyond +/-1.0 counts (it clips on playback).
    QVERIFY(isFullScale(1.25f));
    QVERIFY(isFullScale(-1.5f));
    QVERIFY(!isFullScale(0.9999f));

    // The detector's int16 path gives the same answers.
    ClipDetector d;
    d.reset(1);
    const int16_t clipped[] = {0, 32767, 32767, 32767, 0, -32768, -32768, -32768, -32768, 0};
    const int16_t nearly[] = {32766, 32766, 32766, -32767, -32767, -32767};
    QCOMPARE(d.feed(clipped, std::size(clipped)), int64_t(2));
    QCOMPARE(d.feed(nearly, std::size(nearly)), int64_t(0));
    QCOMPARE(d.clippedSamples(), int64_t(7));
}

void TestCapture::clipCountsAreExact() {
    // Mono: runs of 3, 5, 2 (not a clip) and 4 samples.
    ClipDetector d;
    d.reset(1);
    std::vector<float> mono;
    for (int run : {3, 5, 2, 4}) {
        mono.push_back(0.0f);
        mono.insert(mono.end(), size_t(run), run % 2 ? 1.0f : -1.0f);
    }
    mono.push_back(0.0f);
    QCOMPARE(d.feed(mono.data(), mono.size() / 1), int64_t(3));
    QCOMPARE(d.events(), int64_t(3));
    QCOMPARE(d.clippedSamples(), int64_t(3 + 5 + 4));

    // Stereo: runs are per channel. Left clips for four frames; right
    // alternates, so it never has two in a row.
    d.reset(2);
    std::vector<float> stereo;
    for (int i = 0; i < 8; ++i) {
        stereo.push_back(i >= 2 && i < 6 ? 1.0f : 0.1f);
        stereo.push_back(i % 2 ? -1.0f : 0.2f);
    }
    QCOMPARE(d.feed(stereo.data(), 8), int64_t(1));
    QCOMPARE(d.clippedSamples(), int64_t(4));

    // Full scale hopping between channels (L, R, L, R, ...) is no run.
    d.reset(2);
    std::vector<float> hop;
    for (int i = 0; i < 8; ++i) {
        hop.push_back(i % 2 ? 0.0f : 1.0f);
        hop.push_back(i % 2 ? 1.0f : 0.0f);
    }
    QCOMPARE(d.feed(hop.data(), 8), int64_t(0));
}

void TestCapture::captureBlockChecksTheRawInputForClips() {
    // A hard-clipped input is reported as clipped even with the gain turned
    // all the way down: the clipping happened before zrecord.
    std::vector<float> in = sine(dbfs(6.0), 4410, 2);
    for (float& v : in) v = std::clamp(v, -1.0f, 1.0f);
    ClipDetector d;
    d.reset(2);
    std::vector<float> block = in;
    const CapturePeaks peaks =
        processCaptureBlock(block, 4410, 2, float(inputGainToLinear(kInputGainMinDb)), &d);
    // 440 Hz for 0.1 s is 44 cycles: 88 half-cycles, each flattened at the
    // top or bottom, in both channels (the sine starts and ends at 0, so no
    // run is cut off at the edges).
    QCOMPARE(peaks.inputClipEvents, int64_t(2 * 88));
    QCOMPARE(d.events(), int64_t(2 * 88));
    QVERIFY(peaks.recorded < 0.07f); // -24 dB of full scale

    // Without a detector nothing is counted, and a clean -6 dBFS sine has
    // no clips at all.
    block = in;
    QCOMPARE(processCaptureBlock(block, 4410, 2, 1.0f).inputClipEvents, int64_t(0));
    d.reset(2);
    block = sine(dbfs(-6.0), 4410, 2);
    QCOMPARE(processCaptureBlock(block, 4410, 2, 1.0f, &d).inputClipEvents, int64_t(0));
}

void TestCapture::peakCacheFlagsClippedBlocks() {
    // The waveform summary carries a clip flag per 256-frame block, using the
    // same run rule: runs crossing a block boundary flag both blocks, a lone
    // full-scale peak flags nothing.
    constexpr int64_t B = PeakCache::kBlockFrames;
    std::vector<float> mono(size_t(5 * B), 0.25f);
    for (int64_t f = B - 2; f <= B; ++f) mono[size_t(f)] = 1.0f; // blocks 0 and 1
    mono[size_t(2 * B + 10)] = -1.0f;                             // lone peak, block 2
    mono[size_t(2 * B + 11)] = -1.0f;                             // ...and a second: still 2 < 3
    for (int64_t f = 4 * B + 5; f < 4 * B + 50; ++f) mono[size_t(f)] = -1.0f; // block 4
    PeakCache cache;
    cache.build(SampleBuffer(mono), 1);
    QCOMPARE(cache.blockCount(), int64_t(5));
    QVERIFY(cache.blockAt(0).clipped);
    QVERIFY(cache.blockAt(1).clipped);
    QVERIFY(!cache.blockAt(2).clipped);
    QCOMPARE(cache.blockAt(2).minValue, -1.0f); // the peak is still drawn
    QVERIFY(!cache.blockAt(3).clipped);
    QVERIFY(cache.blockAt(4).clipped);

    // A run that spans a whole block and more flags every block it touches.
    std::vector<float> longRun(size_t(4 * B), 0.0f);
    for (int64_t f = B / 2; f < 3 * B + 1; ++f) longRun[size_t(f)] = 1.0f;
    cache.build(SampleBuffer(longRun), 1);
    QVERIFY(cache.blockAt(0).clipped && cache.blockAt(1).clipped && cache.blockAt(2).clipped &&
            cache.blockAt(3).clipped);

    // Stereo: full scale alternating between channels is not a run.
    std::vector<float> hop(size_t(2 * B * 2), 0.0f);
    for (int64_t f = 0; f < 2 * B; ++f) hop[size_t(2 * f + (f % 2))] = 1.0f;
    cache.build(SampleBuffer(hop), 2);
    QVERIFY(!cache.blockAt(0).clipped && !cache.blockAt(1).clipped);

    // The per-sample test the zoomed-in view uses agrees.
    const SampleBuffer buf(mono);
    QVERIFY(isInClipRun(buf, 1, B - 2, 0));
    QVERIFY(isInClipRun(buf, 1, B, 0));
    QVERIFY(!isInClipRun(buf, 1, B + 1, 0));
    QVERIFY(!isInClipRun(buf, 1, 2 * B + 10, 0));
    QVERIFY(!isInClipRun(buf, 1, 2 * B + 11, 0));
    QVERIFY(isInClipRun(buf, 1, 4 * B + 49, 0));
    QVERIFY(!isInClipRun(SampleBuffer(hop), 2, 3, 1));
}

bool TestCapture::captureFromFile(const std::vector<int16_t>& samples, std::vector<float>& take, float& meterPeak,
                                  float& inputPeak) {
    QFile raw(infile_);
    if (!raw.open(QIODevice::WriteOnly)) {
        return false;
    }
    raw.write(reinterpret_cast<const char*>(samples.data()), qint64(samples.size() * sizeof(int16_t)));
    raw.close();

    if (device_ < 0) {
        return false;
    }
    std::string error;
    if (!engine_->startRecording(device_, 2, kRate, error)) {
        return false;
    }
    // The null device has no clock, so capture runs far faster than real
    // time; drain often so the ring never overruns.
    QElapsedTimer timer;
    timer.start();
    meterPeak = 0.0f;
    inputPeak = 0.0f;
    meter_ = MeterBlock{};
    std::vector<MeterBlock> blocks;
    auto drain = [&] {
        blocks.clear();
        engine_->drainMeterBlocks(blocks);
        for (const MeterBlock& b : blocks) meter_.merge(b);
    };
    while (timer.elapsed() < 150) {
        engine_->capturedFrameCount();
        drain();
        QThread::msleep(2);
    }
    engine_->stopRecording();
    drain(); // whatever the last callbacks published
    for (int c = 0; c < meter_.channels; ++c) {
        meterPeak = std::max(meterPeak, meter_.peak[c]);
        inputPeak = std::max(inputPeak, meter_.inputPeak[c]);
    }
    take = engine_->copyCapturedBuffer();
    return true;
}

namespace {
std::vector<int16_t> s16Sine(double db, size_t frames) {
    std::vector<int16_t> s(frames * 2);
    const double a = std::pow(10.0, db / 20.0);
    for (size_t i = 0; i < frames; ++i) {
        const double v = std::round(a * 32767.0 * std::sin(2.0 * M_PI * 440.0 * double(i) / kRate));
        s[2 * i] = s[2 * i + 1] = static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0));
    }
    return s;
}
} // namespace

void TestCapture::realEngineCapturesAtUnityGain_data() {
    QTest::addColumn<double>("db");
    QTest::newRow("-12 dBFS") << -12.0;
    QTest::newRow("-6 dBFS") << -6.0;
    QTest::newRow("0 dBFS") << 0.0;
}

void TestCapture::realEngineCapturesAtUnityGain() {
    QFETCH(double, db);
    engine_->setInputGainDb(0.0);
    std::vector<float> take;
    float meter = 0.0f, input = 0.0f;
    if (!captureFromFile(s16Sine(db, size_t(3 * kRate)), take, meter, input)) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    QVERIFY(!take.empty());
    const float expected = float(std::round(std::pow(10.0, db / 20.0) * 32767.0) / 32768.0);
    const float peak = peakOf(take);
    QVERIFY2(std::fabs(peak - expected) < 1e-4f,
             qPrintable(QString("peak %1, expected %2").arg(double(peak)).arg(double(expected))));
    QVERIFY(peak <= 1.0f);
    QCOMPARE(meter, peak); // the meter shows what was recorded
    QCOMPARE(input, peak); // and at 0 dB that is the raw input
}

void TestCapture::realEngineAppliesInputGain() {
    std::vector<float> take;
    float meter = 0.0f, input = 0.0f;
    engine_->setInputGainDb(-6.0);
    const bool ok = captureFromFile(s16Sine(-6.0, size_t(3 * kRate)), take, meter, input);
    engine_->setInputGainDb(0.0);
    if (!ok) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    QVERIFY(std::fabs(20.0 * std::log10(peakOf(take)) - -12.0) < 0.02);
    QCOMPARE(meter, peakOf(take));
    QVERIFY(std::fabs(20.0 * std::log10(input) - -6.0) < 0.02); // before the gain
}

void TestCapture::realEngineFlagsAFullScaleInput() {
    // A +6 dB sine hard clipped at the int16 extremes, which is what an
    // overdriven converter delivers.
    const std::vector<int16_t> source = s16Sine(6.0, size_t(3 * kRate));
    ClipDetector perPass;
    perPass.reset(2);
    perPass.feed(source.data(), source.size() / 2); // the integer test, before conversion
    QVERIFY(perPass.events() > 1000);

    // At 0 dB the take is the raw input, bit for bit, so the count the engine
    // kept across the real PortAudio callback buffers must equal one pass of
    // the detector over the whole take. (The ALSA file device replays the
    // file, faster than real time and not always seamlessly, so the source
    // alone isn't an exact reference -- the take is.)
    std::vector<float> take;
    float meter = 0.0f, input = 0.0f;
    engine_->setInputGainDb(0.0);
    if (!captureFromFile(source, take, meter, input)) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    ClipDetector wholeTake;
    wholeTake.reset(2);
    wholeTake.feed(take.data(), take.size() / 2);
    InputClipStats stats = engine_->inputClipStats();
    QCOMPARE(stats.events, wholeTake.events());
    QCOMPARE(stats.samples, wholeTake.clippedSamples());
    QVERIFY2(stats.events >= perPass.events(),
             qPrintable(QString("%1 clips; one pass of the source has %2").arg(stats.events).arg(perPass.events())));

    // A clipped input stays clipped whatever the gain: it's still counted
    // with the gain turned down, while the take itself is quiet.
    engine_->setInputGainDb(-12.0);
    const bool ok = captureFromFile(source, take, meter, input);
    engine_->setInputGainDb(0.0);
    QVERIFY(ok);
    stats = engine_->inputClipStats();
    QVERIFY(stats.events >= perPass.events());
    QVERIFY(stats.samples >= kClipRunLength * stats.events);
    QVERIFY(meter < 0.3f); // -12 dBFS recorded
    wholeTake.reset(2);
    wholeTake.feed(take.data(), take.size() / 2);
    QCOMPARE(wholeTake.events(), int64_t(0)); // nothing at full scale in the take
}

void TestCapture::realEngineCleanFullScaleSineIsNotAClip() {
    // A clean 0 dBFS sine touches full scale for a sample at most per cycle:
    // loud, but not clipped.
    std::vector<float> take;
    float meter = 0.0f, input = 0.0f;
    if (!captureFromFile(s16Sine(0.0, size_t(3 * kRate)), take, meter, input)) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    QCOMPARE(engine_->inputClipStats().events, int64_t(0));
    QVERIFY(input > 0.99f);
}

void TestCapture::realEngineMeterShowsTheRecordedLevel() {
    // The reported bug's mechanism, end to end: +12 dB of gain records a
    // -6 dBFS input at +6 dBFS. The meter used to read -6 (the raw input) with
    // CLIP dark; it must read what was recorded.
    engine_->setInputGainDb(12.0);
    std::vector<float> take;
    float meter = 0.0f, input = 0.0f;
    const bool ok = captureFromFile(s16Sine(-6.0, size_t(3 * kRate)), take, meter, input);
    engine_->setInputGainDb(0.0);
    if (!ok) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    QVERIFY(std::fabs(20.0 * std::log10(peakOf(take)) - 6.0) < 0.02);
    QCOMPARE(meter, peakOf(take));
    QVERIFY(meter > 1.0f); // i.e. the meter's CLIP light comes on
    QVERIFY(std::fabs(20.0 * std::log10(input) - -6.0) < 0.02);
}

void TestCapture::realEngineTakeIsTheRawInputBitExact() {
    // Nothing but the input gain touches a take: at 0 dB it is the device's
    // samples exactly, as the S16 -> float conversion delivered them (x/32768).
    // Effects live on tracks and are applied on playback, never recorded.
    engine_->setInputGainDb(0.0);
    std::vector<int16_t> in(size_t(2 * kRate) * 2);
    uint32_t seed = 12345;
    for (int16_t& v : in) { // noise, so any processing or offset shows
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<int16_t>(int32_t(seed >> 16) - 32768) / 2;
    }
    std::vector<float> take;
    float meter = 0.0f, input = 0.0f;
    if (!captureFromFile(in, take, meter, input)) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    // The clock-less null device sometimes skips ahead in the input file
    // (whole ALSA periods, before zrecord sees anything), so the take is
    // checked as runs of the input, in order: every sample must equal the
    // input sample it continues from, exactly. Any processing -- a filter, a
    // gain, an effect -- would change the values and break the runs.
    auto value = [&](size_t j) { return float(in[j]) / 32768.0f; };
    size_t j = 0, matched = 0, skips = 0;
    for (size_t i = 0; i < take.size() && j < in.size(); ++i, ++j) {
        if (take[i] == value(j)) {
            ++matched;
            continue;
        }
        // A skip: find where this run of the take continues in the input
        // (same channel, 16 samples in a row).
        size_t k = j + 1;
        for (; k + 16 <= in.size(); ++k) {
            if ((k - j) % 2 != 0) continue;
            bool run = i + 16 <= take.size();
            for (size_t m = 0; run && m < 16; ++m) run = take[i + m] == value(k + m);
            if (run) break;
        }
        if (k + 16 > in.size()) {
            QFAIL(qPrintable(QString("take sample %1 (%2) isn't the input's").arg(i).arg(take[i])));
        }
        ++skips;
        j = k;
        ++matched;
    }
    QVERIFY2(matched >= size_t(kRate), qPrintable(QString("only %1 samples matched").arg(matched)));
    qInfo("%zu input samples matched exactly (%zu device-side skips)", matched, skips);
}

void TestCapture::realEngineMeterBlocksCarryRmsAndEveryFrame() {
    // The meter's blocks come from the real capture callback through the
    // lock-free queue: per channel, a -6 dBFS sine reads 3.01 dB lower as
    // RMS, and the blocks account for every frame that was recorded.
    engine_->setInputGainDb(0.0);
    std::vector<float> take;
    float meter = 0.0f, input = 0.0f;
    if (!captureFromFile(s16Sine(-6.0, size_t(3 * kRate)), take, meter, input)) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    QCOMPARE(meter_.channels, 2);
    QCOMPARE(meter_.frames, int64_t(take.size() / 2));
    for (int c = 0; c < 2; ++c) {
        const double peakDb = linearToDb(meter_.peak[c]);
        const double rmsDb = linearToDb(meter_.rms(c));
        QVERIFY2(std::fabs(peakDb - -6.0) < 0.02, qPrintable(QString::number(peakDb)));
        QVERIFY2(std::fabs(rmsDb - (peakDb - 3.0103)) < 0.02, qPrintable(QString("rms %1").arg(rmsDb)));
        QVERIFY(!meter_.clipped[c]);
        QCOMPARE(meter_.inputPeak[c], meter_.peak[c]); // 0 dB input gain
    }
    QVERIFY(meter_.lastSequence >= meter_.firstSequence);
}

namespace {
// Stereo S16 input that is never exactly zero (a sine riding on a DC
// offset), so any silent frame in a take is padding.
std::vector<int16_t> s16NeverSilent(size_t frames) {
    std::vector<int16_t> s(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const double v = std::round(32767.0 * (0.3 + 0.25 * std::sin(2.0 * M_PI * 440.0 * double(i) / kRate)));
        s[2 * i] = s[2 * i + 1] = static_cast<int16_t>(v);
    }
    return s;
}

bool isSilentFrame(const std::vector<float>& take, int64_t i) {
    return take[size_t(2 * i)] == 0.0f && take[size_t(2 * i + 1)] == 0.0f;
}

// Every silent frame of `take` is inside a logged interval and every frame
// of an interval is silent. Returns a problem, or an empty string.
QString silenceMatchesIntervals(const std::vector<float>& take, const std::vector<LostInterval>& lost) {
    const int64_t frames = int64_t(take.size() / 2);
    size_t k = 0;
    for (int64_t i = 0; i < frames; ++i) {
        while (k < lost.size() && i >= lost[k].endFrame()) ++k;
        const bool inLoss = k < lost.size() && i >= lost[k].startFrame;
        if (inLoss != isSilentFrame(take, i)) {
            return QString("frame %1: %2 but %3").arg(i).arg(inLoss ? "logged lost" : "not logged lost")
                .arg(isSilentFrame(take, i) ? "silent" : "not silent");
        }
    }
    return {};
}
} // namespace

void TestCapture::realEngineInjectedOverflowIsPaddedInPlace() {
    // The host dropping input (paInputOverflow with a measured gap) can't be
    // provoked on demand, so it is injected into the real callback: 2205
    // frames (50 ms) never arrive. The take gets exactly 50 ms of silence at
    // the logged position, and is 2205 frames longer than the audio that
    // reached the callback (the meter counts every one of those frames).
    if (device_ < 0) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    engine_->setInputGainDb(0.0);
    const std::vector<int16_t> file = s16NeverSilent(size_t(3 * kRate));
    QFile raw(infile_);
    QVERIFY(raw.open(QIODevice::WriteOnly));
    raw.write(reinterpret_cast<const char*>(file.data()), qint64(file.size() * sizeof(int16_t)));
    raw.close();
    std::string error;
    if (!engine_->startRecording(device_, 2, kRate, error)) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    MeterBlock meter;
    std::vector<MeterBlock> blocks;
    auto drain = [&] {
        engine_->capturedFrameCount();
        blocks.clear();
        engine_->drainMeterBlocks(blocks);
        for (const MeterBlock& b : blocks) meter.merge(b);
    };
    QElapsedTimer timer;
    timer.start();
    bool injected = false;
    while (timer.elapsed() < 150) {
        if (!injected && engine_->capturedFrameCount() >= 4410) {
            engine_->simulateInputOverflowForTesting(2205);
            injected = true;
        }
        drain();
        QThread::msleep(1);
    }
    engine_->stopRecording();
    drain();
    QVERIFY(injected);
    const std::vector<float> take = engine_->copyCapturedBuffer();
    const std::vector<LostInterval> lost = engine_->takeDropouts();
    QCOMPARE(lost.size(), size_t(1));
    QCOMPARE(lost[0].frames, int64_t(2205));
    QCOMPARE(lost[0].causes, uint32_t(kDropoutInputOverflow));
    QVERIFY(lost[0].startFrame >= 4410);
    QVERIFY(int64_t(take.size() / 2) > lost[0].endFrame());
    QCOMPARE(engine_->lostFrames(), int64_t(2205));
    QCOMPARE(int64_t(take.size() / 2), meter.frames + 2205);
    const QString problem = silenceMatchesIntervals(take, lost);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
}

void TestCapture::realEngineRingOverrunIsPaddedInPlace() {
    // A real ring overrun: the null device has no clock and delivers input
    // far faster than real time, so pausing the take writer for a moment (a
    // stalled disk) fills the 10 s capture ring. Each lost block becomes silence of the same length
    // at its logged position, so the take is exactly as long as the input
    // the callback received (every frame of it counted by the meter).
    if (device_ < 0) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    engine_->setInputGainDb(0.0);
    const std::vector<int16_t> file = s16NeverSilent(size_t(3 * kRate));
    QFile raw(infile_);
    QVERIFY(raw.open(QIODevice::WriteOnly));
    raw.write(reinterpret_cast<const char*>(file.data()), qint64(file.size() * sizeof(int16_t)));
    raw.close();
    std::string error;
    if (!engine_->startRecording(device_, 2, kRate, error)) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    MeterBlock meter;
    std::vector<MeterBlock> blocks;
    auto drainMeter = [&] {
        blocks.clear();
        engine_->drainMeterBlocks(blocks);
        for (const MeterBlock& b : blocks) meter.merge(b);
    };
    QElapsedTimer timer;
    timer.start();
    engine_->pauseTakeWriterForTesting(true); // a stalled disk
    while (engine_->lostFrames() == 0 && timer.elapsed() < 5000) {
        drainMeter(); // the meter keeps up; the capture ring is left to fill
        QThread::msleep(2);
    }
    const qint64 stalled = timer.elapsed();
    engine_->pauseTakeWriterForTesting(false); // catch up
    while (timer.elapsed() < stalled + 50) {
        drainMeter();
        QThread::msleep(1);
    }
    engine_->stopRecording();
    drainMeter();
    if (engine_->lostFrames() == 0) {
        QSKIP("the capture device did not outrun the ring");
    }
    const std::vector<float> take = engine_->copyCapturedBuffer();
    const std::vector<LostInterval> lost = engine_->takeDropouts();
    QVERIFY(!lost.empty());
    int64_t lostFrames = 0;
    int64_t prevEnd = -1;
    for (const LostInterval& iv : lost) {
        QVERIFY(iv.frames > 0);
        QCOMPARE(iv.causes, uint32_t(kDropoutRingOverrun));
        QVERIFY(iv.startFrame > prevEnd);
        prevEnd = iv.endFrame();
        lostFrames += iv.frames;
    }
    QCOMPARE(lostFrames, engine_->lostFrames());
    QVERIFY(lost.front().startFrame >= int64_t(9 * kRate)); // the ring held ~10 s before the first loss
    QCOMPARE(int64_t(take.size() / 2), meter.frames);
    const QString problem = silenceMatchesIntervals(take, lost);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
}

void TestCapture::realEngineStreamsTheTakeToItsFile() {
    // The take goes to disk as it's recorded: the file named for it holds
    // exactly the take (a valid WAV any reader opens), and the finished take
    // handed to the caller is that file's audio.
    if (device_ < 0) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    engine_->setInputGainDb(0.0);
    const QString takePath = home_.filePath("streamed.wav");
    engine_->setNextTakePath(takePath.toStdString());
    std::vector<float> take;
    float meter = 0.0f, input = 0.0f;
    if (!captureFromFile(s16NeverSilent(size_t(3 * kRate)), take, meter, input)) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    const TakeFileStatus status = engine_->takeFileStatus();
    QCOMPARE(QString::fromStdString(status.path), takePath);
    QVERIFY(!status.failed);
    QCOMPARE(status.framesInMemory, int64_t(0));
    QVERIFY(!take.empty());
    QCOMPARE(status.framesOnDisk, int64_t(take.size() / 2));
    QCOMPARE(int64_t(take.size() / 2), meter_.frames); // every frame the callback got
    std::vector<float> onDisk;
    int channels = 0, rate = 0;
    std::string error;
    QVERIFY(readTakeFile(takePath.toStdString(), onDisk, channels, rate, error));
    QCOMPARE(channels, 2);
    QCOMPARE(rate, int(kRate));
    QVERIFY(onDisk == take);
    QVERIFY(AudioFileReader::read(takePath.toStdString(), onDisk, rate, channels, error));
    QVERIFY(onDisk == take);
    QFile::remove(takePath);
}

void TestCapture::realEngineDiskFullKeepsThePartialTake() {
    // The disk fills up one second into the take. The file keeps that second
    // intact; what the callback captured after it is held in memory until
    // the take is stopped (as the UI does at its next tick), so the finished
    // take still has every frame -- nothing captured is lost.
    if (device_ < 0) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    engine_->setInputGainDb(0.0);
    const std::vector<int16_t> file = s16NeverSilent(size_t(3 * kRate));
    QFile raw(infile_);
    QVERIFY(raw.open(QIODevice::WriteOnly));
    raw.write(reinterpret_cast<const char*>(file.data()), qint64(file.size() * sizeof(int16_t)));
    raw.close();
    const QString takePath = home_.filePath("full.wav");
    engine_->setNextTakePath(takePath.toStdString());
    engine_->simulateTakeWriteFailureAfterBytesForTesting(int64_t(kRate) * 2 * 4);
    std::string error;
    if (!engine_->startRecording(device_, 2, kRate, error)) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    MeterBlock meter;
    std::vector<MeterBlock> blocks;
    auto drain = [&] {
        blocks.clear();
        engine_->drainMeterBlocks(blocks);
        for (const MeterBlock& b : blocks) meter.merge(b);
    };
    QElapsedTimer timer;
    timer.start();
    while (!engine_->takeFileStatus().failed && timer.elapsed() < 5000) {
        drain();
        QThread::msleep(1);
    }
    engine_->stopRecording();
    drain();
    const TakeFileStatus status = engine_->takeFileStatus();
    QVERIFY(status.failed);
    QVERIFY2(QString::fromStdString(status.error).contains("No space left on device"), status.error.c_str());
    QCOMPARE(status.framesOnDisk, int64_t(kRate));
    QCOMPARE(status.framesDropped, int64_t(0));
    const std::vector<float> take = engine_->copyCapturedBuffer();
    QCOMPARE(int64_t(take.size() / 2), meter.frames);
    QCOMPARE(int64_t(take.size() / 2), status.framesOnDisk + status.framesInMemory);
    std::vector<float> onDisk;
    int channels = 0, rate = 0;
    QVERIFY(readTakeFile(takePath.toStdString(), onDisk, channels, rate, error));
    QCOMPARE(onDisk.size(), size_t(2 * kRate));
    QVERIFY(std::equal(onDisk.begin(), onDisk.end(), take.begin())); // the file is the take's start
    QFile::remove(takePath);
}

QTEST_GUILESS_MAIN(TestCapture)
#include "test_capture.moc"
