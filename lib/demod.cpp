#include "palindrome/demod.hpp"

#include "palindrome/cmul.hpp"
#include "palindrome/restrict_ptr.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

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
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kDcPole = 0.9999; // complex DC-blocker feedback, matching dsp::DcBlocker

// The template's voltage response at a signed offset from the carrier: the
// voltage-linear Nyquist flank times the dB shape table times the dB notch.
double template_voltage(const IfTemplate &t, double rel_hz) {
  const double flank = std::clamp(0.5 * (1.0 + rel_hz / t.flank_half_width_hz), 0.0, 1.0);
  if (flank == 0.0)
    return 0.0;
  double db = 0.0;
  if (!t.shape.empty()) {
    if (rel_hz < t.shape.front().offset_hz || rel_hz > t.shape.back().offset_hz)
      return 0.0; // outside the table's span the channel has ended: true stopband
    for (std::size_t i = 1; i < t.shape.size(); ++i) {
      const auto &[lo_hz, lo_db] = t.shape[i - 1];
      const auto &[hi_hz, hi_db] = t.shape[i];
      if (rel_hz <= hi_hz) {
        db = lo_db + (hi_db - lo_db) * (rel_hz - lo_hz) / (hi_hz - lo_hz);
        break;
      }
    }
  }
  const double dip = std::max(0.0, 1.0 - std::abs(rel_hz - t.sound_notch_offset_hz) / t.sound_notch_half_width_hz);
  db += t.sound_notch_db * dip;
  return flank * std::pow(10.0, db / 20.0);
}

// The ripple's phase at an offset from the carrier: group delay tau(f) =
// ripple * cos(2*pi*f/period) about linear phase, so phase = -2*pi * integral
// of tau, which is -ripple*period*sin(2*pi*f/period). Zero at the carrier, so
// the ripple never moves the level-bearing carrier response.
double ripple_phase(const IfTemplate &t, double rel_hz) {
  if (t.gd_ripple_ns == 0.0)
    return 0.0;
  return -t.gd_ripple_ns * 1e-9 * t.gd_ripple_period_hz * std::sin(kTwoPi * rel_hz / t.gd_ripple_period_hz);
}

// The realised complex kernel's response at an absolute frequency (direct DFT
// of the taps; design-time only).
std::complex<double> taps_response_at(
    std::span<const double> re, std::span<const double> im, double f_hz, double sample_rate_hz) {
  std::complex<double> acc{0.0, 0.0};
  for (std::size_t n = 0; n < re.size(); ++n)
    acc +=
        std::complex<double>{re[n], im[n]} * std::polar(1.0, -kTwoPi * f_hz * static_cast<double>(n) / sample_rate_hz);
  return acc;
}

// Synthesise the one-sided complex kernel realising `shape` around carrier_hz:
// frequency sampling over the N DFT bins (every bin above Nyquist - the negative
// frequencies - stays zero, which is what subsumes the Hilbert), an inverse DFT,
// a window, and a final normalisation pinning the response at the carrier to the
// template's own value, since that is the point the IF AGC levels against.
std::pair<std::vector<float>, std::vector<float>> if_taps(
    double sample_rate_hz, double carrier_hz, const IfTemplate &shape, std::size_t num_taps, dsp::Window window) {
  if (num_taps < 3 || num_taps % 2 == 0)
    throw std::invalid_argument{"VisionIf: num_taps must be odd and at least 3"};
  if (!(carrier_hz > 0.0) || !(carrier_hz < sample_rate_hz / 2.0))
    throw std::invalid_argument{"VisionIf: carrier must be in (0, sample_rate / 2)"};
  if (!(shape.flank_half_width_hz > 0.0) || !(shape.sound_notch_half_width_hz > 0.0) ||
      !(shape.gd_ripple_period_hz > 0.0))
    throw std::invalid_argument{"VisionIf: template widths and ripple period must be positive"};
  for (std::size_t i = 1; i < shape.shape.size(); ++i)
    if (shape.shape[i].offset_hz <= shape.shape[i - 1].offset_hz)
      throw std::invalid_argument{"VisionIf: shape table offsets must be strictly increasing"};
  // The curve must have died away by Nyquist. Truncating it mid-shoulder puts a
  // cliff in the frequency sampling whose windowed interpolation bleeds straight
  // across into the negative-frequency bins - a near-full-strength carrier image
  // folding onto the picture, with no other symptom (measured -4.8 dB worst case
  // for a 16 MS/s rate that can't hold the template). -40 dB is comfortably
  // below anything the picture would show.
  if (template_voltage(shape, sample_rate_hz / 2.0 - carrier_hz) > 0.01)
    throw std::invalid_argument{"VisionIf: the template does not fit below Nyquist at this sample rate/carrier - the "
                                "truncated curve would fold a carrier image onto the picture"};

  const auto n_taps = num_taps;
  const auto nd = static_cast<double>(n_taps);
  const double centre = (nd - 1.0) / 2.0;
  std::vector<std::complex<double>> bins(n_taps);
  for (std::size_t k = 0; k < n_taps; ++k) {
    const double f = static_cast<double>(k) * sample_rate_hz / nd;
    if (f > sample_rate_hz / 2.0)
      continue; // negative-frequency bin: zero keeps the kernel one-sided
    const double a = template_voltage(shape, f - carrier_hz);
    if (a == 0.0)
      continue;
    // Ripple about linear phase; the linear part centres the impulse response.
    bins[k] = std::polar(a, ripple_phase(shape, f - carrier_hz) - kTwoPi * f * centre / sample_rate_hz);
  }

  std::vector<double> re(n_taps);
  std::vector<double> im(n_taps);
  for (std::size_t n = 0; n < n_taps; ++n) {
    std::complex<double> acc{0.0, 0.0};
    for (std::size_t k = 0; k < n_taps; ++k) {
      if (bins[k] == std::complex<double>{0.0, 0.0})
        continue;
      acc += bins[k] * std::polar(1.0, kTwoPi * static_cast<double>(k * n % n_taps) / nd);
    }
    const double w = dsp::window_value(window, static_cast<double>(n), nd - 1.0);
    re[n] = acc.real() / nd * w;
    im[n] = acc.imag() / nd * w;
  }

  const double target = template_voltage(shape, 0.0);
  if (!(target > 0.0))
    throw std::invalid_argument{"VisionIf: template has no response at the carrier"};
  const double realised = std::abs(taps_response_at(re, im, carrier_hz, sample_rate_hz));
  if (!(realised > 0.0)) // != would let a denormal-or-NaN realisation poison every tap
    throw std::invalid_argument{"VisionIf: realised kernel has no response at the carrier"};
  const double scale = target / realised;
  std::vector<float> re_f(n_taps);
  std::vector<float> im_f(n_taps);
  for (std::size_t n = 0; n < n_taps; ++n) {
    re_f[n] = static_cast<float>(re[n] * scale);
    im_f[n] = static_cast<float>(im[n] * scale);
  }
  return {std::move(re_f), std::move(im_f)};
}
} // namespace

IfTemplate saw80_template() {
  return {
      .flank_half_width_hz = 0.75e6,
      // 0 dB through the lower active band; a shoulder from 3.5 MHz rolling to
      // -12 dB by 5.9 MHz (chroma at +4.43 sits ~4.7 dB down, which the
      // decoder's ACC makes up, as the set's chroma bandpass did); the channel
      // ends past the sound notch. The table starts at the System I vestige
      // edge (-1.25 MHz); the flank has already closed the response by there.
      .shape = {{-1.25e6, 0.0}, {3.5e6, 0.0}, {5.9e6, -12.0}, {6.8e6, -50.0}},
      .sound_notch_offset_hz = 6.0e6,
      .sound_notch_db = -26.0,
      .sound_notch_half_width_hz = 0.4e6,
      .gd_ripple_ns = 50.0,
      .gd_ripple_period_hz = 1.0e6,
  };
}

IfTemplate saw90_template() {
  return {
      .flank_half_width_hz = 0.75e6,
      // Flat through the chroma, a short shoulder into the channel edge.
      .shape = {{-1.25e6, 0.0}, {4.8e6, 0.0}, {5.9e6, -8.0}, {6.8e6, -50.0}},
      .sound_notch_offset_hz = 6.0e6,
      .sound_notch_db = -40.0, // split-IF era: sound leaves the vision path early
      .sound_notch_half_width_hz = 0.3e6,
      .gd_ripple_ns = 8.0,
      .gd_ripple_period_hz = 1.0e6,
  };
}

VisionIf::VisionIf(double sample_rate_hz, double carrier_hz, const IfTemplate &shape, std::size_t num_taps,
    dsp::Window window, std::size_t decimation) :
    VisionIf{if_taps(sample_rate_hz, carrier_hz, shape, num_taps, window), decimation} {}

VisionIf::VisionIf(std::pair<std::vector<float>, std::vector<float>> taps, std::size_t decimation) :
    re_filter_{std::move(taps.first), decimation}, im_filter_{std::move(taps.second), decimation} {}

void VisionIf::prepare(std::size_t max_in) {
  re_filter_.prepare(max_in);
  im_filter_.prepare(max_in);
  out_.reserve(re_filter_.max_output_for(max_in));
}

std::span<const float> VisionIf::process(std::span<const float> in) {
  const auto i = re_filter_.process(in);
  const auto q = im_filter_.process(in);
  const std::size_t m = i.size();
  envelope_magnitude(i.data(), q.data(), out_.write_n(m).data(), m);
  return out_.view();
}

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
