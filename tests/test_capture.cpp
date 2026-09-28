#include <QtTest>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "AudioEngine.h"
#include "Capture.h"
#include "Filters.h"

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

// The capture path: input gain, then the live filter chain, then the take.
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
    void meterPeakFollowsTheFilterChain();
    void int16FullScaleDoesNotOverflow();
    void singleFullScalePeakIsNotAClip();
    void clipRunCarriesAcrossBuffers();
    void int16ExtremesAreTheFullScale();
    void clipCountsAreExact();
    void captureBlockChecksTheRawInputForClips();
    void realEngineCapturesAtUnityGain_data();
    void realEngineCapturesAtUnityGain();
    void realEngineAppliesInputGain();
    void realEngineFlagsAFullScaleInput();
    void realEngineCleanFullScaleSineIsNotAClip();
    void realEngineMeterShowsTheRecordedLevel();

private:
    // Records a short take of `infile` (S16 stereo at 44.1 kHz) through the
    // real engine. Returns false (and skips) if the device can't be opened.
    bool captureFromFile(const std::vector<int16_t>& samples, std::vector<float>& take, float& meterPeak,
                         float& inputPeak);
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
    // 0 dB input gain and the default (all off) chain must record exactly
    // what arrives: peak in == peak out, sample for sample, nothing scaled.
    QFETCH(double, db);
    QFETCH(int, channels);
    FilterChain chain;
    chain.prepare(kRate, channels);
    const std::vector<float> in = sine(dbfs(db), 4410, channels);
    std::vector<float> block = in;
    const CapturePeaks peaks =
        processCaptureBlock(block, in.size() / size_t(channels), channels, float(inputGainToLinear(0.0)), chain);
    QVERIFY(block == in);
    QCOMPARE(peaks.input, peakOf(in));
    QCOMPARE(peaks.recorded, peakOf(in));
    QVERIFY(std::fabs(peaks.recorded - dbfs(db)) < 1e-4f);
    QVERIFY(peaks.recorded <= 1.0f);
}

void TestCapture::inputGainScalesExactly() {
    FilterChain chain;
    chain.prepare(kRate, 2);
    const std::vector<float> in = sine(dbfs(-12.0), 4410, 2);
    for (double gainDb : {-12.0, -6.0, 6.0, 12.0}) {
        std::vector<float> block = in;
        const CapturePeaks peaks = processCaptureBlock(block, 4410, 2, float(inputGainToLinear(gainDb)), chain);
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

void TestCapture::meterPeakFollowsTheFilterChain() {
    // Regression: the meter measured the raw input, before the chain, so a
    // +12 dB Gain stage recorded a -6 dBFS input at +6 dBFS while the meter
    // read -6 and CLIP stayed dark.
    FilterChain chain;
    chain.prepare(kRate, 2);
    FilterSettings settings;
    settings.gainEnabled = true;
    settings.gainDb = 12.0;
    chain.setSettings(settings);
    std::vector<float> block = sine(dbfs(-6.0), 4410, 2);
    const CapturePeaks peaks = processCaptureBlock(block, 4410, 2, 1.0f, chain);
    QVERIFY(std::fabs(20.0 * std::log10(peaks.input) - -6.0) < 0.01);
    QVERIFY(std::fabs(20.0 * std::log10(peaks.recorded) - 6.0) < 0.01);
    QCOMPARE(peaks.recorded, peakOf(block));
}

void TestCapture::int16FullScaleDoesNotOverflow() {
    // Full-scale 16-bit input, converted the way PortAudio/ALSA do
    // (x / 32768), stays within +/-1 and counts as input clipping.
    std::vector<float> block;
    for (int v : {32767, -32768, 16384, -16384, 0}) block.push_back(float(v) / 32768.0f);
    FilterChain chain;
    chain.prepare(kRate, 1);
    const CapturePeaks peaks = processCaptureBlock(block, block.size(), 1, 1.0f, chain);
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
    FilterChain chain;
    chain.prepare(kRate, 2);
    ClipDetector d;
    d.reset(2);
    std::vector<float> block = in;
    const CapturePeaks peaks =
        processCaptureBlock(block, 4410, 2, float(inputGainToLinear(kInputGainMinDb)), chain, &d);
    // 440 Hz for 0.1 s is 44 cycles: 88 half-cycles, each flattened at the
    // top or bottom, in both channels (the sine starts and ends at 0, so no
    // run is cut off at the edges).
    QCOMPARE(peaks.inputClipEvents, int64_t(2 * 88));
    QCOMPARE(d.events(), int64_t(2 * 88));
    QVERIFY(peaks.recorded < 0.07f); // -24 dB of full scale

    // Without a detector nothing is counted, and a clean -6 dBFS sine has
    // no clips at all.
    block = in;
    QCOMPARE(processCaptureBlock(block, 4410, 2, 1.0f, chain).inputClipEvents, int64_t(0));
    d.reset(2);
    block = sine(dbfs(-6.0), 4410, 2);
    QCOMPARE(processCaptureBlock(block, 4410, 2, 1.0f, chain, &d).inputClipEvents, int64_t(0));
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
    engine_->takeMeterPeak();
    engine_->takeInputPeak();
    if (!engine_->startRecording(device_, 2, kRate, error)) {
        return false;
    }
    // The null device has no clock, so capture runs far faster than real
    // time; drain often so the ring never overruns.
    QElapsedTimer timer;
    timer.start();
    meterPeak = 0.0f;
    inputPeak = 0.0f;
    while (timer.elapsed() < 150) {
        engine_->capturedFrameCount();
        meterPeak = std::max(meterPeak, engine_->takeMeterPeak());
        inputPeak = std::max(inputPeak, engine_->takeInputPeak());
        QThread::msleep(2);
    }
    engine_->stopRecording();
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
    // The reported bug's mechanism, end to end: +12 dB from the Gain filter
    // records a -6 dBFS input at +6 dBFS. The meter used to read -6 (the raw
    // input) with CLIP dark; it must read what was recorded.
    FilterSettings settings;
    settings.gainEnabled = true;
    settings.gainDb = 12.0;
    engine_->setFilterSettings(settings);
    engine_->setInputGainDb(0.0);
    std::vector<float> take;
    float meter = 0.0f, input = 0.0f;
    const bool ok = captureFromFile(s16Sine(-6.0, size_t(3 * kRate)), take, meter, input);
    engine_->setFilterSettings(FilterSettings{});
    if (!ok) {
        QSKIP("ALSA file/null capture device unavailable");
    }
    QVERIFY(std::fabs(20.0 * std::log10(peakOf(take)) - 6.0) < 0.02);
    QCOMPARE(meter, peakOf(take));
    QVERIFY(meter > 1.0f); // i.e. the meter's CLIP light comes on
    QVERIFY(std::fabs(20.0 * std::log10(input) - -6.0) < 0.02);
}

QTEST_GUILESS_MAIN(TestCapture)
#include "test_capture.moc"
