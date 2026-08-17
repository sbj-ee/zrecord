#include <QtTest>
#include <thread>

#include "RingBuffer.h"

using namespace zrecord;

class TestRingBuffer : public QObject {
    Q_OBJECT

private slots:
    void writeThenReadRoundTrips();
    void wrapsAroundTheEnd();
    void refusesAnOversizedWriteWholesale();
    void latchesOverrun();
    void survivesAProducerAndConsumerRunningAtOnce();
};

void TestRingBuffer::writeThenReadRoundTrips() {
    RingBuffer ring;
    ring.reset(16);

    const std::vector<float> in{1.0f, 2.0f, 3.0f};
    QVERIFY(ring.write(in.data(), in.size()));
    QCOMPARE(ring.available(), size_t(3));

    std::vector<float> out;
    QCOMPARE(ring.readAll(out), size_t(3));
    QCOMPARE(out, in);
    QCOMPARE(ring.available(), size_t(0));
}

void TestRingBuffer::wrapsAroundTheEnd() {
    RingBuffer ring;
    ring.reset(8);

    // Fill, drain, then write again so the next write straddles the end of the
    // storage -- the case where an off-by-one silently corrupts audio.
    std::vector<float> filler(6, 1.0f);
    QVERIFY(ring.write(filler.data(), filler.size()));
    std::vector<float> drained;
    ring.readAll(drained);

    const std::vector<float> in{10.0f, 20.0f, 30.0f, 40.0f, 50.0f};
    QVERIFY(ring.write(in.data(), in.size()));

    std::vector<float> out;
    QCOMPARE(ring.readAll(out), size_t(5));
    QCOMPARE(out, in);
}

void TestRingBuffer::refusesAnOversizedWriteWholesale() {
    RingBuffer ring;
    ring.reset(4);

    const std::vector<float> tooBig{1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    QVERIFY(!ring.write(tooBig.data(), tooBig.size()));

    // Nothing partial was written: a torn block would be worse than a dropped one.
    QCOMPARE(ring.available(), size_t(0));
}

void TestRingBuffer::latchesOverrun() {
    RingBuffer ring;
    ring.reset(4);
    QVERIFY(!ring.overran());

    std::vector<float> block(4, 1.0f);
    QVERIFY(ring.write(block.data(), block.size()));
    QVERIFY(!ring.write(block.data(), block.size())); // no room

    QVERIFY(ring.overran());
    // Draining doesn't clear it: the audio is gone and the caller should know.
    std::vector<float> out;
    ring.readAll(out);
    QVERIFY(ring.overran());
}

void TestRingBuffer::survivesAProducerAndConsumerRunningAtOnce() {
    RingBuffer ring;
    ring.reset(1024);

    const int blocks = 2000;
    const int blockSize = 64;

    std::thread producer([&] {
        std::vector<float> block(blockSize);
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < blockSize; ++i) {
                block[static_cast<size_t>(i)] = static_cast<float>(b * blockSize + i);
            }
            while (!ring.write(block.data(), block.size())) {
                std::this_thread::yield(); // consumer is behind; wait rather than drop
            }
        }
    });

    std::vector<float> received;
    received.reserve(static_cast<size_t>(blocks) * blockSize);
    while (received.size() < static_cast<size_t>(blocks) * blockSize) {
        ring.readAll(received);
    }
    producer.join();

    // Every sample arrives exactly once and in order.
    QCOMPARE(received.size(), static_cast<size_t>(blocks) * blockSize);
    for (size_t i = 0; i < received.size(); ++i) {
        if (received[i] != static_cast<float>(i)) {
            QFAIL(qPrintable(QString("sample %1 was %2").arg(i).arg(received[i])));
        }
    }
}

QTEST_GUILESS_MAIN(TestRingBuffer)
#include "test_ringbuffer.moc"
