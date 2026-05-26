#include "palindrome/demod.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace palindrome::demod {

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;
// Renormalise the running phasor this often to shed accumulated rounding drift.
constexpr unsigned renorm_interval = 1024;
} // namespace

AmEnvelope::AmEnvelope(double sample_rate_hz, double carrier_hz, double cutoff_hz) {
  if (sample_rate_hz <= 0.0)
    throw std::invalid_argument("sample rate must be positive");
  if (cutoff_hz <= 0.0 || cutoff_hz >= sample_rate_hz / 2.0)
    throw std::invalid_argument("cutoff must be in (0, sample_rate / 2)");

  // Mixing by e^{-i*omega} shifts +carrier down to DC, one step per sample.
  const double omega = two_pi * carrier_hz / sample_rate_hz;
  step_ = std::polar(1.0, -omega);

  // One-pole low-pass with unity DC gain: y += alpha * (x - y). The corner
  // frequency maps to alpha = 1 - e^{-2*pi*fc/fs}.
  alpha_ = 1.0 - std::exp(-two_pi * cutoff_hz / sample_rate_hz);
}

void AmEnvelope::process(std::span<const float> in, std::vector<float> &out) {
  out.reserve(out.size() + in.size());
  for (const float sample: in) {
    // Down-convert: x * e^{-i*theta}. A real carrier of amplitude A lands as a
    // baseband component of magnitude A/2, so scale by 2 below to recover A.
    const std::complex<double> mixed = static_cast<double>(sample) * phasor_;
    i_ += alpha_ * (mixed.real() - i_);
    q_ += alpha_ * (mixed.imag() - q_);
    out.push_back(static_cast<float>(2.0 * std::hypot(i_, q_)));

    phasor_ *= step_;
    if (++since_renorm_ >= renorm_interval) {
      since_renorm_ = 0;
      phasor_ /= std::abs(phasor_);
    }
  }
}

} // namespace palindrome::demod
