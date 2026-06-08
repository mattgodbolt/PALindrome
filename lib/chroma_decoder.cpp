#include "palindrome/chroma_decoder.hpp"

#include "palindrome/cmul.hpp"
#include "palindrome/phase.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace palindrome::video {

namespace {
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kApc = 0.1; // APC EMA rate (averages the ±45° swing over ~10 lines)
constexpr double kIdent = 0.1; // ident leaky-integrator rate for the PAL-switch bistable

// Keep the chroma band-pass top edge below Nyquist (the AirSpy's 10 MS/s only
// just spans the subcarrier, so a 5 MHz edge would otherwise overrun it).
[[nodiscard]] double clamp_high(double hi, double sample_rate_hz) noexcept {
  return std::min(hi, 0.49 * sample_rate_hz);
}
} // namespace

ChromaDecoder::ChromaDecoder(const ChromaDecoderConfig &cfg) :
    cfg_{cfg}, bandpass_{dsp::bandpass_kernel(cfg.bandpass_taps, cfg.sample_rate_hz, cfg.band_lo_hz,
                   clamp_high(cfg.band_hi_hz, cfg.sample_rate_hz))},
    lp_u_{dsp::lowpass_kernel(cfg.demod_lp_taps, cfg.sample_rate_hz, cfg.uv_bandwidth_hz)},
    lp_v_{dsp::lowpass_kernel(cfg.demod_lp_taps, cfg.sample_rate_hz, cfg.uv_bandwidth_hz)},
    // Luma notch length = band-pass + demod low-pass lengths, so its group delay
    // equals the chroma path's and the two rails stay aligned at the screen.
    lp_luma_{dsp::notch_kernel(cfg.bandpass_taps + cfg.demod_lp_taps - 1, cfg.sample_rate_hz,
        cfg.subcarrier_hz - cfg.luma_notch_half_hz,
        clamp_high(cfg.subcarrier_hz + cfg.luma_notch_half_hz, cfg.sample_rate_hz))} {
  if (!(cfg_.sample_rate_hz > 0.0))
    throw std::invalid_argument{"ChromaDecoder: sample_rate_hz must be positive"};
  if (!(cfg_.subcarrier_hz > 0.0 && cfg_.subcarrier_hz < cfg_.sample_rate_hz / 2.0))
    throw std::invalid_argument{"ChromaDecoder: subcarrier_hz out of range"};
  if (!(cfg_.burst_gate_lo >= 0.0 && cfg_.burst_gate_lo < cfg_.burst_gate_hi && cfg_.burst_gate_hi < 1.0))
    throw std::invalid_argument{"ChromaDecoder: burst gate must be 0 <= lo < hi < 1"};
  // Odd taps give linear phase and an integer group delay; the luma-notch length
  // (bandpass + demod - 1) and the colour registration (group_delay_samples) both
  // assume it, so luma and chroma stay co-registered.
  if (cfg_.bandpass_taps % 2 == 0 || cfg_.demod_lp_taps % 2 == 0)
    throw std::invalid_argument{"ChromaDecoder: bandpass_taps and demod_lp_taps must be odd"};
  nco_omega_ = cfg_.subcarrier_hz / cfg_.sample_rate_hz;
  nco_step_ = std::polar(1.0, kTwoPi * nco_omega_);
  // The comb delay is one line; size the ring for the longest line we expect.
  const double nominal_line = cfg_.sample_rate_hz / kNominalLineHz;
  line_len_ = static_cast<std::size_t>(std::lround(nominal_line));
  ring_cap_ = static_cast<std::size_t>(nominal_line * 1.5) + 2;
  u_ring_.assign(ring_cap_, 0.0f);
  v_ring_.assign(ring_cap_, 0.0f);
}

void ChromaDecoder::prepare(std::size_t max_in) {
  bandpass_.prepare(max_in);
  lp_u_.prepare(max_in);
  lp_v_.prepare(max_in);
  lp_luma_.prepare(max_in);
  mix_u_.reserve(max_in);
  mix_v_.reserve(max_in);
  out_.reserve(max_in);
}

void ChromaDecoder::finalize_line() {
  if (burst_count_ == 0)
    return;
  parity_ = !parity_; // the PAL-switch bistable: the gate closes exactly once per line
  // The line's burst phasor: (mean raw_U, mean raw_V) over the gate. Its angle is
  // φ, the LO-vs-subcarrier phase for this line.
  const double bc = burst_acc_.real() / static_cast<double>(burst_count_);
  const double bs = burst_acc_.imag() / static_cast<double>(burst_count_);
  const double mag = std::hypot(bc, bs);
  burst_acc_ = {0.0, 0.0};
  burst_count_ = 0;

  // Colour-killer: a slow-decay peak tracks the live burst level; lines whose
  // burst is well below it (the vertical-interval/VBI lines, dropouts) carry no
  // colour. Skip them entirely — don't move the APC or rotation — and hold the
  // last good line's rotation across the gap.
  burst_ref_ = std::max(burst_ref_ * 0.9995, mag);
  if (mag < 0.3 * burst_ref_)
    return;
  const double phi = std::atan2(bs, bc);

  // APC: a slow EMA of the burst phasor locks the reference onto the -U axis. The
  // ±45° swing alternates line to line, so it averages out and the EMA tracks the
  // steady LO-vs-subcarrier offset (and any slow source drift), like a real APC.
  apc_phasor_ = (1.0 - kApc) * apc_phasor_ + kApc * std::complex<double>{bc, bs};
  // The ACC level: the same phasor's magnitude is the swing-averaged burst (the
  // ±45° swing shrinks it by cos45°, so ×√2 recovers |burst|). One tracker, not two.
  burst_amp_ = std::numbers::sqrt2 * std::abs(apc_phasor_);
  const double psi_axis = std::atan2(apc_phasor_.imag(), apc_phasor_.real());
  const double swing = dsp::wrap_angle(phi - psi_axis); // ±45°; its sign is the V-sense

  // PAL-switch bistable + ident. parity_ alternates the V-switch every line; the
  // ident leaky-integrates whether the burst's measured sense agrees with the
  // bistable's claim and flips polarity_ on persistent disagreement. Driving the
  // flip off the bistable (not the raw per-line sign) rides out single noisy
  // bursts; the ident only re-phases the bistable, never a per-line decision.
  const double predicted = (parity_ == polarity_) ? 1.0 : -1.0;
  const double measured = (swing >= 0.0) ? -1.0 : 1.0; // +V-burst lines (swing<0) take no flip
  ident_ += kIdent * (predicted * measured - ident_);
  if (ident_ < -0.5) {
    polarity_ = !polarity_;
    ident_ = 0.0;
  }
  v_flip_ = (parity_ == polarity_) ? 1.0 : -1.0;
  swing_deg_ = std::abs(swing) * 180.0 / std::numbers::pi;

  // De-rotate by π - psi_axis so the -U axis lands on the negative-real axis;
  // U = -Re, V = Im (switch-corrected by v_flip_) then follow in process().
  const double psi = std::numbers::pi - psi_axis;
  psi_cos_ = std::cos(psi);
  psi_sin_ = std::sin(psi);
}

std::span<const ChromaSample> ChromaDecoder::process(
    std::span<const float> envelope, std::span<const BeamSample> hbeam) {
  const std::size_t n = std::min(envelope.size(), hbeam.size());

  // Pass 1: isolate the chroma subcarrier and synchronously demodulate it against
  // the fixed crystal LO. nco_phasor_ is e^{+i2πθ}, so (c·cosθ, c·sinθ) are the
  // raw U and V quadratures (matching cos/sin demodulation).
  const auto chroma = bandpass_.process(envelope.first(n));
  const auto mu = mix_u_.write_n(n);
  const auto mv = mix_v_.write_n(n);
  for (std::size_t k = 0; k < n; ++k) {
    // Snapshot the phasor down to float for the per-sample mix; it still advances
    // in double (drift), but the product only needs float (see the precision rule
    // in CLAUDE.md), as the AirSpy ComplexAmEnvelope mix already does.
    const auto re = static_cast<float>(nco_phasor_.real());
    const auto im = static_cast<float>(nco_phasor_.imag());
    mu[k] = chroma[k] * re;
    mv[k] = chroma[k] * im;
    nco_phasor_ = dsp::cmul(nco_phasor_, nco_step_);
    if (++since_renorm_ >= 1024) {
      nco_phasor_ /= std::abs(nco_phasor_);
      since_renorm_ = 0;
    }
  }

  // Pass 2: band-limit the quadratures to the chroma bandwidth (this is where the
  // 2·fsc image and noise go), and notch the envelope to a chroma-free luma.
  const auto raw_u = lp_u_.process(mu);
  const auto raw_v = lp_v_.process(mv);
  const auto luma = lp_luma_.process(envelope.first(n));

  // Pass 3: gate the burst out of the clean quadratures (closing the gate updates
  // this line's rotation), then de-rotate each sample to transmitted U/V, flip V
  // on PAL-style lines, and run the 1H comb. Chroma is normalised by the smoothed
  // burst amplitude (ACC), so the saturation knob is capture-independent.
  const auto out = out_.write_n(n);
  for (std::size_t k = 0; k < n; ++k) {
    if (hbeam[k].line_start) {
      const std::size_t len = sample_index_ - last_line_start_;
      if (len > 0 && len < ring_cap_)
        line_len_ = len;
      last_line_start_ = sample_index_;
      burst_acc_ = {0.0, 0.0};
      burst_count_ = 0;
      in_gate_prev_ = false;
    }

    const double i = static_cast<double>(raw_u[k]);
    const double q = static_cast<double>(raw_v[k]);

    // Gate the burst on h_phase, which is anchored to the sync leading edge (it
    // wraps to 0 there) — unlike line_start, which fires at the trailing edge.
    const float hp = hbeam[k].h_phase;
    const bool in_gate = hp >= static_cast<float>(cfg_.burst_gate_lo) && hp < static_cast<float>(cfg_.burst_gate_hi);
    if (in_gate) {
      burst_acc_ += std::complex<double>{i, q};
      ++burst_count_;
    }
    else if (in_gate_prev_) {
      finalize_line(); // gate closed — recover this line's rotation and class
    }
    in_gate_prev_ = in_gate;
    ++sample_index_;

    // R(ψ): U = cosψ·rawU - sinψ·rawV; V_mod = sinψ·rawU + cosψ·rawV. V flips on
    // PAL-style lines to recover transmitted V.
    const double scale = burst_amp_ > 1e-9 ? 1.0 / burst_amp_ : 0.0;
    float u = static_cast<float>((psi_cos_ * i - psi_sin_ * q) * scale);
    float v = static_cast<float>((psi_sin_ * i + psi_cos_ * q) * v_flip_ * scale);

    if (cfg_.delay_line) {
      // ring_pos_ is kept wrapped in [0, ring_cap_) so the comb's read index is a
      // conditional subtract, not a (non-power-of-two) modulo per sample. line_len_
      // < ring_cap_, so the read offset wraps at most once.
      const std::size_t wi = ring_pos_;
      const std::size_t ri = wi >= line_len_ ? wi - line_len_ : wi + ring_cap_ - line_len_;
      const float u_avg = 0.5f * (u + u_ring_[ri]);
      const float v_avg = 0.5f * (v + v_ring_[ri]);
      u_ring_[wi] = u;
      v_ring_[wi] = v;
      u = u_avg;
      v = v_avg;
      if (++ring_pos_ == ring_cap_)
        ring_pos_ = 0;
    }
    out[k] = ChromaSample{.luma = luma[k], .u = u, .v = v};
  }
  return out_.view();
}

} // namespace palindrome::video
