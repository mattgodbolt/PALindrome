#include "palindrome/chroma_decoder.hpp"

#include "palindrome/phase.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <format>
#include <numbers>
#include <stdexcept>

namespace palindrome::video {

namespace {
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kIdent = 0.1; // ident leaky-integrator rate for the PAL-switch bistable
// The ultrasonic glass delay line's trimmed length, in subcarrier cycles: a
// half-integer count so the delayed subcarrier returns antiphase-aligned —
// 283.5 / 4.43361875 MHz = 63.943 us, NOT the 64 us line period.
constexpr double kGlassDelayCycles = 283.5;

// Keep the chroma band-pass top edge below Nyquist (the AirSpy's 10 MS/s only
// just spans the subcarrier, so a 5 MHz edge would otherwise overrun it).
[[nodiscard]] double clamp_high(double hi, double sample_rate_hz) noexcept {
  return std::min(hi, 0.49 * sample_rate_hz);
}

// Validated on the way into the first mem-initialiser, so a bad config throws
// before any of the four FIR kernels is built (and the odd-taps check runs
// before notch_kernel could build a mis-centred filter).
const ChromaDecoderConfig &validate(const ChromaDecoderConfig &cfg) {
  if (!(cfg.sample_rate_hz > 0.0))
    throw std::invalid_argument{"ChromaDecoder: sample_rate_hz must be positive"};
  if (!(cfg.subcarrier_hz > 0.0 && cfg.subcarrier_hz < cfg.sample_rate_hz / 2.0))
    throw std::invalid_argument{"ChromaDecoder: subcarrier_hz out of range"};
  if (!(cfg.burst_gate_lo >= 0.0 && cfg.burst_gate_lo < cfg.burst_gate_hi && cfg.burst_gate_hi < 1.0))
    throw std::invalid_argument{"ChromaDecoder: burst gate must be 0 <= lo < hi < 1"};
  // Odd taps give linear phase and an integer group delay; the luma-notch length
  // (bandpass + demod - 1) and the colour registration (group_delay_samples) both
  // assume it, so luma and chroma stay co-registered.
  if (cfg.bandpass_taps % 2 == 0 || cfg.demod_lp_taps % 2 == 0)
    throw std::invalid_argument{"ChromaDecoder: bandpass_taps and demod_lp_taps must be odd"};
  if (!(cfg.ref_tc_lines >= 2.0 && cfg.ref_tc_lines <= 100.0))
    throw std::invalid_argument{
        std::format("ChromaDecoder: ref_tc_lines must be in [2, 100], got {}", cfg.ref_tc_lines)};
  if (!(cfg.killer_threshold < 1.0))
    throw std::invalid_argument{std::format(
        "ChromaDecoder: killer_threshold must be < 1 (ident never exceeds 1), got {}", cfg.killer_threshold)};
  if (!(cfg.killer_on_tc_lines >= 1.0 && cfg.killer_off_tc_lines >= 1.0))
    throw std::invalid_argument{
        std::format("ChromaDecoder: killer ramp time constants must be >= 1 line, got on {} / off {}",
            cfg.killer_on_tc_lines, cfg.killer_off_tc_lines)};
  if (!(cfg.apc_catch_range_hz >= 0.0))
    throw std::invalid_argument{
        std::format("ChromaDecoder: apc_catch_range_hz must be >= 0, got {}", cfg.apc_catch_range_hz)};
  if (!(cfg.apc_pull > 0.0 && cfg.apc_pull <= 1.0))
    throw std::invalid_argument{std::format("ChromaDecoder: apc_pull must be in (0, 1], got {}", cfg.apc_pull)};
  return cfg;
}
} // namespace

ChromaDecoder::ChromaDecoder(const ChromaDecoderConfig &cfg) :
    cfg_{validate(cfg)}, bandpass_{dsp::bandpass_kernel(cfg.bandpass_taps, cfg.sample_rate_hz, cfg.band_lo_hz,
                             clamp_high(cfg.band_hi_hz, cfg.sample_rate_hz))},
    lp_u_{dsp::lowpass_kernel(cfg.demod_lp_taps, cfg.sample_rate_hz, cfg.uv_bandwidth_hz)},
    lp_v_{dsp::lowpass_kernel(cfg.demod_lp_taps, cfg.sample_rate_hz, cfg.uv_bandwidth_hz)},
    // Luma notch length = band-pass + demod low-pass lengths, so its group delay
    // equals the chroma path's and the two rails stay aligned at the screen.
    lp_luma_{dsp::notch_kernel(cfg.bandpass_taps + cfg.demod_lp_taps - 1, cfg.sample_rate_hz,
        cfg.subcarrier_hz - cfg.luma_notch_half_hz,
        clamp_high(cfg.subcarrier_hz + cfg.luma_notch_half_hz, cfg.sample_rate_hz))} {
  apc_rate_ = 1.0 / cfg_.ref_tc_lines;
  // killer_threshold <= 0 disables the killer: the gate is pinned open rather
  // than ramped (update_killer no-ops), the pre-killer behaviour.
  if (cfg_.killer_threshold <= 0.0)
    kill_gain_ = 1.0;
  nco_omega_ = cfg_.subcarrier_hz / cfg_.sample_rate_hz;
  nco_step_ = std::polar(1.0, kTwoPi * nco_omega_);
  // The comb delay is one line; size the ring for the longest line we expect.
  const double nominal_line = cfg_.sample_rate_hz / kNominalLineHz;
  line_len_ = static_cast<std::size_t>(std::lround(nominal_line));
  ring_cap_ = static_cast<std::size_t>(nominal_line * 1.5) + 2;
  u_ring_.assign(ring_cap_, 0.0f);
  v_ring_.assign(ring_cap_, 0.0f);
  // The glass block: trimmed to exactly 283.5 cycles of the crystal (63.943 us
  // for PAL) — a half-integer subcarrier count, deliberately NOT the line
  // period, and fixed for the life of the set whatever the source does.
  glass_len_ = static_cast<std::size_t>(std::lround(kGlassDelayCycles * cfg_.sample_rate_hz / cfg_.subcarrier_hz));
  if (cfg_.comb_mode == CombMode::glass && glass_len_ >= ring_cap_)
    throw std::invalid_argument{"ChromaDecoder: glass delay exceeds the comb ring (subcarrier far below nominal?)"};
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

void ChromaDecoder::update_killer(bool identified) {
  if (cfg_.killer_threshold <= 0.0)
    return; // disabled: the gate stays pinned open
  // One ramp step per measured line. Switch-on is the slow direction (colour
  // fades in well after lock — and brief noise-driven ident excursions can't
  // meaningfully open the gate); the kill direction mutes much faster.
  const double rate = identified ? 1.0 / cfg_.killer_on_tc_lines : 1.0 / cfg_.killer_off_tc_lines;
  kill_gain_ += rate * ((identified ? 1.0 : 0.0) - kill_gain_);
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
  // last good line's rotation across the gap. The 0.9995/line decay forgets the
  // peak over ~2000 lines (~a tenth of a second), so after a genuine burst loss
  // noise lines start passing the 0.3 gate within that window and their random
  // swing kills the colour; 0.3 sits far below any real line-to-line burst
  // variation, so VBI lines are the only thing it rejects in normal running.
  burst_ref_ = std::max(burst_ref_ * 0.9995, mag);
  ++gate_closes_;
  if (mag < 0.3 * burst_ref_)
    return;
  const double phi = std::atan2(bs, bc);

  // APC: a slow EMA of the burst phasor locks the reference onto the -U axis. The
  // ±45° swing alternates line to line, so it averages out and the EMA tracks the
  // steady LO-vs-subcarrier offset (and any slow source drift), like a real APC.
  apc_phasor_ = (1.0 - apc_rate_) * apc_phasor_ + apc_rate_ * std::complex<double>{bc, bs};
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

  // The killer's verdict for this line: a sustained, bistable-consistent swing
  // sense is something noise can't fake — amplitude alone could. Lines skipped
  // above (VBI, dropouts) HOLD the gate instead: the vertical interval must not
  // drain a locked killer, and after a genuine burst loss the decaying
  // burst_ref_ starts letting noise lines through here within a couple of
  // thousand lines, whose random swing then mutes the chroma.
  update_killer(ident_ >= cfg_.killer_threshold);

  // APC frequency pull: across a consecutive pair of good lines, the drift of
  // the swing-free reference axis measures the source-vs-NCO frequency error
  // (radians per line); fold apc_pull of it into the NCO, clamped to the
  // crystal's catching range. The clamp is the authentic failure mode: a source
  // beyond it leaves a residual the reference can't track, the burst
  // measurements smear, and the ident/killer drop the colour — where the
  // per-line rotation alone would have locked anything. Gaps (VBI, dropouts)
  // skip the measurement: a wrapped angle over an unknown number of lines is
  // ambiguous.
  if (cfg_.apc_catch_range_hz > 0.0) {
    if (have_prev_psi_ && gate_closes_ == prev_good_close_ + 1) {
      // The demod is (cos, sin) against the NCO, so the measured axis rotates
      // at MINUS the source-vs-NCO frequency error: a positive drift means the
      // NCO is fast. Hence the subtraction.
      const double dpsi = dsp::wrap_angle(psi_axis - prev_psi_axis_);
      const double domega = dpsi / (kTwoPi * cfg_.sample_rate_hz / kNominalLineHz); // cycles/sample
      const double crystal = cfg_.subcarrier_hz / cfg_.sample_rate_hz;
      const double range = cfg_.apc_catch_range_hz / cfg_.sample_rate_hz;
      nco_omega_ = std::clamp(nco_omega_ - cfg_.apc_pull * domega, crystal - range, crystal + range);
      nco_step_ = std::polar(1.0, kTwoPi * nco_omega_);
    }
    prev_psi_axis_ = psi_axis;
    have_prev_psi_ = true;
    prev_good_close_ = gate_closes_;
  }

  // De-rotate by π - psi_axis so the -U axis lands on the negative-real axis;
  // U = -Re, V = Im (switch-corrected by v_flip_) then follow in process().
  const double psi = std::numbers::pi - psi_axis;
  psi_cos_ = std::cos(psi);
  psi_sin_ = std::sin(psi);
}

std::span<const ChromaSample> ChromaDecoder::process(
    std::span<const float> envelope, std::span<const BeamSample> hbeam) {
  // Both rails are per-sample views of one block, so a length mismatch can only
  // be a wiring bug - fail loudly rather than silently decode a short line.
  const std::size_t n = envelope.size();
  if (hbeam.size() != n) [[unlikely]]
    throw std::invalid_argument{"ChromaDecoder: envelope and hbeam rails must be the same length"};
  const auto out = out_.write_n(n);

  // The APC frequency pull feeds BACK into pass 1's mix, so a retune must take
  // effect at the same sample whatever the caller's chunking — otherwise the
  // loop's delay would be the block size and the result chunk-dependent. Split
  // the block at each burst-gate close (the sample where finalize_line may
  // retune the NCO ends its segment), and decode segment by segment: the FIRs
  // and recurrences all stream, so the only effect is that each segment's mix
  // runs at the frequency set by the previous gate close. With the pull
  // disabled nothing feeds back and the whole block is one segment.
  std::size_t start = 0;
  while (start < n) {
    std::size_t end = n;
    if (cfg_.apc_catch_range_hz > 0.0) {
      // Replay the gate state machine (reads only hbeam) to find the first
      // close at or after `start`; pass 3 will then walk the same transitions.
      bool gate_prev = in_gate_prev_;
      for (std::size_t k = start; k < n; ++k) {
        if (hbeam[k].line_start)
          gate_prev = false;
        const float hp = hbeam[k].h_phase;
        const bool in_gate =
            hp >= static_cast<float>(cfg_.burst_gate_lo) && hp < static_cast<float>(cfg_.burst_gate_hi);
        const bool closes = !in_gate && gate_prev;
        gate_prev = in_gate;
        if (closes) {
          end = k + 1;
          break;
        }
      }
    }
    decode_segment(
        envelope.subspan(start, end - start), hbeam.subspan(start, end - start), out.subspan(start, end - start));
    start = end;
  }
  return out_.view();
}

void ChromaDecoder::decode_segment(
    std::span<const float> envelope, std::span<const BeamSample> hbeam, std::span<ChromaSample> out) {
  const std::size_t n = envelope.size();

  // Pass 1: isolate the chroma subcarrier and synchronously demodulate it
  // against the crystal LO. nco_phasor_ is e^{+i2πθ}, so (c·cosθ, c·sinθ) are
  // the raw U and V quadratures (matching cos/sin demodulation).
  const auto chroma = bandpass_.process(envelope.first(n));
  const auto mu = mix_u_.write_n(n);
  const auto mv = mix_v_.write_n(n);
  for (std::size_t k = 0; k < n; ++k) {
    // Snapshot the phasor down to float for the per-sample mix; it still advances
    // in double (drift), but the product only needs float (see the precision rule
    // in CLAUDE.md), as the ComplexAmEnvelope mix already does.
    mu[k] = chroma[k] * nco_phasor_.real_f();
    mv[k] = chroma[k] * nco_phasor_.imag_f();
    nco_phasor_.advance(nco_step_);
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

    // The comb's read index, one delay back: the measured line for the
    // adaptive modes, the fixed glass length for CombMode::glass. ring_pos_ is
    // kept wrapped in [0, ring_cap_) so it's a conditional subtract, not a
    // per-sample modulo; the delay < ring_cap_, so it wraps at most once.
    const std::size_t delay = cfg_.comb_mode == CombMode::glass ? glass_len_ : line_len_;
    const std::size_t wi = ring_pos_;
    const std::size_t ri = wi >= delay ? wi - delay : wi + ring_cap_ - delay;
    // ACC normalisation, gated by the killer. The gate is a SWITCH with a
    // ramped opening, not a proportional fader: below the switch point the
    // chroma is fully muted (the TDA3561A kills > 50 dB — and the unbounded
    // ACC 1/burst would otherwise amplify any leakage on a noise "burst"),
    // above it the slow ramp plays out as the saturation fading in.
    const double gate = kill_gain_ >= kKillerSwitch ? kill_gain_ : 0.0;
    const double scale = burst_amp_ > 1e-9 ? gate / burst_amp_ : 0.0;

    float u = 0.0f;
    float v = 0.0f;
    switch (cfg_.comb_mode) {
      case CombMode::glass: // the same PAL-D sum/difference, at the fixed glass depth (ri above)
      case CombMode::delay_line: {
        // Period-correct PAL-D: the 1H comb sits on the chroma BEFORE the per-line
        // de-rotation (a delay line on the modulated chroma; the continuous NCO
        // demod has already cancelled the inter-line carrier advance, so adjacent
        // lines' raw quadratures are aligned to within the slow crystal-vs-source
        // drift). U comes from the SUM (the opposite-switch V cancels) and V from
        // the DIFFERENCE (U cancels), each on its own demod axis — the TDA3561A's
        // two delay-line demodulators. Unlike `post`, the delayed line is NOT
        // individually phase-corrected, so a source off the nominal line rate
        // leaves the authentic residual. The ring holds the raw quadratures.
        const double id = u_ring_[ri];
        const double qd = v_ring_[ri];
        u_ring_[wi] = static_cast<float>(i);
        v_ring_[wi] = static_cast<float>(q);
        if (++ring_pos_ == ring_cap_)
          ring_pos_ = 0;
        const double su = i + id; // sum: U-bearing (V cancels)
        const double sq = q + qd;
        const double du = i - id; // difference: V-bearing (U cancels)
        const double dq = q - qd;
        u = static_cast<float>(0.5 * (psi_cos_ * su - psi_sin_ * sq) * scale);
        v = static_cast<float>(0.5 * (psi_sin_ * du + psi_cos_ * dq) * v_flip_ * scale);
        break;
      }
      case CombMode::off:
        // R(ψ): U = cosψ·rawU - sinψ·rawV; V_mod = sinψ·rawU + cosψ·rawV. V flips
        // on PAL-style lines to recover transmitted V.
        u = static_cast<float>((psi_cos_ * i - psi_sin_ * q) * scale);
        v = static_cast<float>((psi_sin_ * i + psi_cos_ * q) * v_flip_ * scale);
        break;
      case CombMode::post: {
        // Same R(ψ) as `off`, then the DSP-era convenience: average the recovered
        // baseband U/V across a line pair (both lines already de-rotated and
        // V-un-flipped, so a straight mean).
        u = static_cast<float>((psi_cos_ * i - psi_sin_ * q) * scale);
        v = static_cast<float>((psi_sin_ * i + psi_cos_ * q) * v_flip_ * scale);
        const float u_avg = 0.5f * (u + u_ring_[ri]);
        const float v_avg = 0.5f * (v + v_ring_[ri]);
        u_ring_[wi] = u;
        v_ring_[wi] = v;
        u = u_avg;
        v = v_avg;
        if (++ring_pos_ == ring_cap_)
          ring_pos_ = 0;
        break;
      }
    }
    out[k] = ChromaSample{.luma = luma[k], .u = u, .v = v};
  }
}

} // namespace palindrome::video
