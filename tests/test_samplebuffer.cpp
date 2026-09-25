#include <QtTest>

#include <random>

#include "SampleBuffer.h"

using namespace zrecord;

namespace {

constexpr size_t kChunk = SampleBuffer::kChunkSize;

std::vector<float> ramp(size_t n) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<float>(i);
    }
    return v;
}

} // namespace

class TestSampleBuffer : public QObject {
    Q_OBJECT

private slots:
    void roundTripsAcrossChunks();
    void copyIsSharedNotDuplicated();
    void writeCopiesOnlyTouchedChunks();
    void writeLeavesEarlierCopiesUntouched();
    void sliceSharesChunks();
    void randomisedOpsMatchAPlainVector();
};

void TestSampleBuffer::roundTripsAcrossChunks() {
    const std::vector<float> source = ramp(3 * kChunk + 17);
    SampleBuffer buffer(source);
    QCOMPARE(buffer.size(), source.size());
    QCOMPARE(buffer.toVector(), source);
    QCOMPARE(buffer[kChunk - 1], float(kChunk - 1));
    QCOMPARE(buffer[kChunk], float(kChunk));
    QCOMPARE(buffer.back(), source.back());

    std::vector<float> part(10);
    buffer.copyTo(kChunk - 5, 10, part.data()); // straddles a chunk edge
    for (size_t i = 0; i < 10; ++i) {
        QCOMPARE(part[i], float(kChunk - 5 + i));
    }
}

void TestSampleBuffer::copyIsSharedNotDuplicated() {
    SampleBuffer a(ramp(4 * kChunk));
    SampleBuffer b = a;
    QVERIFY(b.sharesStorageWith(a));
}

void TestSampleBuffer::writeCopiesOnlyTouchedChunks() {
    SampleBuffer original(ramp(8 * kChunk));
    SampleBuffer edited = original;
    edited.fill(3 * kChunk + 10, 100, 0.0f); // inside chunk 3 only

    for (size_t chunk = 0; chunk < 8; ++chunk) {
        bool shared = edited.sharesChunkAt(original, chunk * kChunk);
        QVERIFY2(shared == (chunk != 3), qPrintable(QString("chunk %1").arg(chunk)));
    }
}

void TestSampleBuffer::writeLeavesEarlierCopiesUntouched() {
    const std::vector<float> source = ramp(2 * kChunk + 5);
    SampleBuffer original(source);
    SampleBuffer edited = original;
    const std::vector<float> patch(kChunk + 10, -1.0f); // ends 5 before the end
    edited.write(kChunk - 10, patch.data(), patch.size());

    QCOMPARE(original.toVector(), source); // the "undo snapshot" is intact
    std::vector<float> expected = source;
    std::copy(patch.begin(), patch.end(), expected.begin() + static_cast<long>(kChunk - 10));
    QCOMPARE(edited.toVector(), expected);
}

void TestSampleBuffer::sliceSharesChunks() {
    SampleBuffer whole(ramp(5 * kChunk));
    SampleBuffer tail = whole.slice(2 * kChunk + 7, 2 * kChunk);
    QCOMPARE(tail.size(), 2 * kChunk);
    QCOMPARE(tail.front(), float(2 * kChunk + 7));
    QVERIFY(tail.sharesChunkAt(whole.slice(2 * kChunk + 7, 2 * kChunk), 0));
    // Editing the slice doesn't reach back into the whole.
    tail.fill(0, 5, 0.0f);
    QCOMPARE(whole[2 * kChunk + 7], float(2 * kChunk + 7));
}

void TestSampleBuffer::randomisedOpsMatchAPlainVector() {
    std::mt19937 rng(1234);
    std::vector<float> reference = ramp(3 * kChunk + 999);
    SampleBuffer buffer(reference);
    for (int step = 0; step < 300; ++step) {
        const size_t size = reference.size();
        std::uniform_int_distribution<size_t> pos(0, size - 1);
        size_t start = pos(rng);
        size_t count = std::min(size - start, pos(rng) % (kChunk + 50) + 1);
        switch (rng() % 3) {
            case 0: {
                std::fill(reference.begin() + static_cast<long>(start),
                          reference.begin() + static_cast<long>(start + count), float(step));
                buffer.fill(start, count, float(step));
                break;
            }
            case 1: {
                std::vector<float> patch(count, -float(step));
                std::copy(patch.begin(), patch.end(), reference.begin() + static_cast<long>(start));
                buffer.write(start, patch.data(), count);
                break;
            }
            default: {
                if (count > 1 && size - count > kChunk) {
                    reference = std::vector<float>(reference.begin() + static_cast<long>(start),
                                                   reference.begin() + static_cast<long>(start + count));
                    buffer = buffer.slice(start, count);
                    if (reference.size() < kChunk) { // keep it multi-chunk
                        reference = ramp(3 * kChunk + 999);
                        buffer = SampleBuffer(reference);
                    }
                }
                break;
            }
        }
        QVERIFY2(buffer == reference, qPrintable(QString("diverged at step %1").arg(step)));
    }
}

QTEST_GUILESS_MAIN(TestSampleBuffer)
#include "test_samplebuffer.moc"
