#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

// x^e on the unit interval without libm's powf. pow must set errno, which pins
// every call scalar (gcc reports the call as clobbering memory, and this
// gcc/glibc pairing never substitutes the libmvec vector powf, fast-math or
// not), so a per-pixel pow costs ~10 ms per 720x576x3 readout. Computing
// exp2(e * log2(x)) with fitted minimax polynomials instead leaves the loop
// free of calls - and of floorf, which gcc also declines to vectorise under
// default trapping-math - so a loop around pow01 autovectorises at -O2.
// Accuracy: log2 poly abs error <= 3.2e-7, exp2 poly rel error <= 2.6e-9;
// through 255 * pow01 that is well inside one 8-bit LSB for exponents to ~8.
namespace palindrome::dsp {

// log2(|x|) for x in (0, 1]. Exact 0 at x == 1. Subnormal x reads as roughly
// -127 instead of its true log; pow01's range clamp absorbs the difference
// for any exponent above ~1/16. The sign strip is load-bearing: -0.0 passes a
// [0, 1] clamp (it compares equal to 0) but its sign bit would otherwise ride
// into the exponent and read as +129, turning black into full white.
[[nodiscard]] inline float log2_poly(float x) noexcept {
  const auto xi = std::bit_cast<std::uint32_t>(x) & 0x7FFFFFFFu;
  const auto ex = static_cast<float>(static_cast<int>(xi >> 23) - 127);
  const auto m = std::bit_cast<float>((xi & 0x007FFFFFu) | 0x3F800000u); // [1, 2)
  const float f = m - 1.0f;
  float p = 0.0147787554f;
  p = p * f - 0.0768489041f;
  p = p * f + 0.190421171f;
  p = p * f - 0.323116249f;
  p = p * f + 0.472499676f;
  p = p * f - 0.720386648f;
  p = p * f + 1.44265211f;
  return ex + f * p;
}

// 2^t for t in [-40, 0]. Exact 1 at t == 0.
[[nodiscard]] inline float exp2_poly(float t) noexcept {
  // Floor by offset truncation (valid because t + 40 is non-negative); the
  // exponent reconstruction below stays in range for the whole domain.
  const auto ti = static_cast<int>(t + 40.0f) - 40;
  const float fr = t - static_cast<float>(ti);
  float q = 0.000218775071f;
  q = q * fr + 0.00123878221f;
  q = q * fr + 0.00968458019f;
  q = q * fr + 0.0554804266f;
  q = q * fr + 0.240230502f;
  q = q * fr + 0.693146929f;
  q = q * fr + 1.0f;
  const auto bits = static_cast<std::uint32_t>((ti + 127) << 23);
  return std::bit_cast<float>(bits) * q;
}

// x^e for x in [0, 1] and e > 0. Exact 1 at x == 1; x below ~2^(-40/e)
// (including 0) returns 2^-40, which any 8-bit readout rounds to 0.
[[nodiscard]] inline float pow01(float x, float e) noexcept {
  const float t = std::clamp(e * log2_poly(x), -40.0f, 0.0f);
  return exp2_poly(t);
}

} // namespace palindrome::dsp
