#include "palindrome/video.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace palindrome::video {

namespace {
// Wrap a phase error into [-0.5, 0.5). Past that range the loop would push the
// wrong way around the cycle; this is the standard PLL phase-detector range.
[[nodiscard]] double wrap_error(double e) noexcept {
  e -= std::floor(e);
  return e < 0.5 ? e : e - 1.0;
}

// Slicer hysteresis (fraction of the floor-to-peak range) and the per-sample
// release time constant of the peak/floor trackers. Analog equivalents of
// fixed circuit values — picked once, not user-tuned.
constexpr double kSyncHysteresis = 0.05;
constexpr double kLevelRelease = 0.999999; // ~60 ms settle at 16 MS/s
constexpr double kVsyncHysteresis = 0.05; // integrator slice hysteresis for vertical sync
} // namespace

// === SyncSeparator ===

SyncSeparator::SyncSeparator(const SyncSeparatorConfig &cfg) : cfg_{cfg} {
  if (!(cfg_.sample_rate_hz > 0.0))
    throw std::invalid_argument{"SyncSeparator: sample_rate_hz must be positive"};
  if (!(cfg_.sync_level + kSyncHysteresis * 0.5 < 1.0 && cfg_.sync_level - kSyncHysteresis * 0.5 > 0.0))
    throw std::invalid_argument{"SyncSeparator: sync_level +/- hysteresis must stay within (0, 1)"};
}

void SyncSeparator::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const SyncSample> SyncSeparator::process(std::span<const float> envelope) {
  const std::size_t n = envelope.size();
  out_.reserve(n);
  const auto dst = out_.write_n(n);

  // Seed peak/floor from the first sample we ever see, so the slice level is
  // meaningful from sample 0 rather than starting at a degenerate range of 0.
  if (!seeded_ && n > 0) {
    peak_ = floor_ = static_cast<double>(envelope[0]);
    seeded_ = true;
  }

  const double enter_frac = cfg_.sync_level + kSyncHysteresis * 0.5;
  const double leave_frac = cfg_.sync_level - kSyncHysteresis * 0.5;
  const double release = 1.0 - kLevelRelease;

  for (std::size_t k = 0; k < n; ++k) {
    const auto env = static_cast<double>(envelope[k]);

    // Track peak (sync tip) and floor (white) with fast attack to a new
    // extreme and slow release back toward the other end. Both release terms
    // read the pre-update pair, so there's no within-sample order dependency.
    const double range_pre = peak_ - floor_;
    peak_ = std::max(env, peak_ - range_pre * release);
    floor_ = std::min(env, floor_ + range_pre * release);
    const double range = peak_ - floor_;

    // Slice with hysteresis: enter the sync region high, leave it lower, so
    // chroma ripple on a transition can't chatter the bit. The range > 0 guard
    // stops a degenerate flat input (range == 0, where the enter test would be
    // trivially true) from latching permanently into sync.
    if (!sync_ && range > 0.0 && env >= floor_ + range * enter_frac)
      sync_ = true;
    else if (sync_ && env < floor_ + range * leave_frac)
      sync_ = false;

    dst[k] = SyncSample{.sync = sync_};
  }

  return out_.view();
}

// === HorizontalSweep ===

HorizontalSweep::HorizontalSweep(const HorizontalSweepConfig &cfg) : cfg_{cfg} {
  if (!(cfg_.sample_rate_hz > 0.0))
    throw std::invalid_argument{"HorizontalSweep: sample_rate_hz must be positive"};
  if (!(cfg_.nominal_line_hz > 0.0 && cfg_.nominal_line_hz < cfg_.sample_rate_hz / 2.0))
    throw std::invalid_argument{"HorizontalSweep: nominal_line_hz out of range"};
  if (!(cfg_.min_pulse_fraction > 0.0 && cfg_.min_pulse_fraction < cfg_.max_pulse_fraction))
    throw std::invalid_argument{"HorizontalSweep: min_pulse_fraction must be in (0, max_pulse_fraction)"};
  if (!(cfg_.max_pulse_fraction < cfg_.min_line_fraction))
    throw std::invalid_argument{"HorizontalSweep: max_pulse_fraction must be < min_line_fraction"};
  if (!(cfg_.min_line_fraction < 1.0))
    throw std::invalid_argument{"HorizontalSweep: min_line_fraction must be < 1"};
  if (!(cfg_.pll_kp >= 0.0 && cfg_.pll_kp <= 1.0))
    throw std::invalid_argument{"HorizontalSweep: pll_kp must be in [0, 1]"};
  if (!(cfg_.omega_clamp > 0.0 && cfg_.omega_clamp < 1.0))
    throw std::invalid_argument{"HorizontalSweep: omega_clamp must be in (0, 1)"};
  omega_ = cfg_.nominal_line_hz / cfg_.sample_rate_hz;
}

void HorizontalSweep::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const BeamSample> HorizontalSweep::process(std::span<const SyncSample> in) {
  const std::size_t n = in.size();
  out_.reserve(n);
  const auto dst = out_.write_n(n);

  const double nominal_omega = cfg_.nominal_line_hz / cfg_.sample_rate_hz;
  const double omega_lo = nominal_omega * (1.0 - cfg_.omega_clamp);
  const double omega_hi = nominal_omega * (1.0 + cfg_.omega_clamp);

  for (std::size_t k = 0; k < n; ++k) {
    const bool sync = in[k].sync;
    bool line_start = false;

    if (sync && !prev_sync_) {
      // Leading edge of a sync pulse: the timing reference. Stash where the
      // oscillator phase was here; we commit to it only once the trailing edge
      // confirms the pulse was line-width (not a vertical-interval pulse).
      leading_edge_sample_ = sample_index_;
      leading_edge_phase_ = phase_;
    }
    else if (!sync && prev_sync_) {
      // Trailing edge: the pulse width is now known. Accept it as line sync
      // only if it falls in the line-sync width window AND the horizontal-hold
      // lockout has elapsed since the last accepted edge. Width gating rejects
      // the broad pulses (too wide) and equalising pulses (too narrow); the
      // lockout rejects chroma-ripple retriggers on the back porch.
      const auto width = static_cast<double>(sample_index_ - leading_edge_sample_);
      const auto since = static_cast<double>(sample_index_ - last_accepted_sample_);
      const double min_pulse = cfg_.min_pulse_fraction / omega_;
      const double max_pulse = cfg_.max_pulse_fraction / omega_;
      const double min_gap = cfg_.min_line_fraction / omega_;
      // The hold gate needs a prior edge to measure against; before the first
      // accepted edge there is none, so don't let it veto edge zero.
      const bool gap_ok = !have_accepted_ || since >= min_gap;
      if (width >= min_pulse && width <= max_pulse && gap_ok) {
        // PI correction anchored to the LEADING edge (the sharp, stable sync
        // transition). err is the leading edge's phase, which should have been
        // 0. kp snaps it there; ki nudges omega toward the true line rate,
        // anti-windup clamped so a biased acquisition can't run omega away.
        const double err = wrap_error(leading_edge_phase_);
        phase_ -= cfg_.pll_kp * err;
        omega_ = std::clamp(omega_ - cfg_.pll_ki * err, omega_lo, omega_hi);
        last_accepted_sample_ = sample_index_;
        have_accepted_ = true;
        ++accepted_;
        line_start = true;
      }
      else {
        ++rejected_;
      }
    }

    dst[k] = BeamSample{
        .h_phase = static_cast<float>(phase_ - std::floor(phase_)),
        .line_start = line_start,
    };

    // Advance the oscillator and wrap into [0, 1). floor() covers the rare case
    // where a kp snap pushed phase_ slightly negative.
    phase_ += omega_;
    phase_ -= std::floor(phase_);
    prev_sync_ = sync;
    ++sample_index_;
  }

  return out_.view();
}

// === VerticalSync ===

VerticalSync::VerticalSync(const VerticalSyncConfig &cfg) : cfg_{cfg} {
  if (!(cfg_.sample_rate_hz > 0.0))
    throw std::invalid_argument{"VerticalSync: sample_rate_hz must be positive"};
  if (!(cfg_.nominal_field_hz > 0.0 && cfg_.nominal_field_hz < cfg_.sample_rate_hz / 2.0))
    throw std::invalid_argument{"VerticalSync: nominal_field_hz out of range"};
  if (!(cfg_.nominal_line_hz > cfg_.nominal_field_hz))
    throw std::invalid_argument{"VerticalSync: nominal_line_hz must exceed nominal_field_hz"};
  // Lower bound 1/line_samples keeps alpha <= 1, so the integrator stays a
  // convex combination (integ_ can't overshoot [0, 1] and ring).
  if (!(cfg_.integrator_tc_lines * cfg_.sample_rate_hz / cfg_.nominal_line_hz >= 1.0))
    throw std::invalid_argument{"VerticalSync: integrator_tc_lines too small (alpha would exceed 1)"};
  if (!(cfg_.vsync_level - kVsyncHysteresis * 0.5 > 0.0 && cfg_.vsync_level + kVsyncHysteresis * 0.5 < 1.0))
    throw std::invalid_argument{"VerticalSync: vsync_level +/- hysteresis must stay within (0, 1)"};
  if (!(cfg_.min_field_fraction >= 0.0 && cfg_.min_field_fraction < 1.0))
    throw std::invalid_argument{"VerticalSync: min_field_fraction must be in [0, 1)"};
  if (!(cfg_.pll_kp >= 0.0 && cfg_.pll_kp <= 1.0))
    throw std::invalid_argument{"VerticalSync: pll_kp must be in [0, 1]"};
  if (!(cfg_.omega_clamp > 0.0 && cfg_.omega_clamp < 1.0))
    throw std::invalid_argument{"VerticalSync: omega_clamp must be in (0, 1)"};

  // Integrator time constant expressed in lines: alpha = 1 / (tc * samples/line).
  const double line_samples = cfg_.sample_rate_hz / cfg_.nominal_line_hz;
  alpha_ = 1.0 / (cfg_.integrator_tc_lines * line_samples);
  omega_ = cfg_.nominal_field_hz / cfg_.sample_rate_hz;
}

void VerticalSync::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const VSample> VerticalSync::process(std::span<const SyncSample> in) {
  const std::size_t n = in.size();
  out_.reserve(n);
  const auto dst = out_.write_n(n);

  const double nominal_omega = cfg_.nominal_field_hz / cfg_.sample_rate_hz;
  const double omega_lo = nominal_omega * (1.0 - cfg_.omega_clamp);
  const double omega_hi = nominal_omega * (1.0 + cfg_.omega_clamp);
  const double enter = cfg_.vsync_level + kVsyncHysteresis * 0.5;
  const double leave = cfg_.vsync_level - kVsyncHysteresis * 0.5;

  for (std::size_t k = 0; k < n; ++k) {
    // Low-pass the sync bit toward its duty cycle. Line sync settles it near
    // ~0.07; the broad-pulse train drives it toward ~0.84.
    integ_ += alpha_ * ((in[k].sync ? 1.0 : 0.0) - integ_);

    bool field_start = false;
    // Rising crossing of the slice marks the vertical interval. The hold gate
    // rejects a second crossing within the same field (the integrator can dip
    // and re-cross between broad pulses); the flywheel coasts otherwise.
    if (!in_vsync_ && integ_ >= enter) {
      in_vsync_ = true;
      const auto since = static_cast<double>(sample_index_ - last_field_sample_);
      const double min_gap = cfg_.min_field_fraction / omega_;
      if (!have_field_ || since >= min_gap) {
        // PI correction: v_phase should be 0 at the field-sync anchor.
        const double err = wrap_error(v_phase_);
        v_phase_ -= cfg_.pll_kp * err;
        omega_ = std::clamp(omega_ - cfg_.pll_ki * err, omega_lo, omega_hi);
        last_field_sample_ = sample_index_;
        have_field_ = true;
        ++fields_;
        field_start = true;
      }
    }
    else if (in_vsync_ && integ_ < leave) {
      in_vsync_ = false;
    }

    dst[k] = VSample{
        .v_phase = static_cast<float>(v_phase_ - std::floor(v_phase_)),
        .field_start = field_start,
    };

    v_phase_ += omega_;
    v_phase_ -= std::floor(v_phase_);
    ++sample_index_;
  }

  return out_.view();
}

// === ChromaDecoder ===

namespace {
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kApc = 0.1; // APC EMA rate (averages the ±45° swing over ~10 lines)
constexpr double kIdent = 0.1; // ident leaky-integrator rate for the PAL-switch bistable

// Wrap a phase in radians into [-π, π).
[[nodiscard]] double wrap_angle(double a) noexcept { return std::remainder(a, kTwoPi); }

// A band-stop (notch) kernel: the all-pass impulse minus a band-pass, so luma
// keeps its full bandwidth except the chroma band around fsc (a chroma trap),
// rather than being low-passed soft. Same length as the band-pass, so its group
// delay matches and the notch sits exactly on the subcarrier.
[[nodiscard]] std::vector<float> notch_kernel(std::size_t taps, double sample_rate_hz, double low_hz, double high_hz) {
  auto k = dsp::bandpass_kernel(taps, sample_rate_hz, low_hz, high_hz);
  for (auto &t: k)
    t = -t;
  k[(taps - 1) / 2] += 1.0f; // + unit impulse at the centre tap
  return k;
}

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
    lp_luma_{notch_kernel(cfg.bandpass_taps + cfg.demod_lp_taps - 1, cfg.sample_rate_hz,
        cfg.subcarrier_hz - cfg.luma_notch_half_hz,
        clamp_high(cfg.subcarrier_hz + cfg.luma_notch_half_hz, cfg.sample_rate_hz))} {
  if (!(cfg_.sample_rate_hz > 0.0))
    throw std::invalid_argument{"ChromaDecoder: sample_rate_hz must be positive"};
  if (!(cfg_.subcarrier_hz > 0.0 && cfg_.subcarrier_hz < cfg_.sample_rate_hz / 2.0))
    throw std::invalid_argument{"ChromaDecoder: subcarrier_hz out of range"};
  if (!(cfg_.burst_gate_lo >= 0.0 && cfg_.burst_gate_lo < cfg_.burst_gate_hi && cfg_.burst_gate_hi < 1.0))
    throw std::invalid_argument{"ChromaDecoder: burst gate must be 0 <= lo < hi < 1"};
  nco_omega_ = cfg_.subcarrier_hz / cfg_.sample_rate_hz;
  nco_step_ = std::polar(1.0, kTwoPi * nco_omega_);
  // The comb delay is one line; size the ring for the longest line we expect.
  const double nominal_line = cfg_.sample_rate_hz / 15625.0;
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

  // APC: a slow EMA of the burst phasor locks the reference onto the −U axis. The
  // ±45° swing alternates line to line, so it averages out and the EMA tracks the
  // steady LO-vs-subcarrier offset (and any slow source drift), like a real APC.
  apc_phasor_ = (1.0 - kApc) * apc_phasor_ + kApc * std::complex<double>{bc, bs};
  // The ACC level: the same phasor's magnitude is the swing-averaged burst (the
  // ±45° swing shrinks it by cos45°, so ×√2 recovers |burst|). One tracker, not two.
  burst_amp_ = std::numbers::sqrt2 * std::abs(apc_phasor_);
  const double psi_axis = std::atan2(apc_phasor_.imag(), apc_phasor_.real());
  const double swing = wrap_angle(phi - psi_axis); // ±45°; its sign is the V-sense

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

  // De-rotate by π − psi_axis so the −U axis lands on the negative-real axis;
  // U = −Re, V = Im (switch-corrected by v_flip_) then follow in process().
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
    const double c = static_cast<double>(chroma[k]);
    mu[k] = static_cast<float>(c * nco_phasor_.real());
    mv[k] = static_cast<float>(c * nco_phasor_.imag());
    nco_phasor_ *= nco_step_;
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

    // R(ψ): U = cosψ·rawU − sinψ·rawV; V_mod = sinψ·rawU + cosψ·rawV. V flips on
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

// === Screen ===

Screen::Screen(const ScreenConfig &cfg) :
    cfg_{cfg}, channels_{cfg.colour ? std::size_t{3} : std::size_t{1}},
    bright_(cfg.width * cfg.height * (cfg.colour ? 3 : 1), 0.0f), last_(cfg.width * cfg.height, 0) {
  if (cfg_.width == 0 || cfg_.height == 0)
    throw std::invalid_argument{"Screen: width and height must be positive"};
  if (!(cfg_.saturation >= 0.0))
    throw std::invalid_argument{"Screen: saturation must be non-negative"};
  if (!(cfg_.sample_rate_hz > 0.0 && cfg_.field_hz > 0.0))
    throw std::invalid_argument{"Screen: sample_rate_hz and field_hz must be positive"};
  if (!(cfg_.persistence_fields > 0.0))
    throw std::invalid_argument{"Screen: persistence_fields must be positive"};
  if (!(cfg_.beam_sigma_rows >= 0.0))
    throw std::invalid_argument{"Screen: beam_sigma_rows must be non-negative"};
  if (!(cfg_.nominal_line_hz > cfg_.field_hz))
    throw std::invalid_argument{"Screen: nominal_line_hz must exceed field_hz"};
  // Decay per sample so that brightness falls by 1/e over persistence_fields
  // field periods: tau_samples = persistence * (sample_rate / field_hz).
  const double tau_samples = cfg_.persistence_fields * cfg_.sample_rate_hz / cfg_.field_hz;
  log_decay_ = -1.0 / tau_samples;
  // AGC release: hold the tracked white for a few fields, so the white point is
  // steady against picture content (the slow time constant of a real IF AGC).
  constexpr double kAgcFields = 8.0;
  agc_release_ = std::exp(-1.0 / (kAgcFields * cfg_.sample_rate_hz / cfg_.field_hz));
  // A pixel hit once per frame (two field periods, interlaced) accumulates its
  // deposit to a steady 1/(1 - decay_per_frame); white reads that, so divide it
  // out to put tracked white at full scale.
  const double decay_per_frame = std::exp(log_decay_ * 2.0 * cfg_.sample_rate_hz / cfg_.field_hz);
  phosphor_gain_ = 1.0 / (1.0 - decay_per_frame);
  // Yoke shear: the beam steps (field_hz / line_hz) of a field per line, i.e.
  // this many output rows — the amount to un-creep within each line.
  yoke_tilt_rows_ = static_cast<double>(cfg_.height) * cfg_.field_hz / cfg_.nominal_line_hz;
  // Splat the beam spot out to 2.5 sigma. The spot centre sits at a sub-pixel row
  // that shifts line to line, so tabulate the normalised weights per fraction bin.
  splat_radius_ = cfg_.beam_sigma_rows > 0.0 ? static_cast<std::size_t>(std::ceil(2.5 * cfg_.beam_sigma_rows)) : 0;
  gauss_stride_ = 2 * splat_radius_ + 1;
  gauss_lut_.assign(kGaussBins * gauss_stride_, 0.0);
  const double sigma2 = cfg_.beam_sigma_rows * cfg_.beam_sigma_rows;
  for (std::size_t bin = 0; bin < kGaussBins; ++bin) {
    const double frac = (static_cast<double>(bin) + 0.5) / static_cast<double>(kGaussBins);
    double *row = &gauss_lut_[bin * gauss_stride_];
    double sum = 0.0;
    for (std::size_t k = 0; k < gauss_stride_; ++k) {
      const double d = static_cast<double>(k) - static_cast<double>(splat_radius_) + 0.5 - frac;
      row[k] = sigma2 > 0.0 ? std::exp(-0.5 * d * d / sigma2) : 1.0;
      sum += row[k];
    }
    const double inv = sum > 0.0 ? 1.0 / sum : 0.0;
    for (std::size_t k = 0; k < gauss_stride_; ++k)
      row[k] *= inv;
  }
  // Phosphor decay tables: dt = lo + 256*hi, so exp(a*dt) = lo-table * hi-table.
  // The hi-table runs until the factor underflows below any 8-bit contribution.
  decay_lo_.resize(kDecayMask + 1);
  for (std::size_t lo = 0; lo < decay_lo_.size(); ++lo)
    decay_lo_[lo] = std::exp(log_decay_ * static_cast<double>(lo));
  const double hi_step = static_cast<double>(kDecayMask + 1);
  for (std::size_t hi = 0;; ++hi) {
    const double v = std::exp(log_decay_ * hi_step * static_cast<double>(hi));
    decay_hi_.push_back(v);
    if (v < 1e-12)
      break;
  }
}

// exp(log_decay_ * dt) for integer dt, from the split tables. Beyond the
// hi-table the factor has underflowed, so it reads as zero.
float Screen::decay_for(std::size_t dt) const {
  const std::size_t hi = dt >> kDecayShift;
  if (hi >= decay_hi_.size())
    return 0.0f;
  return static_cast<float>(decay_lo_[dt & kDecayMask] * decay_hi_[hi]);
}

void Screen::prepare(std::size_t) {}

namespace {
// The back porch — the blanking shelf just after the line-sync pulse (h_phase=0
// is the sync leading edge) — sits at black every line, a clean reference for
// DC restoration. As an h_phase window: sync is ~0..0.07, the back porch ~0.09.
constexpr float kBackPorchLo = 0.09f;
constexpr float kBackPorchHi = 0.14f;
constexpr double kBlackTrack = 0.02; // black-reference tracking per back-porch sample
} // namespace

float Screen::drive_of(float luma_f, float h_phase) {
  // luma arrives in negative-modulation envelope units (lower = whiter, see
  // ChromaSample). The electron gun (see Screen::black_): DC-restore black from
  // the back-porch window, then drive = black - luma, cut off below black. Linear
  // (no gamma — the per-gun gamma is applied after the colour matrix). No upper bound.
  const auto luma = static_cast<double>(luma_f);
  if (!seeded_) {
    black_ = luma;
    seeded_ = true;
  }
  if (h_phase >= kBackPorchLo && h_phase < kBackPorchHi)
    black_ += kBlackTrack * (luma - black_);
  return static_cast<float>(std::max(0.0, black_ - luma));
}

namespace {
// One electron gun: drive^gamma, the beam-current curve. 0 stays 0 (cutoff).
[[nodiscard]] float gun(double drive, double gamma) {
  if (drive <= 0.0)
    return 0.0f;
  return static_cast<float>(gamma == 1.0 ? drive : std::pow(drive, gamma));
}
// BT.601 colour-difference -> RGB, in gun-drive space (luma drive is the Y term).
// These match the TDA3561A demodulator ratios: (B−Y)/(R−Y) = kBu/kRv = 1.78, and
// the (G−Y) axis within the datasheet's tolerance (see docs/TDA3561A.md).
constexpr double kRv = 1.140;
constexpr double kGu = -0.395;
constexpr double kGv = -0.581;
constexpr double kBu = 2.032;
} // namespace

void Screen::process(std::span<const ChromaSample> picture, std::span<const BeamSample> hbeam,
    std::span<const VSample> vbeam, const FieldCallback &on_field) {
  const std::size_t n = std::min({picture.size(), hbeam.size(), vbeam.size()});
  for (std::size_t i = 0; i < n; ++i) {
    // A field boundary closes off a completed field: snapshot the phosphor as
    // it stands now (before this sample's deposit), then carry on. Block-size
    // independent — field_start is per-sample, so the snapshots land identically
    // however the input is chunked.
    if (on_field && vbeam[i].field_start)
      on_field(snapshot());

    // Gun drives: luma sets the Y term; the matrix adds the colour difference.
    // Each gun cuts off at zero, then takes its gamma. Grey = the luma gun alone.
    const double drive = static_cast<double>(drive_of(picture[i].luma, hbeam[i].h_phase));
    const float luma_gun = gun(drive, cfg_.gamma);
    // IF AGC: track the peak luma gun (readout white) and the peak luma drive (the
    // chroma reference), fast up, slow down. Tracking drive ties the colour to the
    // luma scale: U/V arrive normalised to the burst, so scaling them by the white
    // drive makes saturation a bounded fraction of white, not a free multiplier.
    white_ref_ = std::max(static_cast<double>(luma_gun), white_ref_ * agc_release_);
    white_drive_ = std::max(drive, white_drive_ * agc_release_);
    // Retrace blanking: the beam is off through sync, back porch and burst, the
    // line-blanking pulse of a real set. This is why the burst — a big subcarrier
    // sitting in the back porch — never paints a coloured bar; it's blanked, not
    // gated on luma.
    if (hbeam[i].h_phase < static_cast<float>(cfg_.h_blank)) {
      ++sample_index_;
      continue;
    }

    // R/G/B guns: luma drive plus the colour difference, each cut off then gamma'd.
    float gun_rgb[3] = {luma_gun, 0.0f, 0.0f};
    if (channels_ == 3) {
      const double cs = cfg_.saturation * white_drive_; // chroma referenced to white
      const double u = cs * static_cast<double>(picture[i].u);
      const double v = cs * static_cast<double>(picture[i].v);
      gun_rgb[0] = gun(drive + kRv * v, cfg_.gamma);
      gun_rgb[1] = gun(drive + kGu * u + kGv * v, cfg_.gamma);
      gun_rgb[2] = gun(drive + kBu * u, cfg_.gamma);
    }

    // Sub-pixel horizontal position: the beam sweeps continuously, so spread the
    // deposit across the two columns it straddles. Without this, each sample dumps
    // its whole charge into one integer column and the ~1.4 samples per output
    // pixel beat into vertical stripes.
    const double xf = static_cast<double>(hbeam[i].h_phase) * static_cast<double>(cfg_.width);
    const auto x0 = static_cast<std::ptrdiff_t>(std::floor(xf));
    const double fx = xf - static_cast<double>(x0);
    const double col_w[2] = {1.0 - fx, fx};

    // Yoke shear: un-creep the vertical so the scanline is flat (see header).
    const auto yc = static_cast<double>(vbeam[i].v_phase) * static_cast<double>(cfg_.height) -
                    yoke_tilt_rows_ * static_cast<double>(hbeam[i].h_phase);

    // Gaussian beam spot, centred on the sheared sub-pixel row. The normalised
    // weights depend only on yc's fractional part, so look up the matching bin.
    const double yc_floor = std::floor(yc);
    const auto base = static_cast<std::ptrdiff_t>(yc_floor) - static_cast<std::ptrdiff_t>(splat_radius_);
    auto bin = static_cast<std::size_t>((yc - yc_floor) * static_cast<double>(kGaussBins));
    if (bin >= kGaussBins)
      bin = kGaussBins - 1;
    const double *rweights = &gauss_lut_[bin * gauss_stride_];

    // Splat into those rows × the two straddled columns; each pixel-channel keeps
    // its own lazy phosphor decay (charge brought forward to now, then the fresh
    // deposit). last_ is shared across the channels of a pixel — the three guns
    // hit it at the same instant.
    for (std::size_t k = 0; k < gauss_stride_; ++k) {
      const std::ptrdiff_t row = base + static_cast<std::ptrdiff_t>(k);
      if (row < 0 || row >= static_cast<std::ptrdiff_t>(cfg_.height))
        continue;
      const double rw = rweights[k];
      for (std::size_t cc = 0; cc < 2; ++cc) {
        const std::ptrdiff_t col = x0 + static_cast<std::ptrdiff_t>(cc);
        if (col < 0 || col >= static_cast<std::ptrdiff_t>(cfg_.width))
          continue;
        const auto pixel = static_cast<std::size_t>(row) * cfg_.width + static_cast<std::size_t>(col);
        const auto decay = decay_for(sample_index_ - last_[pixel]);
        const auto w = static_cast<float>(rw * col_w[cc]);
        // channels_ is 1 or 3; spell both out so the guns stay in registers and
        // the RGB triple isn't a variable-trip loop.
        float *cell = &bright_[pixel * channels_];
        cell[0] = cell[0] * decay + gun_rgb[0] * w;
        if (channels_ == 3) {
          cell[1] = cell[1] * decay + gun_rgb[1] * w;
          cell[2] = cell[2] * decay + gun_rgb[2] * w;
        }
        last_[pixel] = sample_index_;
      }
    }
    ++sample_index_;
  }
}

Screen::Frame Screen::snapshot() const {
  // Scale by the AGC white reference (the steady-state phosphor brightness a
  // tracked-white pixel reaches), one shared scale so hue is preserved. Contrast
  // moves the white point; cells above it clip into white, as a real tube does —
  // no per-frame statistic, so the exposure is causal and doesn't breathe.
  const double white = white_ref_ * phosphor_gain_;
  const float scale = white > 0.0 ? static_cast<float>(255.0 * cfg_.contrast / white) : 0.0f;
  std::vector<std::uint8_t> pixels(bright_.size());
  for (std::size_t pixel = 0; pixel < last_.size(); ++pixel) {
    const auto decay = decay_for(sample_index_ - last_[pixel]);
    for (std::size_t ch = 0; ch < channels_; ++ch) {
      const auto idx = pixel * channels_ + ch;
      pixels[idx] = static_cast<std::uint8_t>(std::clamp(bright_[idx] * decay * scale, 0.0f, 255.0f) + 0.5f);
    }
  }
  return Frame{.pixels = std::move(pixels), .width = cfg_.width, .height = cfg_.height, .channels = channels_};
}

// === Decoder ===

namespace {
// Sync-branch low-pass length. Modest, so the group delay (~half this) stays a
// small fraction of a line — a constant horizontal offset, not a smear.
constexpr std::size_t kSyncLpTaps = 63;

// Copy a sub-stage config and stamp the shared sample rate into it; lets
// DecoderConfig carry the sub-configs with their sample_rate_hz left at 0.
template<class C>
[[nodiscard]] C with_rate(C c, double rate) {
  c.sample_rate_hz = rate;
  return c;
}
} // namespace

Decoder::Decoder(const DecoderConfig &cfg) :
    colour_{cfg.colour}, sync_lp_{dsp::lowpass_kernel(kSyncLpTaps, cfg.sample_rate_hz, cfg.sync_lp_cutoff_hz)},
    sep_{with_rate(cfg.sep, cfg.sample_rate_hz)}, hsweep_{with_rate(cfg.hsweep, cfg.sample_rate_hz)},
    vsync_{with_rate(cfg.vsync, cfg.sample_rate_hz)}, chroma_{with_rate(cfg.chroma, cfg.sample_rate_hz)},
    screen_{ScreenConfig{.width = cfg.width,
        .height = cfg.height,
        .sample_rate_hz = cfg.sample_rate_hz,
        .persistence_fields = cfg.persistence_fields,
        .beam_sigma_rows = cfg.beam_sigma_rows,
        .gamma = cfg.gamma,
        .colour = cfg.colour,
        .saturation = cfg.saturation,
        .contrast = cfg.contrast,
        .h_blank = cfg.h_blank}} {}

void Decoder::prepare(std::size_t max_in) {
  sync_lp_.prepare(max_in);
  sep_.prepare(max_in);
  hsweep_.prepare(max_in);
  vsync_.prepare(max_in);
  chroma_.prepare(max_in);
  screen_.prepare(max_in);
  grey_pic_.reserve(max_in);
}

void Decoder::process(std::span<const float> envelope, const Screen::FieldCallback &on_field) {
  // The branch: the envelope fans to a narrow sync low-pass (whose sliced sync
  // bit feeds both timebases) and, untouched, to the picture rail. In colour the
  // chroma decoder is a third branch off the envelope (it needs the horizontal
  // rail for the burst gate); the screen joins the picture rail with the two
  // timebases. Spans stay valid because each producer is read before it runs
  // again next block.
  const auto sync_env = sync_lp_.process(envelope);
  const auto sync = sep_.process(sync_env);
  const auto hbeam = hsweep_.process(sync);
  const auto vbeam = vsync_.process(sync);
  if (colour_) {
    const auto picture = chroma_.process(envelope, hbeam);
    screen_.process(picture, hbeam, vbeam, on_field);
  }
  else {
    // Monochrome: the luma rail is the envelope straight through, no chroma.
    const auto n = envelope.size();
    const auto pic = grey_pic_.write_n(n);
    for (std::size_t i = 0; i < n; ++i)
      pic[i] = ChromaSample{.luma = envelope[i], .u = 0.0f, .v = 0.0f};
    screen_.process(grey_pic_.view(), hbeam, vbeam, on_field);
  }
}

} // namespace palindrome::video
