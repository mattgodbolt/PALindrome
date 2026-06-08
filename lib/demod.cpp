#include "palindrome/demod.hpp"

#include "palindrome/cmul.hpp"
#include "palindrome/restrict_ptr.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <span>
#include <stdexcept>

namespace palindrome::demod {

namespace {
// Validate a Hilbert tap count before any work is done with it: odd (so the
// group delay (n-1)/2 is integer and the I-plane delay matches the FIR exactly)
// and non-zero. Returns it for use in a member initialiser.
std::size_t require_odd_hilbert_taps(std::size_t num_taps) {
  if (num_taps == 0 || num_taps % 2 == 0)
    throw std::invalid_argument{"Hilbert: num_taps must be odd and non-zero (for an integer group delay)"};
  return num_taps;
}

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

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kDcPole = 0.9999; // complex DC-blocker feedback, matching dsp::DcBlocker
} // namespace

ComplexAmEnvelope::ComplexAmEnvelope(double sample_rate_hz, double carrier_offset_hz, double cutoff_hz,
    std::size_t num_taps, dsp::Window window, std::size_t decimation) :
    i_filter_{dsp::lowpass_kernel(num_taps, sample_rate_hz, cutoff_hz, window), decimation},
    q_filter_{dsp::lowpass_kernel(num_taps, sample_rate_hz, cutoff_hz, window), decimation} {
  // Mix the vision carrier offset down to DC: multiply by e^{-i*2*pi*offset/fs}
  // per sample. A positive offset rotates one way, negative the other.
  const double omega = kTwoPi * carrier_offset_hz / sample_rate_hz;
  step_ = std::polar(1.0, -omega);
}

void ComplexAmEnvelope::prepare(std::size_t max_in) {
  i_filter_.prepare(max_in);
  q_filter_.prepare(max_in);
  mix_i_.reserve(max_in);
  mix_q_.reserve(max_in);
  out_.reserve(i_filter_.max_output_for(max_in));
}

std::span<const float> ComplexAmEnvelope::process(std::span<const std::complex<float>> in) {
  const std::size_t n = in.size();
  const auto mi = mix_i_.write_n(n);
  const auto mq = mix_q_.write_n(n);

  // DC-block, then shift the carrier to DC. Both recurrences carry across calls,
  // so the result is independent of how the input is chunked (to float rounding).
  for (std::size_t k = 0; k < n; ++k) {
    const std::complex<double> x{in[k].real(), in[k].imag()};
    const std::complex<double> hp = x - dc_prev_in_ + kDcPole * dc_prev_out_;
    dc_prev_in_ = x;
    dc_prev_out_ = hp;

    const std::complex<float> p{static_cast<float>(phasor_.real()), static_cast<float>(phasor_.imag())};
    const std::complex<float> m = std::complex<float>{static_cast<float>(hp.real()), static_cast<float>(hp.imag())} * p;
    mi[k] = m.real();
    mq[k] = m.imag();
    phasor_ = dsp::cmul(phasor_, step_);
    if (++since_renorm_ >= 1024) {
      phasor_ /= std::abs(phasor_); // keep |phasor_| == 1 against accumulated drift
      since_renorm_ = 0;
    }
  }

  const auto filtered_i = i_filter_.process(mi);
  const auto filtered_q = q_filter_.process(mq);
  const std::size_t m = filtered_i.size();
  envelope_magnitude(filtered_i.data(), filtered_q.data(), out_.write_n(m).data(), m);
  return out_.view();
}

Hilbert::Hilbert(std::size_t num_taps, dsp::Window window) :
    q_filter_{dsp::hilbert_kernel(require_odd_hilbert_taps(num_taps), window)}, delay_{(num_taps - 1) / 2} {
  i_history_.assign(delay_, 0.0f);
}

void Hilbert::prepare(std::size_t max_in) {
  q_filter_.prepare(max_in);
  out_.reserve(max_in);
}

std::span<const std::complex<float>> Hilbert::process(std::span<const float> in) {
  const std::size_t n = in.size();
  // Quadrature plane: the Hilbert transform, delayed by the FIR group delay.
  const auto q = q_filter_.process(in);
  const auto out = out_.write_n(n);

  // In-phase plane: the input delayed by the same group delay so I and Q align.
  // The first `delay_` outputs read the tail carried from the previous block;
  // the rest read this block. Reads happen before the history is rewritten below.
  for (std::size_t k = 0; k < n; ++k) {
    const float i = k < delay_ ? i_history_[k] : in[k - delay_];
    out[k] = std::complex<float>{i, q[k]};
  }

  // Carry the last `delay_` inputs for the next block. If this block is shorter
  // than the delay, slide the existing history down by n and append the block —
  // so the result is independent of how the stream is chunked.
  if (delay_ > 0) {
    const auto d = static_cast<std::ptrdiff_t>(delay_);
    if (n >= delay_)
      std::copy(in.end() - d, in.end(), i_history_.begin());
    else {
      const auto m = static_cast<std::ptrdiff_t>(n);
      std::copy(i_history_.begin() + m, i_history_.end(), i_history_.begin());
      std::copy(in.begin(), in.end(), i_history_.end() - m);
    }
  }
  return out_.view();
}

} // namespace palindrome::demod
