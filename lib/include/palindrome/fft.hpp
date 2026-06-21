#pragma once

#include <complex>
#include <span>

// A small in-place radix-2 FFT. The streaming decode is all FIR - this exists
// for the cold, one-shot spectral jobs around it: finding the vision carrier in
// a recording before the front end is built (see demod::find_vision_carrier),
// and any future spectrum diagnostics. It is NOT a streaming stage and carries
// no state.
namespace palindrome::dsp {

// Forward DFT in place (exponent -2*pi*i*k*n/N): x[k] <- sum_n x[n] e^{...}.
// x.size() must be a power of two (the radix-2 decimation-in-time assumes it);
// throws std::invalid_argument otherwise. Empty input is a no-op.
void fft(std::span<std::complex<double>> x);

} // namespace palindrome::dsp
