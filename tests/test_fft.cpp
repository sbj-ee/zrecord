#include <QtTest>
#include <cmath>

#include "Fft.h"

using namespace zrecord;

namespace {
// Index of the largest value.
size_t peakBin(const std::vector<float>& bins) {
    return static_cast<size_t>(std::max_element(bins.begin(), bins.end()) - bins.begin());
}
} // namespace

class TestFft : public QObject {
    Q_OBJECT

private slots:
    void rejectsNonPowerOfTwo();
    void dcLandsInBinZero();
    void toneLandsInItsOwnBin();
    void fullScaleToneReadsNearZeroDb();
    void silenceSitsAtTheFloor();
    void windowingKeepsAnOffBinToneLocal();
};

void TestFft::rejectsNonPowerOfTwo() {
    QVERIFY(isPowerOfTwo(1));
    QVERIFY(isPowerOfTwo(512));
    QVERIFY(!isPowerOfTwo(0));
    QVERIFY(!isPowerOfTwo(768));

    // A non-power-of-two transform must leave its input alone rather than
    // reading past the end of it.
    std::vector<float> real{1.0f, 2.0f, 3.0f};
    std::vector<float> imag(3, 0.0f);
    fftRadix2(real, imag);
    QCOMPARE(real, (std::vector<float>{1.0f, 2.0f, 3.0f}));

    QVERIFY(magnitudeSpectrumDb(std::vector<float>(300, 0.5f)).empty());
}

void TestFft::dcLandsInBinZero() {
    std::vector<float> bins = magnitudeSpectrumDb(std::vector<float>(256, 1.0f));

    QCOMPARE(bins.size(), size_t(128));
    QVERIFY2(bins[0] > -1.0f, qPrintable(QString("DC read %1 dB").arg(bins[0])));
    // The peak is bin 0 or 1, not strictly 0: the one-sided convention doubles
    // every bin above DC, which for a DC input doubles Hann's leakage into
    // bin 1 until it ties exactly with bin 0. Harmless for a spectrogram, but
    // it means "peak == 0" is not a property this actually has.
    size_t peak = peakBin(bins);
    QVERIFY2(peak <= 1, qPrintable(QString("DC energy landed in bin %1").arg(peak)));
    // What matters is that the energy stays at the low end rather than
    // spreading across the spectrum.
    QVERIFY(bins[10] < -40.0f);
    QVERIFY(bins[64] < -40.0f);
}

void TestFft::toneLandsInItsOwnBin() {
    const size_t n = 512;
    const size_t targetBin = 32;
    std::vector<float> tone(n);
    for (size_t i = 0; i < n; ++i) {
        tone[i] = static_cast<float>(std::sin(2.0 * M_PI * targetBin * i / n));
    }

    std::vector<float> bins = magnitudeSpectrumDb(tone);

    QCOMPARE(peakBin(bins), targetBin);
    // Neighbours pick up window leakage, but bins well away stay quiet.
    QVERIFY2(bins[targetBin + 20] < -40.0f,
             qPrintable(QString("leakage %1 dB well away from the tone").arg(bins[targetBin + 20])));
}

void TestFft::fullScaleToneReadsNearZeroDb() {
    const size_t n = 512;
    std::vector<float> tone(n);
    for (size_t i = 0; i < n; ++i) {
        tone[i] = static_cast<float>(std::sin(2.0 * M_PI * 64 * i / n));
    }

    std::vector<float> bins = magnitudeSpectrumDb(tone);

    // The window's coherent gain is divided back out, so a unit-amplitude
    // tone should land close to 0 dBFS rather than the -6 dB a raw Hann
    // window would give.
    QVERIFY2(bins[64] > -1.5f, qPrintable(QString("full-scale tone read %1 dB").arg(bins[64])));
    QVERIFY(bins[64] <= 0.0f); // clamped, never positive
}

void TestFft::silenceSitsAtTheFloor() {
    std::vector<float> bins = magnitudeSpectrumDb(std::vector<float>(256, 0.0f), -96.0f);

    QCOMPARE(bins.size(), size_t(128));
    for (float bin : bins) {
        QCOMPARE(bin, -96.0f);
    }
}

void TestFft::windowingKeepsAnOffBinToneLocal() {
    // A tone halfway between two bins is the worst case for leakage; the Hann
    // window is what stops it smearing across the whole spectrum.
    const size_t n = 512;
    std::vector<float> tone(n);
    for (size_t i = 0; i < n; ++i) {
        tone[i] = static_cast<float>(std::sin(2.0 * M_PI * 32.5 * i / n));
    }

    std::vector<float> bins = magnitudeSpectrumDb(tone);

    size_t peak = peakBin(bins);
    QVERIFY(peak == 32 || peak == 33);
    QVERIFY2(bins[200] < -50.0f,
             qPrintable(QString("far-field leakage %1 dB").arg(bins[200])));
}

QTEST_GUILESS_MAIN(TestFft)
#include "test_fft.moc"
