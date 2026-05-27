#include "palindrome/demod.hpp"

#include <cmath>
#include <numbers>

namespace palindrome::demod {

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;
// Renormalise the running phasor this often to shed accumulated rounding drift.
constexpr unsigned renorm_interval = 1024;

// AM envelope: 2 * sqrt(i^2 + q^2), elementwise. restrict rules out aliasing;
// dropping the errno and trapping guards on sqrt (scoped to this function) lets
// it lower to a packed vsqrtps store. __builtin_sqrtf sidesteps the std::sqrt
// wrapper, which the optimize attribute otherwise keeps from inlining.
//
// TODO(std::simd): drop this [[gnu::optimize]] attribute for explicit std::simd
// (see the matching note in fir.cpp). The load/store/mul/add port cleanly to
// GCC 16.1's <simd>, but its simd-math (sqrt) isn't implemented yet, so this one
// also waits on a newer toolchain before it can go fully flag-free.
[[gnu::optimize("-fno-math-errno", "-fno-trapping-math")]]
void envelope_magnitude(const float *__restrict i, const float *__restrict q, float *__restrict out, std::size_t n) {
  for (std::size_t k = 0; k < n; ++k)
    out[k] = 2.0f * __builtin_sqrtf(i[k] * i[k] + q[k] * q[k]);
}
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
  // A/2, so scale by 2 to recover A. std::hypot's overflow guarding is needless
  // here (the I/Q are bounded), and plain sqrt of the sum of squares vectorises
  // into an elementwise store.
  const std::size_t n = filtered_i_.size();
  out.resize(out.size() + n);
  envelope_magnitude(filtered_i_.data(), filtered_q_.data(), out.data() + (out.size() - n), n);
}

} // namespace palindrome::demod
