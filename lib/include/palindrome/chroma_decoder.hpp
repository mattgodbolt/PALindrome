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
//   delay_line — the PAL-D comb topology: a 1H delay line on the *modulated*
//                chroma, summed (-> U, V cancels) and differenced (-> V, U cancels)
//                BEFORE demodulation, exactly as the TDA3561A's external glass delay
//                line feeds its (R-Y)/(B-Y) demodulators (see docs/TDA3561A.md). The
//                delay ADAPTS to the measured line length — a convenience no real
//                glass block had, kept so the comb's structural behaviour can be
//                studied on an off-nominal source without the timing error.
//   glass      — delay_line with the real geometry: the delay is fixed at 283.5
//                subcarrier cycles (63.943 us), the ultrasonic glass block's
//                trimmed length — set by the subcarrier, NOT the line period, and
//                fixed whatever the source does. On a source off the nominal line
//                rate the comb pairs chroma displaced along the line (the SMS
//                corpus runs ~0.35 us long), so colour edges ghost and shimmer
//                with extra cross-colour — the off-spec misregistration a real
//                PAL-D set showed. The classic 1980s set, warts and all.
//   post       — demodulate first (per-line burst de-rotation), then average the
//                recovered baseband U/V across a line pair. A DSP-era convenience
//                (late-80s/90s digital line stores and beyond); robust to an
//                off-nominal source line rate that the fixed glass geometry is
//                not. This is the default.
enum class CombMode { off, post, delay_line, glass };

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
  // APC reference time constant, in lines: how slowly the crystal phase locks onto
  // the burst (kApc = 1/ref_tc_lines for the per-line EMA). 10 is the modern fast
  // default. A real set's APC was slower; pushing this up makes the reference stop
  // chasing per-line drift, which is what lets the delay_line comb's structural
  // sum/difference earn its keep (Hanover-bar suppression a fast loop hides). Out
  // of [2, 100]: below ~2 the loop tracks the ±45° swing itself, not the axis;
  // above ~100 it can't pull in an off-nominal source's per-line phase ramp.
  double ref_tc_lines = 10.0;
  // Colour killer. The ident circuit's verdict — does the burst's ±45° swing
  // sense agree with the bistable, line after line — gates the chroma, exactly
  // as the TDA3561A's ident/killer mutes the chrominance amplifier when no PAL
  // ident is found. Noise can pass an amplitude test but cannot fake a
  // consistent swing, so ident is the discriminator. The gate is a slow ramp:
  // switch-ON is deliberately delayed (the saturation-control time constant —
  // colour fades in a fraction of a second after lock), which is also what
  // makes brief noise-driven ident excursions harmless; the kill direction is
  // much faster. killer_threshold <= 0 disables the killer — no ident-based
  // muting (chroma can still come out zero when no burst is measured at all:
  // the ACC has nothing to normalise against).
  double killer_threshold = 0.4; // ident level counted as "PAL identified"
  double killer_on_tc_lines = 1500.0; // switch-on ramp (~0.1 s of lines)
  double killer_off_tc_lines = 100.0; // kill ramp (fast mute)
  // APC frequency pull. A real burst phase detector pulls the 4.43 MHz crystal
  // itself, with a spec'd catching range (TDA3561A: 500-700 Hz); a source
  // further off simply failed to lock colour (the killer drops it). The pull is
  // the loop's integral term: per pair of consecutive good lines, the drift of
  // the (swing-free) APC reference axis is a frequency-error measurement, and
  // apc_pull of it is folded into the NCO, clamped to ±apc_catch_range_hz of
  // the crystal. catch range 0 disables the pull — the NCO stays a fixed
  // crystal and the per-line rotation absorbs everything (the pre-pull
  // behaviour, whose unlimited catch range no real set had). Within range the
  // pull also removes the intra-line hue ramp a fixed NCO leaves on a source
  // off crystal frequency (the per-line rotation only corrects line averages).
  double apc_catch_range_hz = 500.0;
  double apc_pull = 0.02; // fraction of the measured per-line drift folded in per line
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
  // The killer gate hard-mutes below this level (the > 50 dB mute of a real
  // killer); diagnostics should treat a gate below it as "colour killed".
  static constexpr double kKillerSwitch = 0.1;

  explicit ChromaDecoder(const ChromaDecoderConfig &cfg);

  void prepare(std::size_t max_in);
  [[nodiscard]] std::span<const ChromaSample> process(
      std::span<const float> envelope, std::span<const BeamSample> hbeam);

  [[nodiscard]] std::size_t max_output_for(std::size_t n) const noexcept { return n; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

  // Diagnostics: the crystal frequency (Hz), the smoothed burst amplitude (the
  // ACC level), this line's burst swing in degrees (~45° once the colour is
  // locked — the ±45° -U±V swinging burst), and the killer's gate ([0, 1]:
  // 0 = chroma muted, 1 = full colour).
  [[nodiscard]] double subcarrier_hz() const noexcept { return nco_omega_ * cfg_.sample_rate_hz; }
  [[nodiscard]] double burst_amplitude() const noexcept { return burst_amp_; }
  [[nodiscard]] double burst_swing_deg() const noexcept { return swing_deg_; }
  [[nodiscard]] double killer_gain() const noexcept { return kill_gain_; }

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
  // One internally-chunked piece of process(): the three decode passes over a
  // segment that ends at a burst-gate close, so an NCO retune there reaches
  // the next segment's mix at the same sample whatever the caller's chunking.
  void decode_segment(std::span<const float> envelope, std::span<const BeamSample> hbeam, std::span<ChromaSample> out);

  ChromaDecoderConfig cfg_;
  dsp::Fir bandpass_; // isolates the chroma subcarrier from the composite
  dsp::Fir lp_u_; // post-demod low-pass on raw U
  dsp::Fir lp_v_; // post-demod low-pass on raw V
  dsp::Fir lp_luma_; // luma = notch(envelope): a 4.43 MHz chroma trap, luma stays wide

  // The crystal LO, advanced one step per sample. The APC's frequency pull
  // retunes it within ±apc_catch_range_hz of the crystal (see the config);
  // with the pull disabled it stays fixed, the original behaviour.
  double nco_omega_; // cycles/sample (× sample_rate = crystal Hz)
  std::complex<double> nco_phasor_{1.0, 0.0}; // e^{+i*2π*phase}
  std::complex<double> nco_step_; // e^{+i*2π*omega}
  std::size_t since_renorm_ = 0;
  // Frequency-pull bookkeeping: the reference axis at the previous good line,
  // usable only when that line was the immediately preceding gate close (a
  // VBI/dropout gap would alias the wrapped phase drift, so the pull skips
  // across gaps and resumes on the next consecutive pair).
  double prev_psi_axis_ = 0.0;
  bool have_prev_psi_ = false; // prev_psi_axis_ holds a real measurement (not the initial 0)
  std::size_t gate_closes_ = 0; // every finalize_line() that measured a burst
  std::size_t prev_good_close_ = 0; // gate_closes_ at the last APC-updating line

  // Per-line burst gate + the recovered rotation it sets for the line.
  std::complex<double> burst_acc_{0.0, 0.0}; // Σ(raw U, raw V) over the gate
  std::size_t burst_count_ = 0;
  bool in_gate_prev_ = false;
  double burst_amp_ = 0.0; // ACC level = √2·|apc_phasor_| (the swing-averaged burst)
  double burst_ref_ = 0.0; // slow-decay peak burst level for the colour-killer
  double psi_cos_ = 1.0; // this line's rotation R(ψ)
  double psi_sin_ = 0.0;
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
  double apc_rate_; // EMA coefficient = 1 / cfg_.ref_tc_lines (set in the ctor)

  // PAL-switch bistable + ident. parity_ toggles every line (the V-switch); the
  // V-inversion is the intrinsic 2-fold ambiguity. The ident leaky-integrates
  // whether the burst's measured swing sense agrees with the bistable's claim,
  // and flips polarity_ on persistent disagreement — a local, bounded loop, like
  // the ident/killer in a real decoder, not a whole-signal vote.
  bool parity_ = false;
  bool polarity_ = false; // which bistable phase is the V-inverted (PAL) line
  double ident_ = 0.0; // leaky agreement: < 0 means the bistable is mis-phased
  double swing_deg_ = 90.0; // this line's burst |swing|; ~45 once locked

  // The killer's gate, ramped per line toward 1 (ident above threshold) or 0.
  // Starts killed: a set powers up mono and fades colour in once identified.
  double kill_gain_ = 0.0;
  void update_killer(bool identified); // one per-line ramp step (or hold, if disabled)

  // Line-length tracking, for the 1H comb delay depth.
  std::size_t sample_index_ = 0;
  std::size_t last_line_start_ = 0;
  std::size_t line_len_; // samples in the last line (adaptive comb delay)
  std::size_t glass_len_; // the fixed glass block: 283.5 crystal cycles in samples

  // The delay line: a ring one line deep of the comb's input — the final U/V for
  // CombMode::post, the raw demod quadratures for CombMode::delay_line.
  std::vector<float> u_ring_;
  std::vector<float> v_ring_;
  std::size_t ring_cap_;
  std::size_t ring_pos_ = 0; // running write position into the comb ring

  // Scratch (reused across calls): the demodulated quadratures pass 1 produces.
  Buffer<float> mix_u_;
  Buffer<float> mix_v_;
  Buffer<ChromaSample> out_;
};

} // namespace palindrome::video
