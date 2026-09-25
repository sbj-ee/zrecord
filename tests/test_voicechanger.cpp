#include <QtTest>
#include <QUndoStack>

#include <cmath>

#include "Commands.h"
#include "Fft.h"
#include "VoiceChanger.h"

using namespace zrecord;

namespace {

constexpr double kRate = 44100.0;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double hz, double seconds, float amp = 0.5f) {
    std::vector<float> s(static_cast<size_t>(seconds * kRate));
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = amp * static_cast<float>(std::sin(2 * kPi * hz * double(i) / kRate));
    }
    return s;
}

// A voiced, vowel-like tone: harmonics of f0 shaped by two formant peaks.
std::vector<float> vowel(double f0, double seconds, double f1 = 700.0, double f2 = 1200.0) {
    std::vector<float> s(static_cast<size_t>(seconds * kRate), 0.0f);
    for (int h = 1; f0 * h < 8000.0; ++h) {
        const double f = f0 * h;
        const double a = 1.0 / (1.0 + std::pow((f - f1) / 150.0, 2)) + 0.7 / (1.0 + std::pow((f - f2) / 200.0, 2)) + 0.02;
        for (size_t i = 0; i < s.size(); ++i) {
            s[i] += static_cast<float>(0.12 * a * std::sin(2 * kPi * f * double(i) / kRate + h));
        }
    }
    return s;
}

std::vector<float> middle(const std::vector<float>& s, double seconds = 0.4) {
    const size_t count = static_cast<size_t>(seconds * kRate);
    const size_t start = (s.size() - count) / 2;
    return {s.begin() + static_cast<long>(start), s.begin() + static_cast<long>(start + count)};
}

// YIN fundamental estimate (de Cheveigne & Kawahara 2002), 50..1500 Hz.
double yinF0(const std::vector<float>& x) {
    const size_t minLag = static_cast<size_t>(kRate / 1500.0);
    const size_t maxLag = static_cast<size_t>(kRate / 50.0);
    const size_t w = x.size() - maxLag - 2;
    std::vector<double> d(maxLag + 2, 0.0), cmnd(maxLag + 2, 1.0);
    for (size_t tau = 1; tau <= maxLag + 1; ++tau) {
        double sum = 0.0;
        for (size_t i = 0; i < w; ++i) {
            const double diff = double(x[i]) - double(x[i + tau]);
            sum += diff * diff;
        }
        d[tau] = sum;
    }
    double running = 0.0;
    for (size_t tau = 1; tau <= maxLag + 1; ++tau) {
        running += d[tau];
        cmnd[tau] = running > 0.0 ? d[tau] * double(tau) / running : 1.0;
    }
    size_t tau = minLag;
    for (; tau <= maxLag; ++tau) {
        if (cmnd[tau] < 0.15) {
            while (tau + 1 <= maxLag && cmnd[tau + 1] < cmnd[tau]) {
                ++tau;
            }
            break;
        }
    }
    if (tau > maxLag) { // no clear dip: take the global minimum
        tau = size_t(std::min_element(cmnd.begin() + long(minLag), cmnd.begin() + long(maxLag + 1)) - cmnd.begin());
    }
    const double a = cmnd[tau - 1], b = cmnd[tau], c = cmnd[tau + 1];
    const double denom = a - 2 * b + c;
    const double offset = std::fabs(denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
    return kRate / (double(tau) + offset);
}

// Power-weighted mean frequency below 5 kHz (a stand-in for where the
// spectral envelope's energy sits).
double spectralCentroid(const std::vector<float>& x) {
    const size_t n = 8192;
    std::vector<float> re(n), im(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        re[i] = x[i] * float(0.5 - 0.5 * std::cos(2 * kPi * double(i) / double(n)));
    }
    fftRadix2(re, im);
    double num = 0.0, den = 0.0;
    for (size_t k = 1; k < n / 2; ++k) {
        const double f = double(k) * kRate / double(n);
        if (f > 5000.0) break;
        const double p = double(re[k]) * re[k] + double(im[k]) * im[k];
        num += f * p;
        den += p;
    }
    return num / den;
}


double rms(const std::vector<float>& x, size_t from, size_t count);

// Time offset (s) of `b`'s loudness contour against `a`'s: the lag that
// best lines up their 5 ms RMS envelopes (1 ms steps, +/-40 ms).
double envelopeLag(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t step = size_t(0.001 * kRate), win = size_t(0.005 * kRate);
    std::vector<double> ea, eb;
    for (size_t i = 0; i + win < a.size(); i += step) {
        ea.push_back(rms(a, i, win));
        eb.push_back(rms(b, i, win));
    }
    int bestLag = 0;
    double bestScore = -1.0;
    for (int lag = -40; lag <= 40; ++lag) {
        double score = 0.0;
        for (size_t i = 40; i + 40 < ea.size(); ++i) score += ea[i] * eb[size_t(int(i) + lag)];
        if (score > bestScore) {
            bestScore = score;
            bestLag = lag;
        }
    }
    return bestLag * 0.001;
}

double rms(const std::vector<float>& x, size_t from, size_t count) {
    double e = 0.0;
    for (size_t i = from; i < from + count; ++i) e += double(x[i]) * x[i];
    return std::sqrt(e / double(count));
}

// First and last sample where a 5 ms RMS reaches half the steady level.
std::pair<double, double> halfLevelEdges(const std::vector<float>& x, double steady) {
    const size_t win = size_t(0.005 * kRate);
    double first = -1, last = -1;
    for (size_t i = 0; i + win < x.size(); i += 8) {
        if (rms(x, i, win) >= 0.5 * steady) {
            if (first < 0) first = double(i + win / 2) / kRate;
            last = double(i + win / 2) / kRate;
        }
    }
    return {first, last};
}

} // namespace

class TestVoiceChanger : public QObject {
    Q_OBJECT

private slots:
    void identityLeavesAudioUntouched();
    void durationIsPreserved_data();
    void durationIsPreserved();
    void pitchMovesByTheExpectedRatio_data();
    void pitchMovesByTheExpectedRatio();
    void formantShiftMovesTheEnvelopeNotThePitch();
    void robotGivesAMonotone();
    void presetsAreSane();
    void applyIsOneUndoStepOnTheTargetsOnly();
};

void TestVoiceChanger::identityLeavesAudioUntouched() {
    const std::vector<float> in = vowel(150, 0.3);
    QCOMPARE(processVoice(in, kRate, VoiceSettings{}), in);
}

void TestVoiceChanger::durationIsPreserved_data() {
    QTest::addColumn<float>("pitch");
    QTest::addColumn<float>("formant");
    QTest::addColumn<bool>("robot");
    QTest::newRow("down 12") << -12.0f << 0.0f << false;
    QTest::newRow("up 7") << 7.0f << 0.0f << false;
    QTest::newRow("up 12, formant 12") << 12.0f << 12.0f << false;
    QTest::newRow("robot") << 0.0f << 0.0f << true;
}

void TestVoiceChanger::durationIsPreserved() {
    QFETCH(float, pitch);
    QFETCH(float, formant);
    QFETCH(bool, robot);
    // 0.5 s silence, 1.0 s tone, 0.5 s silence.
    std::vector<float> in(size_t(2.0 * kRate), 0.0f);
    const std::vector<float> tone = vowel(160, 1.0);
    std::copy(tone.begin(), tone.end(), in.begin() + long(0.5 * kRate));
    const std::vector<float> out = processVoice(in, kRate, {pitch, formant, robot});

    // The processed audio is exactly as long as the input, sample for sample...
    QCOMPARE(out.size(), in.size());
    // ...and the sound stays where it was: its loudness contour lines up.
    const double shift = envelopeLag(in, out);
    const auto [inStart, inEnd] = halfLevelEdges(in, rms(in, size_t(0.8 * kRate), size_t(0.4 * kRate)));
    const auto [outStart, outEnd] = halfLevelEdges(out, rms(out, size_t(0.8 * kRate), size_t(0.4 * kRate)));
    qInfo("contour offset %+.1f ms; tone edges %.4f..%.4f s -> %.4f..%.4f s (%+.1f / %+.1f ms)", shift * 1000,
          inStart, inEnd, outStart, outEnd, (outStart - inStart) * 1000, (outEnd - inEnd) * 1000);
    QVERIFY(std::fabs(shift) < 0.005); // robot output is an 86 Hz pulse train, so its contour ripples
    // Onsets and ends blur by up to half an STFT frame (46 ms at 44.1 kHz),
    // the usual phase-vocoder smear; they must stay well inside that.
    QVERIFY(std::fabs(outStart - inStart) < 0.015);
    QVERIFY(std::fabs(outEnd - inEnd) < 0.015);
}

void TestVoiceChanger::pitchMovesByTheExpectedRatio_data() {
    QTest::addColumn<QString>("signal");
    QTest::addColumn<double>("f0");
    QTest::addColumn<float>("semitones");
    for (float st : {-12.0f, -7.0f, -3.0f, 3.0f, 7.0f, 12.0f}) {
        QTest::newRow(qPrintable(QString("sine 220 %1").arg(st))) << "sine" << 220.0 << st;
        QTest::newRow(qPrintable(QString("vowel 150 %1").arg(st))) << "vowel" << 150.0 << st;
    }
}

void TestVoiceChanger::pitchMovesByTheExpectedRatio() {
    QFETCH(QString, signal);
    QFETCH(double, f0);
    QFETCH(float, semitones);
    const std::vector<float> in = signal == "sine" ? sine(f0, 1.0) : vowel(f0, 1.0);
    const double measuredIn = yinF0(middle(in));
    QVERIFY(std::fabs(measuredIn / f0 - 1.0) < 0.005);

    const std::vector<float> out = processVoice(in, kRate, {semitones, 0.0f, false});
    const double expected = f0 * std::pow(2.0, semitones / 12.0);
    const double measured = yinF0(middle(out));
    const double error = measured / expected - 1.0;
    qInfo("%s %.0f Hz %+g st: expected %.2f Hz, measured %.2f Hz (%+.2f%%)", qPrintable(signal), f0,
          double(semitones), expected, measured, error * 100);
    QVERIFY(std::fabs(error) < 0.015);
}

void TestVoiceChanger::formantShiftMovesTheEnvelopeNotThePitch() {
    const std::vector<float> in = vowel(140, 1.0);
    const double centroidIn = spectralCentroid(middle(in, 0.3));
    for (float formant : {4.0f, -4.0f}) {
        const std::vector<float> out = processVoice(in, kRate, {0.0f, formant, false});
        const double f0 = yinF0(middle(out));
        const double centroid = spectralCentroid(middle(out, 0.3));
        qInfo("formant %+g st: f0 %.2f Hz (was 140), spectral centroid %.0f -> %.0f Hz (x%.3f, ideal x%.3f)",
              double(formant), f0, centroidIn, centroid, centroid / centroidIn, std::pow(2.0, formant / 12.0));
        QVERIFY(std::fabs(f0 / 140.0 - 1.0) < 0.015); // pitch stays
        if (formant > 0) {
            QVERIFY(centroid > centroidIn * 1.12);
        } else {
            QVERIFY(centroid < centroidIn / 1.12);
        }
    }
    // And with pitch preserved-formant shifting, the envelope stays put.
    const std::vector<float> shifted = processVoice(in, kRate, {5.0f, 0.0f, false});
    const double ratio = spectralCentroid(middle(shifted, 0.3)) / centroidIn;
    qInfo("pitch +5 st, formant 0: centroid x%.3f (plain resampling would give x%.3f)", ratio, std::pow(2.0, 5 / 12.0));
    QVERIFY(ratio > 0.9 && ratio < 1.12);
}

void TestVoiceChanger::robotGivesAMonotone() {
    // Zeroed phases every hop: the pitch becomes sampleRate / hop, whatever
    // the input's was.
    const double hopRate = kRate / double(voiceFrameSize(kRate) / 4);
    for (double f0 : {130.0, 210.0}) {
        const std::vector<float> out = processVoice(vowel(f0, 1.0), kRate, voicePresetSettings(VoicePreset::Robot));
        const double measured = yinF0(middle(out));
        qInfo("robot from %.0f Hz: %.2f Hz (hop rate %.2f Hz)", f0, measured, hopRate);
        QVERIFY(std::fabs(measured / hopRate - 1.0) < 0.02);
    }
}

void TestVoiceChanger::presetsAreSane() {
    const VoiceSettings deeper = voicePresetSettings(VoicePreset::Deeper);
    const VoiceSettings higher = voicePresetSettings(VoicePreset::Higher);
    const VoiceSettings robot = voicePresetSettings(VoicePreset::Robot);
    const VoiceSettings chipmunk = voicePresetSettings(VoicePreset::Chipmunk);
    for (VoicePreset p : {VoicePreset::Deeper, VoicePreset::Higher, VoicePreset::Robot, VoicePreset::Chipmunk}) {
        const VoiceSettings s = voicePresetSettings(p);
        QVERIFY(!s.isIdentity());
        QVERIFY(std::fabs(s.pitchSemitones) <= 12.0f && std::fabs(s.formantSemitones) <= 12.0f);
        QVERIFY(QString(voicePresetName(p)).size() > 0);
    }
    QVERIFY(deeper.pitchSemitones < 0 && deeper.formantSemitones <= 0 && !deeper.robot);
    QVERIFY(higher.pitchSemitones > 0 && higher.formantSemitones >= 0 && !higher.robot);
    // Natural-sounding shifts move formants less than the pitch...
    QVERIFY(std::fabs(deeper.formantSemitones) < std::fabs(deeper.pitchSemitones));
    QVERIFY(std::fabs(higher.formantSemitones) < std::fabs(higher.pitchSemitones));
    // ...while the chipmunk is the cartoon: higher still, formants riding along.
    QVERIFY(chipmunk.pitchSemitones > higher.pitchSemitones);
    QCOMPARE(chipmunk.formantSemitones, chipmunk.pitchSemitones);
    QVERIFY(robot.robot && robot.pitchSemitones == 0.0f);
    QVERIFY(voicePresetSettings(VoicePreset::Custom).isIdentity());
    QCOMPARE(QString(voicePresetName(VoicePreset::Custom)), QString("Custom"));
}

void TestVoiceChanger::applyIsOneUndoStepOnTheTargetsOnly() {
    Project project;
    project.channels = 2;
    project.sampleRate = kRate;
    for (int t = 0; t < 2; ++t) {
        const std::vector<float> mono = vowel(150 + 50 * t, 0.5);
        std::vector<float> stereo(mono.size() * 2);
        for (size_t i = 0; i < mono.size(); ++i) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
        Track track;
        Clip clip;
        clip.channels = 2;
        clip.samples = SampleBuffer(stereo);
        clip.peaks.build(clip.samples, 2);
        track.clips.push_back(clip);
        project.tracks.push_back(track);
    }
    const SampleBuffer before0 = project.tracks[0].clips[0].samples;
    const SampleBuffer before1 = project.tracks[1].clips[0].samples;
    const int64_t from = 4410, to = 17640; // 0.1 .. 0.4 s on track 0

    QUndoStack stack;
    stack.push(new VoiceChangeCommand(project, {GainTarget{0, from, to}}, voicePresetSettings(VoicePreset::Deeper),
                                      "Voice Changer: Deeper"));
    QCOMPARE(stack.count(), 1);
    QCOMPARE(stack.undoText(), QString("Voice Changer: Deeper"));
    const SampleBuffer& after0 = project.tracks[0].clips[0].samples;
    QCOMPARE(after0.size(), before0.size());
    QCOMPARE(after0[2 * 100], before0[2 * 100]);                 // before the range: untouched
    QCOMPARE(after0[2 * (to + 100)], before0[2 * (to + 100)]);   // after it: untouched
    QVERIFY(after0 != before0);
    QCOMPARE(project.tracks[1].clips[0].samples, before1);       // other track untouched
    // Pitch inside the range went down ~4 semitones.
    std::vector<float> region(size_t(to - from));
    for (size_t i = 0; i < region.size(); ++i) region[i] = after0[2 * (size_t(from) + i)];
    const double f0 = yinF0(std::vector<float>(region.begin() + 2000, region.end() - 2000));
    QVERIFY(std::fabs(f0 / (150.0 * std::pow(2.0, -4.0 / 12.0)) - 1.0) < 0.02);

    stack.undo();
    QCOMPARE(project.tracks[0].clips[0].samples, before0);
    stack.redo();
    QVERIFY(project.tracks[0].clips[0].samples != before0);
}

QTEST_GUILESS_MAIN(TestVoiceChanger)
#include "test_voicechanger.moc"
