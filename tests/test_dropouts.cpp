#include <QtTest>

#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "Dropouts.h"
#include "RingBuffer.h"

using namespace zrecord;

// Dropout detection: the capture writer pads lost input with silence so the
// take keeps its timeline, and logs each loss (see Dropouts.h).
class TestDropouts : public QObject {
    Q_OBJECT

private slots:
    void ringOverrunIsPaddedAndLogged();
    void intervalMathHoldsAcrossRandomBuffers();
    void hostOverflowIsMeasuredFromAdcTime();
    void hostOverflowWithoutUsableTimingHasUnknownLength();
    void adcJumpAloneIsNotADropout();
    void hostLossAndRingLossRunTogether();
    void fullLogCountsWhatItCannotKeep();
    void logIsSafeToReadWhileWriting();
    void nearbyLossesMergeIntoOneLabel();
    void labelAndSummaryText();
};

namespace {
// A block of `frames` mono frames whose values are their timeline position + 1
// (never 0, so silence padding is unmistakable).
std::vector<float> ramp(int64_t firstFrame, size_t frames) {
    std::vector<float> v(frames);
    for (size_t i = 0; i < frames; ++i) v[i] = float(firstFrame + int64_t(i) + 1);
    return v;
}

bool inAnyInterval(const std::vector<LostInterval>& intervals, int64_t frame) {
    for (const LostInterval& iv : intervals) {
        if (frame >= iv.startFrame && frame < iv.endFrame()) return true;
    }
    return false;
}
} // namespace

void TestDropouts::ringOverrunIsPaddedAndLogged() {
    RingBuffer ring;
    ring.reset(1000);
    CaptureWriter w;
    w.reset(&ring, 1, 1000.0, 16, 64);
    std::vector<float> out;
    int64_t pos = 0;
    for (int b = 0; b < 10; ++b, pos += 100) QVERIFY(w.write(ramp(pos, 100).data(), 100)); // exactly fills it
    QVERIFY(!w.write(ramp(pos, 100).data(), 100)); // lost: the ring is full
    pos += 100;
    QVERIFY(!w.write(ramp(pos, 100).data(), 100)); // and the next one, same outage
    pos += 100;
    QCOMPARE(w.owedFrames(), int64_t(200));
    ring.readAll(out); // the UI catches up
    QVERIFY(w.write(ramp(pos, 100).data(), 100)); // pays 200 frames of silence first
    pos += 100;
    ring.readAll(out);
    QCOMPARE(w.finish(), int64_t(0));
    QCOMPARE(w.timelineFrames(), pos);
    QCOMPARE(w.lostFrames(), int64_t(200));
    QCOMPARE(int64_t(out.size()), pos);
    for (int64_t i = 0; i < pos; ++i) {
        const float expected = (i >= 1000 && i < 1200) ? 0.0f : float(i + 1);
        QCOMPARE(out[size_t(i)], expected);
    }
    const std::vector<LostInterval> log = w.log().intervals();
    QCOMPARE(log.size(), size_t(1));
    QCOMPARE(log[0].startFrame, int64_t(1000));
    QCOMPARE(log[0].frames, int64_t(200));
    QCOMPARE(log[0].causes, uint32_t(kDropoutRingOverrun));
    QCOMPARE(log[0].events, 1);
}

void TestDropouts::intervalMathHoldsAcrossRandomBuffers() {
    // Random block sizes, a reader that sometimes stalls, stereo: whatever
    // is lost, the take is audio at its own timeline position or silence
    // exactly where the log says, and the lengths add up.
    std::mt19937 rng(1234);
    for (int round = 0; round < 20; ++round) {
        RingBuffer ring;
        ring.reset(2 * 700);
        CaptureWriter w;
        w.reset(&ring, 2, 48000.0, 4096, 50);
        std::vector<float> out;
        int64_t pos = 0;
        for (int b = 0; b < 400; ++b) {
            const size_t frames = 1 + rng() % 180;
            std::vector<float> block(frames * 2);
            for (size_t i = 0; i < frames; ++i) block[2 * i] = block[2 * i + 1] = float(pos + int64_t(i) + 1);
            w.write(block.data(), frames);
            pos += int64_t(frames);
            if (rng() % 4 == 0) ring.readAll(out); // a slow reader
        }
        ring.readAll(out);
        const int64_t owed = w.finish();
        out.insert(out.end(), size_t(owed) * 2, 0.0f); // what the engine appends at Stop
        const std::vector<LostInterval> log = w.log().intervals();
        QVERIFY(!log.empty());
        QCOMPARE(int64_t(out.size() / 2), pos);
        QCOMPARE(w.timelineFrames(), pos);
        int64_t lost = 0, prevEnd = -1;
        for (const LostInterval& iv : log) {
            QVERIFY(iv.frames > 0);
            QVERIFY(iv.startFrame > prevEnd); // sorted, separate (contiguous ones merge)
            QCOMPARE(iv.causes, uint32_t(kDropoutRingOverrun));
            prevEnd = iv.endFrame();
            lost += iv.frames;
        }
        QCOMPARE(lost, w.lostFrames());
        for (int64_t i = 0; i < pos; ++i) {
            const float expected = inAnyInterval(log, i) ? 0.0f : float(i + 1);
            if (out[size_t(2 * i)] != expected || out[size_t(2 * i + 1)] != expected) {
                QFAIL(qPrintable(QString("round %1 frame %2: %3, expected %4")
                                     .arg(round).arg(i).arg(out[size_t(2 * i)]).arg(expected)));
            }
        }
    }
}

void TestDropouts::hostOverflowIsMeasuredFromAdcTime() {
    RingBuffer ring;
    ring.reset(10000);
    CaptureWriter w;
    w.reset(&ring, 1, 1000.0, 16, 64);
    w.beginCallback(100, false, 5.0);
    QVERIFY(w.write(ramp(0, 100).data(), 100));
    // The next callback should start at 5.1 s; it starts at 5.15 s and the
    // host flags an overflow: 50 frames never arrived.
    w.beginCallback(100, true, 5.15);
    QVERIFY(w.write(ramp(150, 100).data(), 100));
    w.beginCallback(100, false, 5.25);
    QVERIFY(w.write(ramp(250, 100).data(), 100));
    std::vector<float> out;
    ring.readAll(out);
    QCOMPARE(w.finish(), int64_t(0));
    QCOMPARE(out.size(), size_t(350));
    for (int64_t i = 0; i < 350; ++i) {
        QCOMPARE(out[size_t(i)], (i >= 100 && i < 150) ? 0.0f : float(i + 1));
    }
    const std::vector<LostInterval> log = w.log().intervals();
    QCOMPARE(log.size(), size_t(1));
    QCOMPARE(log[0].startFrame, int64_t(100));
    QCOMPARE(log[0].frames, int64_t(50));
    QCOMPARE(log[0].causes, uint32_t(kDropoutInputOverflow));
}

void TestDropouts::hostOverflowWithoutUsableTimingHasUnknownLength() {
    RingBuffer ring;
    ring.reset(10000);
    CaptureWriter w;
    w.reset(&ring, 1, 1000.0, 16, 64);
    w.beginCallback(100, true, 0.0); // first callback: nothing to measure against
    w.write(ramp(0, 100).data(), 100);
    w.beginCallback(100, false, 0.0);
    w.write(ramp(100, 100).data(), 100);
    w.beginCallback(100, true, 0.0); // host gives no timestamps
    w.write(ramp(200, 100).data(), 100);
    w.beginCallback(100, false, 1.0);
    w.write(ramp(300, 100).data(), 100);
    w.beginCallback(100, true, 30.0); // a 29 s "gap" is a bogus timestamp
    w.write(ramp(400, 100).data(), 100);
    w.beginCallback(100, true, 30.1004); // under 1 ms off: jitter, not a length
    w.write(ramp(500, 100).data(), 100);
    QCOMPARE(w.finish(), int64_t(0));
    const std::vector<LostInterval> log = w.log().intervals();
    QCOMPARE(log.size(), size_t(4));
    const int64_t at[] = {0, 200, 400, 500};
    for (size_t i = 0; i < 4; ++i) {
        QCOMPARE(log[i].startFrame, at[i]);
        QVERIFY(log[i].lengthUnknown());
        QCOMPARE(log[i].causes, uint32_t(kDropoutInputOverflow));
    }
    QCOMPARE(w.lostFrames(), int64_t(0)); // nothing padded
    QCOMPARE(w.timelineFrames(), int64_t(600));
}

void TestDropouts::adcJumpAloneIsNotADropout() {
    // Without the host's overflow flag a timestamp jump proves nothing
    // (ALSA/PulseAudio timestamps jitter), so nothing is padded or logged.
    RingBuffer ring;
    ring.reset(10000);
    CaptureWriter w;
    w.reset(&ring, 1, 1000.0);
    w.beginCallback(100, false, 1.0);
    w.write(ramp(0, 100).data(), 100);
    w.beginCallback(100, false, 1.5);
    w.write(ramp(100, 100).data(), 100);
    QCOMPARE(w.finish(), int64_t(0));
    QCOMPARE(w.log().size(), size_t(0));
    QCOMPARE(w.timelineFrames(), int64_t(200));
}

void TestDropouts::hostLossAndRingLossRunTogether() {
    RingBuffer ring;
    ring.reset(150);
    CaptureWriter w;
    w.reset(&ring, 1, 1000.0, 16, 64);
    QVERIFY(w.write(ramp(0, 100).data(), 100));
    w.noteHostLoss(30);                          // host dropped 30 frames...
    QVERIFY(!w.write(ramp(130, 100).data(), 100)); // ...and the ring can't take the next block
    std::vector<float> out;
    ring.readAll(out);
    QVERIFY(w.write(ramp(230, 50).data(), 50));
    ring.readAll(out);
    QCOMPARE(w.finish(), int64_t(0));
    const std::vector<LostInterval> log = w.log().intervals();
    QCOMPARE(log.size(), size_t(1)); // one outage
    QCOMPARE(log[0].startFrame, int64_t(100));
    QCOMPARE(log[0].frames, int64_t(130));
    QCOMPARE(log[0].causes, uint32_t(kDropoutInputOverflow | kDropoutRingOverrun));
    QCOMPARE(out.size(), size_t(280));
    for (int64_t i = 0; i < 280; ++i) QCOMPARE(out[size_t(i)], (i >= 100 && i < 230) ? 0.0f : float(i + 1));
}

void TestDropouts::fullLogCountsWhatItCannotKeep() {
    RingBuffer ring;
    ring.reset(100000);
    CaptureWriter w;
    w.reset(&ring, 1, 1000.0, 2, 64);
    int64_t pos = 0;
    for (int i = 0; i < 5; ++i) {
        w.noteHostLoss(10); // separate losses, 90 good frames apart
        pos += 10;
        w.write(ramp(pos, 90).data(), 90);
        pos += 90;
    }
    w.finish();
    QCOMPARE(w.log().size(), size_t(2));
    QCOMPARE(w.log().unrecorded(), int64_t(3));
    QCOMPARE(w.lostFrames(), int64_t(50)); // all still padded
    QCOMPARE(w.timelineFrames(), pos);
}

void TestDropouts::logIsSafeToReadWhileWriting() {
    constexpr size_t kCount = 200000;
    DropoutLog log;
    log.reset(kCount);
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (size_t i = 0; i < kCount; ++i) log.add({int64_t(i) * 10, 5, kDropoutRingOverrun, 1});
        log.flush();
        done.store(true);
    });
    size_t checked = 0;
    bool ok = true;
    while (!done.load() || checked < log.size()) {
        const size_t n = log.size();
        for (; checked < n; ++checked) {
            const LostInterval& iv = log.at(checked);
            ok = ok && iv.startFrame == int64_t(checked) * 10 && iv.frames == 5;
        }
    }
    producer.join();
    QVERIFY(ok);
    QCOMPARE(checked, kCount);
    QCOMPARE(log.unrecorded(), int64_t(0));
}

void TestDropouts::nearbyLossesMergeIntoOneLabel() {
    const double rate = 44100.0;
    QCOMPARE(dropoutJoinFrames(rate), int64_t(4410));
    const std::vector<LostInterval> intervals = {
        {1000, 441, kDropoutRingOverrun, 1},    // 10 ms
        {3000, 441, kDropoutInputOverflow, 1},  // 1559 frames later: same glitch
        {7851, 882, kDropoutRingOverrun, 1},    // exactly 4410 after: separate
        {50000, 0, kDropoutInputOverflow, 1},   // unknown length
    };
    const std::vector<DropoutSpan> spans = mergeDropouts(intervals, dropoutJoinFrames(rate));
    QCOMPARE(spans.size(), size_t(3));
    QCOMPARE(spans[0].startFrame, int64_t(1000));
    QCOMPARE(spans[0].endFrame, int64_t(3441));
    QCOMPARE(spans[0].lostFrames, int64_t(882));
    QCOMPARE(spans[0].events, 2);
    QCOMPARE(spans[0].causes, uint32_t(kDropoutRingOverrun | kDropoutInputOverflow));
    QCOMPARE(spans[1].startFrame, int64_t(7851));
    QCOMPARE(spans[1].lostFrames, int64_t(882));
    QCOMPARE(spans[1].events, 1);
    QVERIFY(spans[2].lengthUnknown);
    QCOMPARE(spans[2].startFrame, spans[2].endFrame);
    QVERIFY(mergeDropouts({}, 4410).empty());
}

void TestDropouts::labelAndSummaryText() {
    const double rate = 44100.0;
    QCOMPARE(formatLostDuration(529, rate), std::string("12 ms"));
    QCOMPARE(formatLostDuration(10, rate), std::string("<1 ms"));
    QCOMPARE(formatLostDuration(55125, rate), std::string("1.25 s"));
    QCOMPARE(formatLostDuration(44070, rate), std::string("999 ms"));
    QCOMPARE(formatLostDuration(44100, rate), std::string("1.00 s"));

    DropoutSpan one{1000, 1529, 529, 1, kDropoutRingOverrun, false};
    DropoutSpan three{9000, 20000, 1587, 3, kDropoutRingOverrun, false};
    DropoutSpan unknown{30000, 30000, 0, 1, kDropoutInputOverflow, true};
    QCOMPARE(dropoutLabelText(one, rate), std::string("Dropout 12 ms"));
    QCOMPARE(dropoutLabelText(three, rate), std::string("Dropout \u00d73, 36 ms"));
    QCOMPARE(dropoutLabelText(unknown, rate), std::string("Dropout (length unknown)"));

    QCOMPARE(dropoutSummary({}, rate), std::string());
    QCOMPARE(dropoutSummary({one}, rate), std::string("1 dropout, 12 ms lost"));
    QCOMPARE(dropoutSummary({one, three}, rate), std::string("4 dropouts, 48 ms lost"));
    QCOMPARE(dropoutSummary({one, unknown}, rate), std::string("2 dropouts, 12 ms lost (some of unknown length)"));
}

QTEST_GUILESS_MAIN(TestDropouts)
#include "test_dropouts.moc"
