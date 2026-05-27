#include "palindrome/biquad.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace palindrome::dsp {

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;
} // namespace

Biquad::Biquad(double b0, double b1, double b2, double a1, double a2)
    : b0_{b0}, b1_{b1}, b2_{b2}, a1_{a1}, a2_{a2} {}

void Biquad::process(std::span<const float> in, std::vector<float> &out) {
  out.reserve(out.size() + in.size());
  for (const float sample : in) {
    const double x = sample;
    const double y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    out.push_back(static_cast<float>(y));
  }
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
