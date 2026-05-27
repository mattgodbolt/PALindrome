#include "palindrome/demod.hpp"

#include <cmath>

namespace palindrome::demod {

namespace {
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

AmEnvelope::AmEnvelope(double sample_rate_hz, double carrier_hz, double cutoff_hz, std::size_t num_taps,
    dsp::Window window, std::size_t decimation) :
    mixer_{carrier_hz, sample_rate_hz},
    i_filter_{dsp::lowpass_kernel(num_taps, sample_rate_hz, cutoff_hz, window), decimation},
    q_filter_{dsp::lowpass_kernel(num_taps, sample_rate_hz, cutoff_hz, window), decimation} {}

void AmEnvelope::process(std::span<const float> in, std::vector<float> &out) {
  // Down-convert to baseband I/Q for the block.
  mixed_i_.clear();
  mixed_q_.clear();
  mixer_.process(in, mixed_i_, mixed_q_);

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
