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
// Validate a Hilbert tap count before any work is done with it, and return it for
// use in a member initialiser. Must be ≡ 3 (mod 4): that makes it odd (so the
// group delay (n-1)/2 is an integer the I-plane delay can match) AND puts the
// centre tap on an odd index, so the Type III kernel's non-zero taps — those at
// an ODD offset from the centre — land on EVEN array indices, which the polyphase
// even/odd split below assumes. The natural Hilbert lengths (2^k - 1: 31, 63, 127,
// ...) all satisfy this; e.g. 65 would not, and is rejected rather than silently
// picking the zero taps.
std::size_t require_hilbert_taps(std::size_t num_taps) {
  if (num_taps % 4 != 3)
    throw std::invalid_argument{"Hilbert: num_taps must be ≡ 3 (mod 4) — odd, with the kernel's non-zero taps on even "
                                "indices (e.g. 31, 63, 127)"};
  return num_taps;
}

// The non-zero taps of the Hilbert kernel. For a num_taps ≡ 3 (mod 4) kernel the
// non-zero taps (odd offset from the centre) sit at the even array indices, so
// each polyphase FIR runs full[0], full[2], ... over one input parity; the dropped
// taps were exact zeros, so the result is bit-identical to convolving the full
// kernel.
std::vector<float> even_hilbert_taps(std::size_t num_taps, dsp::Window window) {
  const auto full = dsp::hilbert_kernel(require_hilbert_taps(num_taps), window);
  std::vector<float> even((full.size() + 1) / 2);
  for (std::size_t m = 0; m < even.size(); ++m)
    even[m] = full[2 * m];
  return even;
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
    const auto xr = static_cast<double>(in[k].real());
    const auto xi = static_cast<double>(in[k].imag());
    const double hp_re = xr - dc_prev_in_re_ + kDcPole * dc_prev_out_re_;
    const double hp_im = xi - dc_prev_in_im_ + kDcPole * dc_prev_out_im_;
    dc_prev_in_re_ = xr;
    dc_prev_in_im_ = xi;
    dc_prev_out_re_ = hp_re;
    dc_prev_out_im_ = hp_im;

    const std::complex<float> p{static_cast<float>(phasor_.real()), static_cast<float>(phasor_.imag())};
    const std::complex<float> m = std::complex<float>{static_cast<float>(hp_re), static_cast<float>(hp_im)} * p;
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
    q_even_{even_hilbert_taps(num_taps, window)}, q_odd_{even_hilbert_taps(num_taps, window)},
    delay_{(num_taps - 1) / 2} {
  i_history_.assign(delay_, 0.0f);
}

void Hilbert::prepare(std::size_t max_in) {
  const std::size_t half = (max_in + 1) / 2; // most even/odd samples a block can hold
  q_even_.prepare(half);
  q_odd_.prepare(half);
  even_in_.reserve(half);
  odd_in_.reserve(half);
  out_.reserve(max_in);
}

std::span<const std::complex<float>> Hilbert::process(std::span<const float> in) {
  const std::size_t n = in.size();

  // Deinterleave by GLOBAL parity (carried in samples_seen_) so each half-rate
  // stream is exactly the even / odd samples of the whole signal, independent of
  // how it was chunked. A block starting on a global-even index has ceil(n/2)
  // even samples; on an odd index, floor(n/2).
  const bool start_odd = (samples_seen_ & 1u) != 0;
  const std::size_t even_count = (n + (start_odd ? 0 : 1)) / 2;
  const auto ev_in = even_in_.write_n(even_count);
  const auto od_in = odd_in_.write_n(n - even_count);
  for (std::size_t i = 0, ei = 0, oi = 0; i < n; ++i) {
    if (((i & 1u) != 0) == start_odd)
      ev_in[ei++] = in[i];
    else
      od_in[oi++] = in[i];
  }

  // Quadrature plane: each polyphase FIR is the even-tap kernel over one parity;
  // its output for the j-th same-parity sample is Q at that sample's index. The
  // two are interleaved back below — bit-identical to the full-kernel transform.
  const auto qe = q_even_.process(ev_in);
  const auto qo = q_odd_.process(od_in);

  // In-phase plane: the input delayed by the FIR group delay so I and Q align.
  // The first `delay_` outputs read the tail carried from the previous block; the
  // rest read this block. Reads happen before the history is rewritten below.
  const auto out = out_.write_n(n);
  for (std::size_t i = 0, ei = 0, oi = 0; i < n; ++i) {
    const float q = (((i & 1u) != 0) == start_odd) ? qe[ei++] : qo[oi++];
    const float iplane = i < delay_ ? i_history_[i] : in[i - delay_];
    out[i] = std::complex<float>{iplane, q};
  }

  // Carry the last `delay_` inputs for the next block. If this block is shorter
  // than the delay, slide the existing history left by n (shift_left, not an
  // overlapping copy) and append the block — so the result is independent of how
  // the stream is chunked.
  if (delay_ > 0) {
    const auto m = static_cast<std::ptrdiff_t>(n);
    if (n >= delay_)
      std::copy(in.end() - static_cast<std::ptrdiff_t>(delay_), in.end(), i_history_.begin());
    else {
      std::shift_left(i_history_.begin(), i_history_.end(), m);
      std::copy(in.begin(), in.end(), i_history_.end() - m);
    }
  }
  samples_seen_ += n;
  return out_.view();
}

} // namespace palindrome::demod
