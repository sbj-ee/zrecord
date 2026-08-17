#include "Fft.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace zrecord {

bool isPowerOfTwo(size_t value) {
    return value > 0 && (value & (value - 1)) == 0;
}

void fftRadix2(std::vector<float>& real, std::vector<float>& imag) {
    const size_t n = real.size();
    if (n != imag.size() || !isPowerOfTwo(n) || n < 2) {
        return;
    }

    // Bit-reversal permutation.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    // Cooley-Tukey butterflies, doubling the transform length each pass.
    for (size_t len = 2; len <= n; len <<= 1) {
        double angle = -2.0 * M_PI / static_cast<double>(len);
        float wReal = static_cast<float>(std::cos(angle));
        float wImag = static_cast<float>(std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            float curReal = 1.0f;
            float curImag = 0.0f;
            for (size_t k = 0; k < len / 2; ++k) {
                float uReal = real[i + k];
                float uImag = imag[i + k];
                float vReal = real[i + k + len / 2] * curReal - imag[i + k + len / 2] * curImag;
                float vImag = real[i + k + len / 2] * curImag + imag[i + k + len / 2] * curReal;
                real[i + k] = uReal + vReal;
                imag[i + k] = uImag + vImag;
                real[i + k + len / 2] = uReal - vReal;
                imag[i + k + len / 2] = uImag - vImag;
                float nextReal = curReal * wReal - curImag * wImag;
                curImag = curReal * wImag + curImag * wReal;
                curReal = nextReal;
            }
        }
    }
}

std::vector<float> magnitudeSpectrumDb(std::vector<float> mono, float floorDb) {
    const size_t n = mono.size();
    if (!isPowerOfTwo(n) || n < 2) {
        return {};
    }

    // Hann window, with the coherent gain (0.5) divided back out so a
    // full-scale tone still reads near 0 dB rather than -6.
    double windowSum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n - 1)));
        mono[i] = static_cast<float>(mono[i] * w);
        windowSum += w;
    }
    if (windowSum <= 0.0) {
        return {};
    }

    std::vector<float> imag(n, 0.0f);
    fftRadix2(mono, imag);

    std::vector<float> bins(n / 2, floorDb);
    for (size_t k = 0; k < n / 2; ++k) {
        double magnitude = std::hypot(static_cast<double>(mono[k]), static_cast<double>(imag[k]));
        // Two-sided spectrum: everything but DC has half its energy in the
        // mirrored upper half, so double it to get the true amplitude.
        double scale = (k == 0) ? 1.0 : 2.0;
        double amplitude = magnitude * scale / windowSum;
        double db = 20.0 * std::log10(std::max(amplitude, 1e-12));
        bins[k] = static_cast<float>(std::clamp(db, static_cast<double>(floorDb), 0.0));
    }
    return bins;
}

} // namespace zrecord
