#include <QtTest>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <random>
#include <thread>

#include "Effects.h"

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
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// The per-track effect stack: the effects themselves, bypass, order, and the
// lock-free parameter handoff to the audio thread.
class TestEffects : public QObject {
    Q_OBJECT

private slots:
    void typesKeysAndDefaults();
    void paramsAreClamped();
    void everyEffectChangesTheSignalAndBypassRestoresIt();
    void orderMatters();
    void limiterCapsEveryEffectBeforeIt();
    void parameterChangesDoNotAllocateOrResetState();
    void echoDelayChangeWithinTheReserveDoesNotAllocate();
    void bypassRestartsFromRest();
    void structureIsTypesInOrder();
    void publishIsSafeWhileTheAudioThreadRenders();
};

namespace {
constexpr double kRate = 48000.0;

// A stereo test signal: two sines plus a little noise, different per channel.
std::vector<float> testSignal(size_t frames, float amplitude = 0.5f) {
    std::vector<float> s(frames * 2);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-0.02f, 0.02f);
    for (size_t f = 0; f < frames; ++f) {
        const double t = double(f) / kRate;
        s[2 * f] = amplitude * float(0.7 * std::sin(2 * M_PI * 220 * t) + 0.3 * std::sin(2 * M_PI * 3100 * t)) + noise(rng);
        s[2 * f + 1] = amplitude * float(0.6 * std::sin(2 * M_PI * 330 * t)) + noise(rng);
    }
    return s;
}

std::vector<float> run(const std::vector<Effect>& effects, std::vector<float> signal, size_t block = 256) {
    EffectStack stack;
    stack.prepare(effects, kRate, 2);
    const size_t frames = signal.size() / 2;
    for (size_t done = 0; done < frames; done += block) {
        const size_t n = std::min(block, frames - done);
        stack.update();
        stack.process(signal.data() + 2 * done, n);
    }
    return signal;
}

float peakOf(const std::vector<float>& v) {
    float p = 0.0f;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}

// Every effect with settings that make it clearly audible.
Effect audible(EffectType type) {
    Effect e = Effect::make(type);
    switch (type) {
        case EffectType::Gain: e.params[0] = 6.0; break;
        case EffectType::HighPass: e.params[0] = 1000.0; break;
        case EffectType::LowPass: e.params[0] = 500.0; break;
        case EffectType::NoiseGate: e.params[0] = -6.0; break;
        case EffectType::Compressor: e.params[0] = -30.0; e.params[1] = 6.0; break;
        case EffectType::Limiter: e.params[0] = -12.0; break;
        default: break;
    }
    return e;
}
} // namespace

void TestEffects::typesKeysAndDefaults() {
    for (int i = 0; i < kEffectTypeCount; ++i) {
        const auto type = static_cast<EffectType>(i);
        const EffectInfo& info = effectInfo(type);
        QCOMPARE(info.type, type);
        EffectType back;
        QVERIFY(effectTypeFromKey(info.key, back));
        QCOMPARE(back, type);
        const Effect e = Effect::make(type);
        QCOMPARE(e.type, type);
        QVERIFY(!e.bypassed);
        for (int p = 0; p < info.paramCount; ++p) {
            QCOMPARE(e.param(p), info.params[p].defaultValue);
            QVERIFY(info.params[p].minValue <= info.params[p].defaultValue &&
                    info.params[p].defaultValue <= info.params[p].maxValue);
        }
    }
    EffectType unused;
    QVERIFY(!effectTypeFromKey("reverb", unused));
    QCOMPARE(Effect::make(EffectType::Echo).param(0), 280.0);
    QCOMPARE(describeEffects({Effect::make(EffectType::Echo), [] {
                                  Effect g = Effect::make(EffectType::Gain);
                                  g.bypassed = true;
                                  return g;
                              }()}),
             std::string("Echo, (Gain)"));
}

void TestEffects::paramsAreClamped() {
    Effect e = Effect::make(EffectType::Echo);
    e.params = {99999.0, -1.0, std::nan("")};
    e.clampParams();
    QCOMPARE(e.param(0), EffectProcessor::kMaxEchoDelayMs);
    QCOMPARE(e.param(1), 0.0);
    QCOMPARE(e.param(2), 0.5); // not a number: the default
    Effect g = Effect::make(EffectType::DeepVoice);
    g.params = {1.0, 2.0, 3.0};
    g.clampParams();
    QCOMPARE(g.param(0), 0.0); // no parameters
}

void TestEffects::everyEffectChangesTheSignalAndBypassRestoresIt() {
    const std::vector<float> dry = testSignal(24000);
    for (int i = 0; i < kEffectTypeCount; ++i) {
        const Effect effect = audible(static_cast<EffectType>(i));
        const std::vector<float> wet = run({effect}, dry);
        double diff = 0.0;
        for (size_t k = 0; k < dry.size(); ++k) diff += std::fabs(wet[k] - dry[k]);
        QVERIFY2(diff / double(dry.size()) > 1e-3, effectInfo(effect.type).name);
        Effect bypassed = effect;
        bypassed.bypassed = true;
        QVERIFY2(run({bypassed}, dry) == dry, effectInfo(effect.type).name); // bit-exact
    }
    QVERIFY(run({}, dry) == dry);
}

void TestEffects::orderMatters() {
    // Gain then Limiter: capped at the ceiling. Limiter then Gain: the gain
    // pushes the limited signal 12 dB past it.
    Effect gain = Effect::make(EffectType::Gain);
    gain.params[0] = 12.0;
    Effect limiter = Effect::make(EffectType::Limiter);
    limiter.params[0] = -6.0;
    const std::vector<float> dry = testSignal(48000, 0.9f); // hot enough to hit the ceiling
    const float ceiling = float(std::pow(10.0, -6.0 / 20.0));
    const float gainFirst = peakOf(run({gain, limiter}, dry));
    const float limiterFirst = peakOf(run({limiter, gain}, dry));
    QVERIFY2(gainFirst <= ceiling + 1e-4f, qPrintable(QString::number(gainFirst)));
    QVERIFY2(limiterFirst > ceiling * 3.9f, qPrintable(QString::number(limiterFirst)));

    // And for effects that don't commute in a less obvious way.
    Effect echo = Effect::make(EffectType::Echo);
    Effect gate = audible(EffectType::NoiseGate);
    QVERIFY(run({echo, gate}, dry) != run({gate, echo}, dry));
}

void TestEffects::limiterCapsEveryEffectBeforeIt() {
    // Nothing before the limiter can get past it: echo adds a delayed copy,
    // distortion normalises to full scale, gain adds 6 dB.
    const float ceiling = float(std::pow(10.0, -6.0 / 20.0));
    Effect limiter = Effect::make(EffectType::Limiter);
    limiter.params[0] = -6.0;
    Effect gain = Effect::make(EffectType::Gain);
    gain.params[0] = 6.0;
    for (EffectType type : {EffectType::Robot, EffectType::Echo, EffectType::DeepVoice, EffectType::Chipmunk,
                            EffectType::Distortion, EffectType::Compressor}) {
        const float peak = peakOf(run({gain, Effect::make(type), limiter}, testSignal(48000, 0.9f)));
        QVERIFY2(peak <= ceiling + 1e-4f, qPrintable(QString("%1: %2").arg(effectInfo(type).name).arg(peak)));
    }
}

void TestEffects::parameterChangesDoNotAllocateOrResetState() {
    // Every effect in one stack; change every parameter and bypass flag
    // between blocks. Neither the UI side (publish) nor the audio side
    // (update + process) may allocate.
    std::vector<Effect> effects;
    for (int i = 0; i < kEffectTypeCount; ++i) effects.push_back(audible(static_cast<EffectType>(i)));
    EffectStack stack;
    stack.prepare(effects, kRate, 2);
    std::vector<float> signal = testSignal(256 * 400);
    std::mt19937 rng(3);
    g_allocations = 0;
    g_countAllocations = true;
    for (size_t b = 0; b < 400; ++b) {
        if (b % 3 == 0) {
            for (Effect& e : effects) {
                const EffectInfo& info = effectInfo(e.type);
                for (int p = 0; p < info.paramCount; ++p) {
                    std::uniform_real_distribution<double> d(info.params[p].minValue, info.params[p].maxValue);
                    e.params[size_t(p)] = d(rng);
                }
                e.bypassed = rng() % 4 == 0;
            }
            stack.publish(effects);
        }
        stack.update();
        stack.process(signal.data() + b * 512, 256);
    }
    g_countAllocations = false;
    QCOMPARE(g_allocations.load(), 0);

    // State is kept across a parameter change: an echo's tail is still there.
    EffectStack echo;
    std::vector<Effect> e = {Effect::make(EffectType::Echo)};
    echo.prepare(e, kRate, 2);
    std::vector<float> impulse(2 * 4800, 0.0f);
    impulse[0] = impulse[1] = 1.0f;
    echo.update();
    echo.process(impulse.data(), 4800); // 100 ms: the 280 ms echo hasn't come yet
    e[0].params[2] = 1.0;                // mix 0.5 -> 1.0
    echo.publish(e);
    std::vector<float> later(2 * 14400, 0.0f);
    echo.update();
    echo.process(later.data(), 14400);
    const size_t at = size_t(std::lround(0.280 * kRate)) - 4800;
    QVERIFY2(std::fabs(later[2 * at] - 1.0f) < 1e-6f, qPrintable(QString::number(later[2 * at])));
}

void TestEffects::echoDelayChangeWithinTheReserveDoesNotAllocate() {
    EchoEffect echo;
    echo.reserve(kRate, 2000.0);
    echo.configure(kRate, 280.0, 0.35, 0.5);
    g_allocations = 0;
    g_countAllocations = true;
    echo.configure(kRate, 1999.0, 0.5, 0.5);
    echo.configure(kRate, 20.0, 0.5, 0.5);
    for (int i = 0; i < 1000; ++i) echo.process(0.1f);
    g_countAllocations = false;
    QCOMPARE(g_allocations.load(), 0);
}

void TestEffects::bypassRestartsFromRest() {
    // An echo taken out of bypass doesn't replay what was in its line from
    // before it was bypassed.
    std::vector<Effect> e = {Effect::make(EffectType::Echo)};
    EffectStack stack;
    stack.prepare(e, kRate, 1 + 1);
    std::vector<float> loud(2 * 4800, 0.8f);
    stack.update();
    stack.process(loud.data(), 4800);
    e[0].bypassed = true;
    stack.publish(e);
    QVERIFY(!stack.update());
    e[0].bypassed = false;
    stack.publish(e);
    QVERIFY(stack.update());
    std::vector<float> silence(2 * 48000, 0.0f);
    stack.process(silence.data(), 48000);
    QCOMPARE(peakOf(silence), 0.0f);
}

void TestEffects::structureIsTypesInOrder() {
    EffectStack stack;
    const std::vector<Effect> ab = {Effect::make(EffectType::Gain), Effect::make(EffectType::Echo)};
    stack.prepare(ab, kRate, 2);
    QVERIFY(stack.sameStructure(ab));
    std::vector<Effect> tweaked = ab;
    tweaked[1].params[0] = 500.0;
    tweaked[0].bypassed = true;
    QVERIFY(stack.sameStructure(tweaked)); // parameters and bypass don't count
    QVERIFY(!stack.sameStructure({ab[1], ab[0]}));
    QVERIFY(!stack.sameStructure({ab[0]}));
    QCOMPARE(stack.size(), size_t(2));
}

void TestEffects::publishIsSafeWhileTheAudioThreadRenders() {
    // The UI thread publishes as fast as it can while the "audio thread"
    // renders; afterwards the last published values are in effect. (Run
    // under TSan to check the handoff for races.)
    std::vector<Effect> effects = {Effect::make(EffectType::Gain), Effect::make(EffectType::Echo)};
    EffectStack stack;
    stack.prepare(effects, kRate, 2);
    std::atomic<bool> stop{false};
    std::thread audio([&] {
        std::vector<float> block(512, 0.1f);
        while (!stop.load()) {
            stack.update();
            stack.process(block.data(), 256);
        }
    });
    for (int i = 0; i < 20000; ++i) {
        effects[0].params[0] = double(i % 48) - 24.0;
        effects[1].params[0] = 20.0 + double(i % 1000);
        effects[1].bypassed = (i % 7) == 0;
        stack.publish(effects);
    }
    stop = true;
    audio.join();
    effects[0].params[0] = 12.0;
    effects[1].bypassed = true;
    stack.publish(effects);
    stack.update();
    std::vector<float> block(2, 0.25f);
    stack.process(block.data(), 1);
    QCOMPARE(block[0], 0.25f * float(std::pow(10.0, 12.0 / 20.0)));
}

QTEST_GUILESS_MAIN(TestEffects)
#include "test_effects.moc"
