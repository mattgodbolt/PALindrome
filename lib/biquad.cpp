#include "palindrome/biquad.hpp"

#include "palindrome/restrict.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace palindrome::dsp {

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;
} // namespace

Biquad::Biquad(double b0, double b1, double b2, double a1, double a2) : b0_{b0}, b1_{b1}, b2_{b2}, a1_{a1}, a2_{a2} {}

void Biquad::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const float> Biquad::process(std::span<const float> in) {
  const std::size_t n = in.size();
  out_.reserve(n);
  const float *PAL_RESTRICT x = in.data();
  float *PAL_RESTRICT y = out_.write_n(n).data();
  // Transposed direct-form II: the state recurrence is serial (no vectorising),
  // so the win over push_back is the flat counted store into reserved storage.
  for (std::size_t k = 0; k < n; ++k) {
    const double xk = x[k];
    const double yk = b0_ * xk + z1_;
    z1_ = b1_ * xk - a1_ * yk + z2_;
    z2_ = b2_ * xk - a2_ * yk;
    y[k] = static_cast<float>(yk);
  }
  return out_.view();
}

void Biquad::reset() { z1_ = z2_ = 0.0; }

Biquad notch(double sample_rate_hz, double center_hz, double q) {
  if (center_hz <= 0.0 || center_hz >= sample_rate_hz / 2.0)
    throw std::invalid_argument("notch center must be in (0, sample_rate / 2)");
  if (q <= 0.0)
    throw std::invalid_argument("Q must be positive");

  // RBJ "Audio EQ Cookbook" notch.
  const double w0 = two_pi * center_hz / sample_rate_hz;
  const double cos_w0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  return Biquad{1.0 / a0, -2.0 * cos_w0 / a0, 1.0 / a0, -2.0 * cos_w0 / a0, (1.0 - alpha) / a0};
}

} // namespace palindrome::dsp
