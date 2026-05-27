#include "palindrome/demod.hpp"

#include <cmath>
#include <numbers>

namespace palindrome::demod {

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;
// Renormalise the running phasor this often to shed accumulated rounding drift.
constexpr unsigned renorm_interval = 1024;
} // namespace

AmEnvelope::AmEnvelope(
    double sample_rate_hz, double carrier_hz, double cutoff_hz, std::size_t num_taps, dsp::Window window) :
    i_filter_{dsp::lowpass_kernel(num_taps, sample_rate_hz, cutoff_hz, window)},
    q_filter_{dsp::lowpass_kernel(num_taps, sample_rate_hz, cutoff_hz, window)} {
  // Mixing by e^{-i*omega} shifts +carrier down to DC, one step per sample.
  const double omega = two_pi * carrier_hz / sample_rate_hz;
  step_ = std::polar(1.0, -omega);
}

void AmEnvelope::process(std::span<const float> in, std::vector<float> &out) {
  // Down-convert: x * e^{-i*theta}, accumulating I/Q for the block.
  mixed_i_.clear();
  mixed_q_.clear();
  mixed_i_.reserve(in.size());
  mixed_q_.reserve(in.size());
  for (const float sample: in) {
    const std::complex<double> mixed = static_cast<double>(sample) * phasor_;
    mixed_i_.push_back(static_cast<float>(mixed.real()));
    mixed_q_.push_back(static_cast<float>(mixed.imag()));
    phasor_ *= step_;
    if (++since_renorm_ >= renorm_interval) {
      since_renorm_ = 0;
      phasor_ /= std::abs(phasor_);
    }
  }

  filtered_i_.clear();
  filtered_q_.clear();
  i_filter_.process(mixed_i_, filtered_i_);
  q_filter_.process(mixed_q_, filtered_q_);

  // A real carrier of amplitude A lands as a baseband component of magnitude
  // A/2, so scale by 2 to recover A.
  out.reserve(out.size() + in.size());
  for (std::size_t k = 0; k < filtered_i_.size(); ++k)
    out.push_back(static_cast<float>(2.0 * std::hypot(filtered_i_[k], filtered_q_[k])));
}

} // namespace palindrome::demod
