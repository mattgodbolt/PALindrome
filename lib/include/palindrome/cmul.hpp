#pragma once

#include <complex>

namespace palindrome::dsp {

// Multiply two complex<double> directly. std::complex's operator* carries full
// Inf/NaN semantics — a branch on the result plus a cold __muldc3 fallback —
// unless the translation unit is built with -fcx-limited-range, which this
// project is not. The phasor bookkeeping in the DSP hot loops only ever advances
// finite, unit-modulus values, so that guard is dead weight on a loop-carried
// multiply; doing the arithmetic by hand keeps the loop branch- and call-free.
// The result is identical to operator*'s finite path (same expressions, same FMA
// contraction), so block-invariance and the golden image are unchanged.
[[nodiscard]] constexpr std::complex<double> cmul(std::complex<double> a, std::complex<double> b) noexcept {
  return {a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real()};
}

} // namespace palindrome::dsp
