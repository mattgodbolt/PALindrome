#include "palindrome/demod.hpp"

#include "palindrome/restrict_ptr.hpp"

#include <cmath>

namespace palindrome::demod {

namespace {
// AM envelope: 2 * sqrt(i^2 + q^2), elementwise. The restrict_ptr params rule out
// aliasing (in the signature, not a comment); dropping the errno and trapping
// guards on sqrt (scoped to this function) lets
// it lower to a packed vsqrtps store. __builtin_sqrtf sidesteps the std::sqrt
// wrapper, which the optimize attribute otherwise keeps from inlining.
//
// TODO(std::simd): drop this [[gnu::optimize]] attribute for explicit std::simd
// (see the matching note in fir.cpp). The load/store/mul/add port cleanly to
// GCC 16.1's <simd>, but its simd-math (sqrt) isn't implemented yet, so this one
// also waits on a newer toolchain before it can go fully flag-free.
[[gnu::optimize("-fno-math-errno", "-fno-trapping-math")]]
void envelope_magnitude(
    restrict_ptr<const float> i, restrict_ptr<const float> q, restrict_ptr<float> out, std::size_t n) {
  for (std::size_t k = 0; k < n; ++k)
    out[k] = 2.0f * __builtin_sqrtf(i[k] * i[k] + q[k] * q[k]);
}
} // namespace

AmEnvelope::AmEnvelope(double sample_rate_hz, double carrier_hz, double cutoff_hz, std::size_t num_taps,
    dsp::Window window, std::size_t decimation) :
    mixer_{carrier_hz, sample_rate_hz},
    i_filter_{dsp::lowpass_kernel(num_taps, sample_rate_hz, cutoff_hz, window), decimation},
    q_filter_{dsp::lowpass_kernel(num_taps, sample_rate_hz, cutoff_hz, window), decimation} {}

void AmEnvelope::prepare(std::size_t max_in) {
  mixer_.prepare(max_in);
  i_filter_.prepare(max_in);
  q_filter_.prepare(max_in);
  out_.reserve(i_filter_.max_output_for(max_in));
}

std::span<const float> AmEnvelope::process(std::span<const float> in) {
  // Down-convert to baseband I/Q, then low-pass each plane.
  const dsp::Mixer::Iq mixed = mixer_.process(in);
  const std::span<const float> filtered_i = i_filter_.process(mixed.i);
  const std::span<const float> filtered_q = q_filter_.process(mixed.q);

  // A real carrier of amplitude A lands as a baseband component of magnitude
  // A/2, so scale by 2 to recover A. std::hypot's overflow guarding is needless
  // here (the I/Q are bounded), and plain sqrt of the sum of squares vectorises
  // into an elementwise store.
  const std::size_t n = filtered_i.size();
  out_.reserve(n);
  envelope_magnitude(filtered_i.data(), filtered_q.data(), out_.write_n(n).data(), n);
  return out_.view();
}

} // namespace palindrome::demod
