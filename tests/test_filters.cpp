#include <QtTest>

#include <cmath>

#include "Filters.h"

using namespace zrecord;

class TestFilters : public QObject {
    Q_OBJECT

private slots:
    void fadeIn_rampsFromSilenceToUnity();
    void fadeOut_rampsFromUnityToSilence();
    void fade_appliesSameGainToEveryChannel();
    void fade_handlesDegenerateBuffers();
    void limiter_holdsOutputUnderCeiling();
    void noiseGate_passesLoudAndCutsQuiet();
};

void TestFilters::fadeIn_rampsFromSilenceToUnity() {
    std::vector<float> samples(5, 1.0f);

    applyLinearFade(samples, 1, FadeShape::In);

    QCOMPARE(samples.front(), 0.0f);
    // The ramp must actually reach unity on the last frame, not stop short.
    QCOMPARE(samples.back(), 1.0f);
    QCOMPARE(samples[2], 0.5f);
    for (size_t i = 1; i < samples.size(); ++i) {
        QVERIFY(samples[i] > samples[i - 1]);
    }
}

void TestFilters::fadeOut_rampsFromUnityToSilence() {
    std::vector<float> samples(5, 1.0f);

    applyLinearFade(samples, 1, FadeShape::Out);

    QCOMPARE(samples.front(), 1.0f);
    QCOMPARE(samples.back(), 0.0f);
    QCOMPARE(samples[2], 0.5f);
}

void TestFilters::fade_appliesSameGainToEveryChannel() {
    // Stereo, both channels identical, so any per-channel drift is visible.
    std::vector<float> samples(8, 1.0f);

    applyLinearFade(samples, 2, FadeShape::In);

    for (size_t frame = 0; frame < 4; ++frame) {
        QCOMPARE(samples[frame * 2], samples[frame * 2 + 1]);
    }
    QCOMPARE(samples[0], 0.0f);
    QCOMPARE(samples[6], 1.0f);
}

void TestFilters::fade_handlesDegenerateBuffers() {
    std::vector<float> empty;
    applyLinearFade(empty, 1, FadeShape::In); // must not crash
    QVERIFY(empty.empty());

    std::vector<float> single{0.7f};
    applyLinearFade(single, 1, FadeShape::In);
    QCOMPARE(single[0], 0.7f); // a one-frame fade has nowhere to ramp

    std::vector<float> zeroChannels{1.0f};
    applyLinearFade(zeroChannels, 0, FadeShape::In);
    QCOMPARE(zeroChannels[0], 1.0f);
}

void TestFilters::limiter_holdsOutputUnderCeiling() {
    Limiter limiter;
    limiter.configure(44100.0, -6.0, 50.0);

    const float ceiling = 0.5012f; // -6 dB, with a little slack for rounding
    float maxSeen = 0.0f;
    for (int i = 0; i < 1000; ++i) {
        maxSeen = std::max(maxSeen, std::fabs(limiter.process(1.0f)));
    }
    QVERIFY2(maxSeen <= ceiling + 1e-3f,
             qPrintable(QString("peak %1 exceeded ceiling %2").arg(maxSeen).arg(ceiling)));
}

void TestFilters::noiseGate_passesLoudAndCutsQuiet() {
    NoiseGate gate;
    gate.configure(44100.0, -40.0, 1.0, 1.0);

    // Well above the threshold: should settle at roughly unity gain.
    float loud = 0.0f;
    for (int i = 0; i < 4410; ++i) {
        loud = gate.process(0.5f);
    }
    QVERIFY2(loud > 0.4f, qPrintable(QString("loud signal was gated to %1").arg(loud)));

    // Well below it: should be pulled down towards silence.
    float quiet = 0.0f;
    for (int i = 0; i < 4410; ++i) {
        quiet = gate.process(0.0001f);
    }
    QVERIFY2(std::fabs(quiet) < 0.0001f, qPrintable(QString("quiet signal leaked at %1").arg(quiet)));
}

QTEST_GUILESS_MAIN(TestFilters)
#include "test_filters.moc"
