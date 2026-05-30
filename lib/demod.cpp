#include "palindrome/demod.hpp"

#include "palindrome/biquad.hpp"
#include "palindrome/dc_blocker.hpp"
#include "palindrome/restrict_ptr.hpp"

#include <cmath>
#include <format>

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

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
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
  const std::span<float> mi = mix_i_.write_n(n);
  const std::span<float> mq = mix_q_.write_n(n);

  // Shift the carrier to DC. The phasor recurrence carries across calls, so the
  // mix is independent of how the input is chunked (to within float rounding).
  for (std::size_t k = 0; k < n; ++k) {
    const std::complex<float> p{static_cast<float>(phasor_.real()), static_cast<float>(phasor_.imag())};
    const std::complex<float> m = in[k] * p;
    mi[k] = m.real();
    mq[k] = m.imag();
    phasor_ *= step_;
    if (++since_renorm_ >= 1024) {
      phasor_ /= std::abs(phasor_); // keep |phasor_| == 1 against accumulated drift
      since_renorm_ = 0;
    }
  }

  const std::span<const float> filtered_i = i_filter_.process(mi);
  const std::span<const float> filtered_q = q_filter_.process(mq);
  const std::size_t m = filtered_i.size();
  out_.reserve(m);
  envelope_magnitude(filtered_i.data(), filtered_q.data(), out_.write_n(m).data(), m);
  return out_.view();
}

VisionChain build_vision_chain(const VisionChainConfig &cfg) {
  VisionChain v;
  if (cfg.sound_trap_hz) {
    v.chain.add(dsp::notch(cfg.sample_rate_hz, *cfg.sound_trap_hz, cfg.sound_q));
    v.trap_notes.push_back(std::format("sound {:.3f} MHz (Q {:g})", *cfg.sound_trap_hz / 1e6, cfg.sound_q));
  }
  // A constant ADC offset would mix down onto the carrier and beat into the envelope.
  v.chain.add(dsp::DcBlocker{});
  v.chain.add(
      AmEnvelope{cfg.sample_rate_hz, cfg.vision_carrier_hz, cfg.cutoff_hz, cfg.num_taps, cfg.window, cfg.decimation});

  // Decimation folds anything above the decimated Nyquist back into band; the
  // low-pass must clear it. Warn rather than fail — it's a quality call.
  if (const double decimated_nyquist = cfg.sample_rate_hz / (2.0 * static_cast<double>(cfg.decimation));
      cfg.cutoff_hz >= decimated_nyquist)
    v.warnings.push_back(std::format("cutoff {:g} MHz exceeds the decimated Nyquist {:g} MHz; expect aliasing",
        cfg.cutoff_hz / 1e6, decimated_nyquist / 1e6));

  return v;
}

} // namespace palindrome::demod
