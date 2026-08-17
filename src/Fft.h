#pragma once

#include <cstddef>
#include <vector>

namespace zrecord {

// A small in-place radix-2 FFT. zrecord has no FFT dependency and only needs
// power-of-two transforms for the spectrogram, so pulling in FFTW or KissFFT
// would cost more (packaging, licensing) than the forty lines it saves.
//
// `real` and `imag` must be the same length and a power of two; anything else
// leaves them untouched.
void fftRadix2(std::vector<float>& real, std::vector<float>& imag);

bool isPowerOfTwo(size_t value);

// Hann-windowed magnitude spectrum of `mono`, in dB relative to full scale.
// Returns size/2 bins (the real half of the transform), each clamped to
// [floorDb, 0]. Returns empty if the input isn't a power-of-two length.
//
// The window matters: without it, a tone that doesn't sit exactly on a bin
// centre smears across the whole spectrum, which reads as broadband noise.
std::vector<float> magnitudeSpectrumDb(std::vector<float> mono, float floorDb = -96.0f);

} // namespace zrecord
