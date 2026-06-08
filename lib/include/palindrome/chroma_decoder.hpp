#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/fir.hpp"
#include "palindrome/video_types.hpp"

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace palindrome::video {

// Where the 1H line-pair comb sits, spanning the eras of PAL colour hardware:
//   off        — no comb (a "PAL-S" simple set: phase errors show as Hanover bars,
//                the eye averages adjacent lines). The cheapest 1970s/early-80s sets.
//   delay_line — the period-correct PAL-D comb: a 1H delay line on the *modulated*
//                chroma, summed (-> U, V cancels) and differenced (-> V, U cancels)
//                BEFORE demodulation, exactly as the TDA3561A's external glass delay
//                line feeds its (R-Y)/(B-Y) demodulators (see docs/TDA3561A.md). The
//                classic 1980s set.
//   post       — demodulate first (per-line burst de-rotation), then average the
//                recovered baseband U/V across a line pair. A DSP-era convenience
//                (late-80s/90s digital line stores and beyond); robust to an
//                off-nominal source line rate that the fixed delay-line geometry is
//                not. This is the default.
enum class CombMode { off, post, delay_line };

struct ChromaDecoderConfig {
  double sample_rate_hz;
  // The subcarrier crystal: a fixed 4.43361875 MHz reference, exactly as a real
  // PAL set's colour crystal. It is NOT measured from the spectrum (the strong
  // peak in 4–5 MHz is chroma SIDEBAND energy, not the burst); the per-line burst
  // rotation below absorbs the source's small offset from this nominal, just as a
  // TV's APC pulls its crystal onto the burst.
  double subcarrier_hz = 4.43361875e6;
  // Chroma band-pass edges. Upper edge 5.0 MHz (not 5.5) keeps the FM sound
  // subcarrier at vision+5.5 MHz out, so it can't beat into the U/V passband.
  double band_lo_hz = 3.5e6;
  double band_hi_hz = 5.0e6;
  double uv_bandwidth_hz = 1.3e6; // post-demod U/V low-pass corner
  // Luma chroma-trap half-width around fsc. Narrower keeps more luma detail (the
  // notch removes ±this around 4.43 MHz) at the cost of a little more dot-crawl.
  double luma_notch_half_hz = 0.6e6;
  std::size_t bandpass_taps = 81;
  std::size_t demod_lp_taps = 41;
  // Burst gate as an h_phase window (0 = line-sync leading edge): the back porch
  // where the burst sits, after the chroma path's group delay. It is rate-
  // dependent — the same delay is a larger fraction of a shorter line — so a
  // 10 MS/s capture wants a slightly later window (~0.16) than a 16 MS/s one.
  double burst_gate_lo = 0.11;
  double burst_gate_hi = 0.14;
  CombMode comb_mode = CombMode::post; // where the 1H comb sits (see CombMode)
};

// The colour channel, a PAL-D delay-line decoder. Off the same composite envelope
// and in parallel with luma/sync: a band-pass lifts the 4.43 MHz chroma; a fixed
// crystal LO synchronously demodulates it to raw (U, V); the back-porch burst is
// measured per line to recover that line's LO-vs-subcarrier phase, and a class-
// aware rotation (the PAL ± line alternation) turns raw U/V into transmitted U/V;
// a 1H comb cancels residual differential phase error. Needs the horizontal rail
// for the burst gate, so it sits after HorizontalSweep. Streaming and
// block-invariant like every other stage.
class ChromaDecoder {
public:
  explicit ChromaDecoder(const ChromaDecoderConfig &cfg);

  void prepare(std::size_t max_in);
  [[nodiscard]] std::span<const ChromaSample> process(
      std::span<const float> envelope, std::span<const BeamSample> hbeam);

  [[nodiscard]] std::size_t max_output_for(std::size_t n) const noexcept { return n; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

  // Diagnostics: the crystal frequency (Hz), the smoothed burst amplitude (the
  // ACC level), and this line's burst swing in degrees (~45° once the colour is
  // locked — the ±45° -U±V swinging burst).
  [[nodiscard]] double subcarrier_hz() const noexcept { return nco_omega_ * cfg_.sample_rate_hz; }
  [[nodiscard]] double burst_amplitude() const noexcept { return burst_amp_; }
  [[nodiscard]] double burst_swing_deg() const noexcept { return swing_deg_; }

  // Group delay of the chroma/luma rails in samples. The luma notch is built with
  // (bandpass_taps + demod_lp_taps - 1) taps, so its delay — and the cascaded
  // band-pass + demod low-pass on the chroma side — is (taps - 1)/2. The picture
  // comes out this many samples behind the sync-locked timing rails, so the
  // renderer shifts it back to register colour with mono — the job of a real
  // set's luminance delay line. (Taps are required odd, so this is exact.)
  [[nodiscard]] std::size_t group_delay_samples() const noexcept {
    return (cfg_.bandpass_taps + cfg_.demod_lp_taps - 2) / 2;
  }

private:
  void finalize_line(); // per-line burst measurement + class assignment at gate close

  ChromaDecoderConfig cfg_;
  dsp::Fir bandpass_; // isolates the chroma subcarrier from the composite
  dsp::Fir lp_u_, lp_v_; // post-demod low-pass on the two quadratures (raw U, V)
  dsp::Fir lp_luma_; // luma = notch(envelope): a 4.43 MHz chroma trap, luma stays wide

  // The fixed crystal LO, advanced one step per sample (never retuned).
  double nco_omega_; // cycles/sample (× sample_rate = crystal Hz)
  std::complex<double> nco_phasor_{1.0, 0.0}; // e^{+i*2π*phase}
  std::complex<double> nco_step_; // e^{+i*2π*omega}
  std::size_t since_renorm_ = 0;

  // Per-line burst gate + the recovered rotation it sets for the line.
  std::complex<double> burst_acc_{0.0, 0.0}; // Σ(raw U, raw V) over the gate
  std::size_t burst_count_ = 0;
  bool in_gate_prev_ = false;
  double burst_amp_ = 0.0; // ACC level = √2·|apc_phasor_| (the swing-averaged burst)
  double burst_ref_ = 0.0; // slow-decay peak burst level for the colour-killer
  double psi_cos_ = 1.0, psi_sin_ = 0.0; // this line's rotation R(ψ)
  double v_flip_ = 1.0; // +1 on NTSC-style lines, -1 on PAL-style (V inversion)

  // Automatic phase control: a slow complex EMA of the back-porch burst locks the
  // reference onto the -U axis. The burst swings ±45° about that axis line to line
  // (the -U±V swinging burst), so the swing averages out of this loop, exactly as
  // a real set's APC averages it to lock its crystal phase. The rotation derives
  // from this axis (held in psi_cos_/sin_); its magnitude is the ACC (burst_amp_).
  // This is the "Long-Tc" reference path of BBC ETD info sheet 21W (PAL Coding
  // Revision), docs/ETD_Info_Sheet_21W_PAL_Coding_Revision.pdf; the parity_/ident_
  // bistable below is its "Short-Tc" Steer, the half-line V-switch sense.
  std::complex<double> apc_phasor_{0.0, 0.0};

  // PAL-switch bistable + ident. parity_ toggles every line (the V-switch); the
  // V-inversion is the intrinsic 2-fold ambiguity. The ident leaky-integrates
  // whether the burst's measured swing sense agrees with the bistable's claim,
  // and flips polarity_ on persistent disagreement — a local, bounded loop, like
  // the ident/killer in a real decoder, not a whole-signal vote.
  bool parity_ = false;
  bool polarity_ = false; // which bistable phase is the V-inverted (PAL) line
  double ident_ = 0.0; // leaky agreement: < 0 means the bistable is mis-phased
  double swing_deg_ = 90.0; // this line's burst |swing|; ~45 once locked

  // Line-length tracking, for the 1H comb delay depth.
  std::size_t sample_index_ = 0;
  std::size_t last_line_start_ = 0;
  std::size_t line_len_; // samples in the last line (comb delay)

  // The delay line: a ring one line deep of the comb's input — the final U/V for
  // CombMode::post, the raw demod quadratures for CombMode::delay_line.
  std::vector<float> u_ring_, v_ring_;
  std::size_t ring_cap_;
  std::size_t ring_pos_ = 0; // running write position into the comb ring

  // Scratch (reused across calls): the demodulated quadratures pass 1 produces.
  Buffer<float> mix_u_, mix_v_;
  Buffer<ChromaSample> out_;
};

} // namespace palindrome::video
