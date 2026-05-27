#include "palindrome/demod.hpp"

#include <cmath>
#include <numbers>

namespace palindrome::demod {

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;
// Renormalise the running phasor this often to shed accumulated rounding drift.
constexpr unsigned renorm_interval = 1024;
// One-pole DC-blocker feedback coefficient; corner ~ (1-R)*fs/2pi (~500 Hz at
// 32 MS/s), far below the carrier, so it removes only the DC bias.
constexpr double dc_blocker_pole = 0.9999;
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
    // Block DC: y[n] = x[n] - x[n-1] + R*y[n-1].
    const double in_sample = static_cast<double>(sample);
    const double blocked = in_sample - dc_prev_in_ + dc_blocker_pole * dc_prev_out_;
    dc_prev_in_ = in_sample;
    dc_prev_out_ = blocked;

    const std::complex<double> mixed = blocked * phasor_;
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
