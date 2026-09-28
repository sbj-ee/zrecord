#include <QtTest>

#include <atomic>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

#include "Meter.h"

using namespace zrecord;

namespace {

constexpr double kRate = 44100.0;

// `cycles` whole cycles of a sine (so RMS is exact), interleaved.
std::vector<float> sine(float amplitude, int channels, int cycles = 100, double hz = 441.0) {
    const size_t frames = size_t(std::lround(cycles * kRate / hz));
    std::vector<float> s(frames * size_t(channels));
    for (size_t i = 0; i < frames; ++i) {
        const float v = amplitude * float(std::sin(2.0 * M_PI * hz * double(i) / kRate));
        for (int c = 0; c < channels; ++c) s[i * size_t(channels) + size_t(c)] = v;
    }
    return s;
}

std::vector<float> square(float amplitude, size_t frames) {
    std::vector<float> s(frames);
    for (size_t i = 0; i < frames; ++i) s[i] = (i / 50) % 2 ? -amplitude : amplitude;
    return s;
}

MeterBlock measure(const std::vector<float>& samples, int channels) {
    MeterQueue q;
    q.reset(4);
    MeterFeed feed;
    feed.reset(channels);
    feed.addSignal(samples.data(), samples.size() / size_t(channels));
    feed.publish(q);
    MeterBlock b;
    q.tryPop(b);
    return b;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

} // namespace

class TestMeter : public QObject {
    Q_OBJECT

private slots:
    void rmsOfKnownSignals();
    void rmsIsExactAcrossBlockBoundaries();
    void clipFlagsUseRunsOrOvershoot();
    void inputPeakIsMeasuredSeparately();
    void peakHoldTimingAndDecay();
    void decayRateIsAdjustable();
    void rangeSetsTheFloor();
    void queueKeepsOrderUnderLoad();
    void feedNeverDropsUnderLoad();
};

void TestMeter::rmsOfKnownSignals() {
    // Sine at 0.5 peak (-6.02 dBFS): RMS is peak / sqrt(2), i.e. -9.03 dB.
    const MeterBlock s = measure(sine(0.5f, 1), 1);
    QVERIFY(near(linearToDb(s.peak[0]), -6.02, 0.01));
    QVERIFY2(near(linearToDb(s.rms(0)), -9.03, 0.01), qPrintable(QString::number(linearToDb(s.rms(0)))));

    // Square wave: RMS equals the peak.
    const MeterBlock q = measure(square(0.5f, 44100), 1);
    QVERIFY(near(linearToDb(q.rms(0)), linearToDb(q.peak[0]), 1e-6));
    QVERIFY(near(linearToDb(q.rms(0)), -6.02, 0.01));

    // Per channel: left sine at -6, right square at -20.
    std::vector<float> st = sine(0.5f, 2);
    const float r = float(std::pow(10.0, -20.0 / 20.0));
    for (size_t i = 0; i < st.size() / 2; ++i) st[2 * i + 1] = (i / 50) % 2 ? -r : r;
    const MeterBlock b = measure(st, 2);
    QCOMPARE(b.channels, 2);
    QVERIFY(near(linearToDb(b.rms(0)), -9.03, 0.01));
    QVERIFY(near(linearToDb(b.rms(1)), -20.0, 0.01));
    QVERIFY(near(linearToDb(b.peak[1]), -20.0, 0.01));

    // The ballistics show the same numbers.
    MeterBallistics m;
    m.advance(&b, 1, 0);
    QCOMPARE(m.channels(), 2);
    QVERIFY(near(m.channel(0).rmsDb, -9.03, 0.01));
    QVERIFY(near(m.channel(0).peakDb, -6.02, 0.01));
    QVERIFY(near(m.channel(1).rmsDb, -20.0, 0.01));

    // Silence reads the floor.
    const MeterBlock z = measure(std::vector<float>(1000, 0.0f), 1);
    QCOMPARE(z.rms(0), 0.0);
    MeterBallistics quiet;
    quiet.advance(&z, 1, 0);
    QCOMPARE(quiet.channel(0).rmsDb, MeterBallistics::kDefaultFloorDb);
}

void TestMeter::rmsIsExactAcrossBlockBoundaries() {
    // The audio thread measures host-sized blocks; the UI merges a tick's
    // worth. Sums of squares add, so the RMS matches one big measurement.
    const std::vector<float> s = sine(0.3f, 2, 300);
    const MeterBlock whole = measure(s, 2);
    MeterQueue q;
    q.reset(1000);
    MeterFeed feed;
    feed.reset(2);
    size_t frames = s.size() / 2;
    std::mt19937 rng(7);
    for (size_t done = 0; done < frames;) {
        const size_t n = std::min<size_t>(frames - done, 1 + rng() % 700);
        feed.addSignal(s.data() + 2 * done, n);
        feed.publish(q);
        done += n;
    }
    std::vector<MeterBlock> blocks;
    drainMeterQueue(q, blocks);
    QVERIFY(blocks.size() > 10);
    MeterBlock merged;
    for (const auto& b : blocks) merged.merge(b);
    QCOMPARE(merged.frames, whole.frames);
    QCOMPARE(merged.peak[0], whole.peak[0]);
    QVERIFY(near(merged.rms(0), whole.rms(0), 1e-12));
    QVERIFY(near(merged.rms(1), whole.rms(1), 1e-12));
}

void TestMeter::clipFlagsUseRunsOrOvershoot() {
    MeterQueue q;
    q.reset(16);
    MeterFeed feed;
    feed.reset(1);
    MeterBlock b;
    // One or two samples touching full scale: a peak, not a clip.
    const float peaks[] = {0.2f, 1.0f, 0.2f, -1.0f, -1.0f, 0.1f};
    feed.addSignal(peaks, 6);
    feed.publish(q);
    QVERIFY(q.tryPop(b));
    QVERIFY(!b.clipped[0]);
    QCOMPARE(b.peak[0], 1.0f);
    // A run of three, split across two blocks (callbacks): the second clips.
    const float a[] = {0.0f, 1.0f, 1.0f};
    const float c[] = {1.0f, 0.0f};
    feed.addSignal(a, 3);
    feed.publish(q);
    feed.addSignal(c, 2);
    feed.publish(q);
    QVERIFY(q.tryPop(b));
    QVERIFY(!b.clipped[0]);
    QVERIFY(q.tryPop(b));
    QVERIFY(b.clipped[0]);
    // Beyond full scale (float), even once: it will be cut off on export.
    const float over[] = {0.0f, 1.25f, 0.0f};
    feed.addSignal(over, 3);
    feed.publish(q);
    QVERIFY(q.tryPop(b));
    QVERIFY(b.clipped[0]);
    // The ballistics latch it.
    MeterBallistics m;
    m.advance(&b, 1, 0);
    QVERIFY(m.clipLatched());
    m.advance(nullptr, 0, 5000);
    QVERIFY(m.clipLatched());
    m.resetClip();
    QVERIFY(!m.clipLatched());
}

void TestMeter::inputPeakIsMeasuredSeparately() {
    // The raw input (before gain) rides along with what is recorded.
    MeterQueue q;
    q.reset(4);
    MeterFeed feed;
    feed.reset(2);
    const float raw[] = {0.9f, -0.5f, 0.1f, 0.2f};
    const float recorded[] = {0.45f, -0.25f, 0.05f, 0.1f};
    feed.addInput(raw, 2);
    feed.addSignal(recorded, 2);
    feed.publish(q);
    MeterBlock b;
    QVERIFY(q.tryPop(b));
    QCOMPARE(b.inputPeak[0], 0.9f);
    QCOMPARE(b.inputPeak[1], 0.5f);
    QCOMPARE(b.peak[0], 0.45f);
    QCOMPARE(b.peak[1], 0.25f);
    MeterBallistics m;
    m.advance(&b, 1, 0);
    QVERIFY(near(m.channel(0).inputDb, linearToDb(0.9), 1e-4));
}

void TestMeter::peakHoldTimingAndDecay() {
    MeterBallistics m; // 24 dB/s, 1500 ms hold
    const MeterBlock b = steadyMeterBlock(1, 0.5f, 0.25f); // peak -6.02, RMS -12.04
    m.advance(&b, 1, 0);
    QVERIFY(near(m.channel(0).peakDb, -6.02, 0.01));
    QVERIFY(near(m.channel(0).rmsDb, -12.04, 0.01));
    QVERIFY(near(m.channel(0).holdDb, -6.02, 0.01));

    // Silence: bars fall at 24 dB/s; the hold stays for 1500 ms...
    m.advance(nullptr, 0, 500);
    QVERIFY(near(m.channel(0).peakDb, -18.02, 0.01));
    QVERIFY(near(m.channel(0).rmsDb, -24.04, 0.01));
    QVERIFY(near(m.channel(0).holdDb, -6.02, 0.01));
    m.advance(nullptr, 0, 1500);
    QVERIFY(near(m.channel(0).holdDb, -6.02, 0.01));
    // ...then falls at the decay rate too (100 ms after this tick: 2.4 dB).
    m.advance(nullptr, 0, 1600);
    QVERIFY2(near(m.channel(0).holdDb, -8.42, 0.01), qPrintable(QString::number(m.channel(0).holdDb)));
    // A louder peak takes over the hold at once.
    const MeterBlock louder = steadyMeterBlock(1, 0.7f, 0.5f);
    m.advance(&louder, 1, 1650);
    QVERIFY(near(m.channel(0).holdDb, -3.10, 0.01));
    QCOMPARE(m.maxHoldDb(), m.channel(0).holdDb);
    // resetHold drops holds to the current peaks.
    m.advance(nullptr, 0, 2150);
    m.resetHold();
    QCOMPARE(m.channel(0).holdDb, m.channel(0).peakDb);
    // Everything bottoms out at the floor.
    m.advance(nullptr, 0, 60000);
    QCOMPARE(m.channel(0).peakDb, m.floorDb());
    QCOMPARE(m.channel(0).rmsDb, m.floorDb());
    QCOMPARE(m.channel(0).holdDb, m.floorDb());
}

void TestMeter::decayRateIsAdjustable() {
    MeterBallistics m;
    m.setDecayDbPerSecond(48.0f);
    const MeterBlock b = steadyMeterBlock(1, 0.5f, 0.5f);
    m.advance(&b, 1, 0);
    m.advance(nullptr, 0, 250);
    QVERIFY(near(m.channel(0).peakDb, -18.02, 0.01)); // 12 dB in 250 ms
    m.setDecayDbPerSecond(12.0f);
    m.advance(nullptr, 0, 500);
    QVERIFY(near(m.channel(0).peakDb, -21.02, 0.01)); // 3 dB in the next 250 ms
    m.setHoldMs(100);
    m.advance(nullptr, 0, 700);
    QVERIFY(m.channel(0).holdDb < -6.1f); // shorter hold: already falling
}

void TestMeter::rangeSetsTheFloor() {
    MeterBallistics m;
    QCOMPARE(m.floorDb(), -60.0f);
    const MeterBlock quiet = steadyMeterBlock(1, 0.0005f, 0.0005f); // -66 dBFS
    m.advance(&quiet, 1, 0);
    QCOMPARE(m.channel(0).peakDb, -60.0f); // below the -60 floor
    m.setFloorDb(-96.0f);
    m.advance(&quiet, 1, 10);
    QVERIFY(m.channel(0).peakDb < -60.0f); // on its way down from the old floor...
    m.advance(&quiet, 1, 5000);
    QVERIFY(near(m.channel(0).peakDb, -66.02, 0.01)); // ...and settled: visible at -96
    m.advance(nullptr, 0, 60000);
    QCOMPARE(m.channel(0).peakDb, -96.0f);
    m.setFloorDb(-48.0f);
    QCOMPARE(m.channel(0).peakDb, -48.0f); // raised to the new floor
    m.setFloorDb(0.0f);
    QCOMPARE(m.floorDb(), -6.0f); // never a range of nothing
}

void TestMeter::queueKeepsOrderUnderLoad() {
    // Raw SPSC queue: one producer spinning when full, one consumer with
    // random stalls; everything arrives once, in order.
    SpscQueue<uint64_t> q;
    q.reset(64);
    constexpr uint64_t kCount = 2'000'000;
    std::thread producer([&] {
        for (uint64_t i = 1; i <= kCount;) {
            if (q.tryPush(i)) ++i;
        }
    });
    uint64_t expected = 1;
    std::mt19937 rng(1);
    while (expected <= kCount) {
        uint64_t v = 0;
        if (q.tryPop(v)) {
            if (v != expected) {
                producer.join();
                QFAIL(qPrintable(QString("got %1, expected %2").arg(v).arg(expected)));
            }
            ++expected;
        } else if (rng() % 64 == 0) {
            std::this_thread::yield();
        }
    }
    producer.join();
    uint64_t extra = 0;
    QVERIFY(!q.tryPop(extra));
}

void TestMeter::feedNeverDropsUnderLoad() {
    // The real producer path: an "audio thread" measuring and publishing
    // 300k small blocks into a queue of 8, and a UI thread that drains in
    // bursts with pauses. Full queue => merge, never drop: the sequence
    // numbers stay contiguous and in order, and frames, sums of squares,
    // peaks and clips all add up.
    MeterQueue q;
    q.reset(8);
    constexpr int kBlocks = 300'000;
    constexpr size_t kFrames = 16;
    std::atomic<bool> done{false};
    double expectedSumSquares = 0.0;
    float expectedPeak = 0.0f;
    int expectedClips = 0;
    std::thread audio([&] {
        MeterFeed feed;
        feed.reset(1);
        float buf[kFrames];
        for (int i = 0; i < kBlocks; ++i) {
            const float level = float((int64_t(i) * 7919) % 1000) / 1000.0f; // 0 .. 0.999
            for (size_t f = 0; f < kFrames; ++f) buf[f] = (f % 2) ? -level : level;
            if (i % 50'000 == 123) std::fill(std::begin(buf), std::end(buf), 1.0f); // a clip now and then
            feed.addSignal(buf, kFrames);
            feed.publish(q);
            for (float s : buf) expectedSumSquares += double(s) * s;
            expectedPeak = std::max(expectedPeak, std::fabs(buf[0]));
            if (buf[0] == 1.0f) ++expectedClips;
        }
        while (!feed.flush(q)) {
        }
        done = true;
    });
    std::vector<MeterBlock> got;
    std::mt19937 rng(3);
    for (;;) {
        const bool finished = done.load();
        drainMeterQueue(q, got);
        if (finished) {
            drainMeterQueue(q, got);
            break;
        }
        if (rng() % 16 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    audio.join();
    drainMeterQueue(q, got);

    uint64_t next = 1;
    int64_t frames = 0;
    double sumSquares = 0.0;
    float peak = 0.0f;
    int clippedBlocks = 0;
    size_t merged = 0;
    for (const MeterBlock& b : got) {
        if (b.firstSequence != next) {
            QFAIL(qPrintable(QString("block starts at %1, expected %2").arg(b.firstSequence).arg(next)));
        }
        if (b.lastSequence > b.firstSequence) ++merged;
        next = b.lastSequence + 1;
        frames += b.frames;
        sumSquares += b.sumSquares[0];
        peak = std::max(peak, b.peak[0]);
        clippedBlocks += b.clipped[0] ? 1 : 0;
    }
    QCOMPARE(next, uint64_t(kBlocks) + 1);
    QCOMPARE(frames, int64_t(kBlocks) * int64_t(kFrames));
    QVERIFY(std::fabs(sumSquares - expectedSumSquares) <= 1e-9 * expectedSumSquares);
    QCOMPARE(peak, expectedPeak);
    QVERIFY(clippedBlocks >= 1 && clippedBlocks <= expectedClips);
    qInfo("%zu blocks received, %zu of them merged while the queue was full", got.size(), merged);
    QVERIFY(merged > 0); // the full-queue path really ran
}

QTEST_GUILESS_MAIN(TestMeter)
#include "test_meter.moc"
