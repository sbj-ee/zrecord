#include <QtTest>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>

#include "Filters.h"

using namespace zrecord;

// Counts heap allocations while `g_countAllocations` is set, to prove the
// real-time paths don't allocate.
namespace {
std::atomic<bool> g_countAllocations{false};
std::atomic<int> g_allocations{0};
} // namespace

void* operator new(std::size_t size) {
    if (g_countAllocations.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

class TestFilters : public QObject {
    Q_OBJECT

private slots:
    void chainSettingsChangeKeepsStateAndDoesNotAllocate();
    void fadeIn_rampsFromSilenceToUnity();
    void fadeOut_rampsFromUnityToSilence();
    void fade_appliesSameGainToEveryChannel();
    void fade_handlesDegenerateBuffers();
    void limiter_holdsOutputUnderCeiling();
    void noiseGate_passesLoudAndCutsQuiet();
    void chainLimiter_capsGainApplied();
    void chainLimiter_capsEveryVoiceEffect();
};

namespace {

float chainPeak(const FilterSettings& settings, float amplitude, double hz = 440.0) {
    FilterChain chain;
    chain.prepare(48000.0, 2);
    chain.setSettings(settings);
    const size_t frames = 48000;
    std::vector<float> buffer(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        const float v = amplitude * static_cast<float>(std::sin(2.0 * M_PI * hz * f / 48000.0));
        buffer[f * 2] = v;
        buffer[f * 2 + 1] = v;
    }
    chain.process(buffer, frames);
    float peak = 0.0f;
    for (float v : buffer) {
        peak = std::max(peak, std::fabs(v));
    }
    return peak;
}

} // namespace

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

void TestFilters::chainLimiter_capsGainApplied() {
    // Regression: the limiter ran first, so gain applied after it went
    // straight past the ceiling (-1 dB limiter + 12 dB gain gave +6 dBFS).
    FilterSettings settings;
    settings.limiterEnabled = true;
    settings.limiterCeilingDb = -1.0;
    settings.gainEnabled = true;
    settings.gainDb = 12.0;
    const float ceiling = static_cast<float>(std::pow(10.0, -1.0 / 20.0));
    const float peak = chainPeak(settings, 0.5f);
    QVERIFY2(peak <= ceiling + 1e-4f,
             qPrintable(QString("peak %1 exceeded the %2 ceiling").arg(peak).arg(ceiling)));
}

void TestFilters::chainLimiter_capsEveryVoiceEffect() {
    // Nothing downstream of the limiter may push the signal back over it:
    // echo adds a delayed copy, distortion normalises to full scale.
    const float ceiling = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    for (VoiceEffect effect : {VoiceEffect::Robot, VoiceEffect::Echo, VoiceEffect::DeepVoice,
                               VoiceEffect::Chipmunk, VoiceEffect::Distortion}) {
        FilterSettings settings;
        settings.limiterEnabled = true;
        settings.limiterCeilingDb = -6.0;
        settings.gainEnabled = true;
        settings.gainDb = 6.0;
        settings.compressorEnabled = true;
        settings.voiceEffect = effect;
        const float peak = chainPeak(settings, 0.9f, 220.0);
        QVERIFY2(peak <= ceiling + 1e-4f,
                 qPrintable(QString("effect %1: peak %2 exceeded the %3 ceiling")
                                .arg(static_cast<int>(effect))
                                .arg(peak)
                                .arg(ceiling)));
    }
}

void TestFilters::chainSettingsChangeKeepsStateAndDoesNotAllocate() {
    // Regression: every slider tick rebuilt the whole chain from the audio
    // callback -- reallocating the echo line and resetting the gate, echo and
    // filters -- so nudging even a disabled control mid-take cut the echo
    // tail and re-attacked the gate from silence (repro_settings).
    FilterSettings settings;
    settings.noiseGateEnabled = true;
    settings.noiseGateThresholdDb = -40.0;
    settings.voiceEffect = VoiceEffect::Echo;
    FilterChain chain;
    chain.prepare(48000.0, 1);
    chain.setSettings(settings);

    const size_t block = 256;
    std::vector<float> buffer(block);
    size_t n = 0;
    auto runBlock = [&] {
        for (size_t i = 0; i < block; ++i, ++n) {
            buffer[i] = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * 440.0 * n / 48000.0));
        }
        chain.process(buffer, block);
        float peak = 0.0f;
        for (float v : buffer) {
            peak = std::max(peak, std::fabs(v));
        }
        return peak;
    };
    float steady = 0.0f;
    for (int b = 0; b < 200; ++b) {
        steady = runBlock(); // echo tail built up, gate open
    }

    settings.compressorThresholdDb = -21.0; // an unrelated, disabled control
    settings.highPassHz = 120.0;            // and an enabled-later one
    g_allocations = 0;
    g_countAllocations = true;
    chain.setSettings(settings);
    g_countAllocations = false;
    QCOMPARE(g_allocations.load(), 0);

    const float after = runBlock();
    QVERIFY2(std::fabs(after - steady) < 0.05f,
             qPrintable(QString("peak jumped from %1 to %2 after a settings change").arg(steady).arg(after)));
}

QTEST_GUILESS_MAIN(TestFilters)
#include "test_filters.moc"
