#include <QtTest>

#include <cmath>

#include "Resampler.h"

using namespace zrecord;

namespace {

std::vector<float> sine(double freq, double rate, double seconds, int channels) {
    const size_t frames = static_cast<size_t>(rate * seconds);
    std::vector<float> out(frames * static_cast<size_t>(channels));
    for (size_t f = 0; f < frames; ++f) {
        for (int c = 0; c < channels; ++c) {
            out[f * channels + c] = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * freq * f / rate));
        }
    }
    return out;
}

// Estimated frequency of channel `c` from upward zero crossings, ignoring the
// filter's start-up transient.
double frequencyOf(const std::vector<float>& s, int channels, int c, double rate) {
    const size_t frames = s.size() / channels;
    int crossings = 0;
    size_t first = 0;
    size_t last = 0;
    for (size_t f = frames / 10 + 1; f < frames - frames / 10; ++f) {
        if (s[(f - 1) * channels + c] < 0.0f && s[f * channels + c] >= 0.0f) {
            if (crossings == 0) {
                first = f;
            }
            last = f;
            ++crossings;
        }
    }
    return crossings > 1 ? (crossings - 1) * rate / static_cast<double>(last - first) : 0.0;
}

} // namespace

class TestResampler : public QObject {
    Q_OBJECT

private slots:
    void keepsPitchAndDurationStereo();
    void sameRateIsACopy();
    void rejectsNonsense();
};

void TestResampler::keepsPitchAndDurationStereo() {
    const auto in = sine(1000.0, 48000.0, 1.0, 2);
    std::vector<float> out;
    std::string error;
    QVERIFY2(Resampler::convert(in, 2, 48000.0, 44100.0, out, error), error.c_str());
    const double frames = static_cast<double>(out.size() / 2);
    QVERIFY2(std::fabs(frames - 44100.0) <= 2.0, qPrintable(QString::number(frames)));
    for (int c = 0; c < 2; ++c) {
        const double f = frequencyOf(out, 2, c, 44100.0);
        QVERIFY2(std::fabs(f - 1000.0) < 2.0, qPrintable(QString("ch %1: %2 Hz").arg(c).arg(f)));
    }
}

void TestResampler::sameRateIsACopy() {
    const auto in = sine(440.0, 44100.0, 0.1, 1);
    std::vector<float> out;
    std::string error;
    QVERIFY(Resampler::convert(in, 1, 44100.0, 44100.0, out, error));
    QCOMPARE(out, in);
}

void TestResampler::rejectsNonsense() {
    std::vector<float> out;
    std::string error;
    QVERIFY(!Resampler::convert({0.1f}, 1, 0.0, 44100.0, out, error));
    QVERIFY(!error.empty());
}

QTEST_GUILESS_MAIN(TestResampler)
#include "test_resampler.moc"
