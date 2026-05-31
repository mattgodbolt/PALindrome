#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/fir.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// Video reconstruction as a branching, streaming filter graph, modelled on the
// analog TV signal path. The demodulated composite fans out the way it does in
// a real set — to the sync separator AND to the picture tube — and the sync bit
// fans again to the two timebases, all rejoining at the phosphor screen:
//
//   envelope --+--> [SyncSeparator] ---+--> [HorizontalSweep] --+
//              |                       |                        |
//              |                       +--> [VerticalSync] -----+--> [Screen]
//              +----------- picture rail (envelope) ------------+
//
// Each filter stage is a streaming block (prepare / process(span)->span) with
// state carried across calls, so output is independent of how the input is
// chunked — which is what lets a driver pump fixed-size blocks through the whole
// graph with bounded memory (the target being live RF, not finite files). The
// screen is the join SINK: the picture rail plus the two timing rails in, a
// phosphor framebuffer out. Because the branches don't decimate (decimation
// happens upstream in the vision chain, before the fan-out), all rails stay
// sample-aligned and the joins are plain zips.
namespace palindrome::video {

// === Sync separator ===

// Output of the sync separator: just the sliced one-bit sync signal — true
// while the envelope sits in the sync region (above the slice level, since
// vision is negatively modulated so sync tips are the envelope peaks). True
// during EVERY sync pulse (line sync and the vertical-interval broad /
// equalising pulses alike); telling them apart is downstream's job. It is a
// struct, not a bare bool, so the sandcastle-style gating levels (burst gate,
// clamp, blanking) can join it later without changing the stage signature.
struct SyncSample {
  bool sync = false;
};

struct SyncSeparatorConfig {
  double sample_rate_hz;
  // Slice level as a fraction of the tracked floor(white) -> peak(sync-tip)
  // range. PAL puts the sync tip at 100% of the carrier and black at ~75%, so
  // a slice around 0.85 sits safely inside the sync region.
  double sync_level = 0.85;
};

// Slices the composite envelope into a clean one-bit sync signal. Tracks a
// running peak (sync tip) and floor (active-video white) so the slice point
// follows the recording's amplitude, with hysteresis so chroma ripple on a
// transition doesn't chatter the output. No timing loop here — it just slices.
class SyncSeparator {
public:
  explicit SyncSeparator(const SyncSeparatorConfig &cfg);

  void prepare(std::size_t max_in);
  [[nodiscard]] std::span<const SyncSample> process(std::span<const float> envelope);

  [[nodiscard]] std::size_t max_output_for(std::size_t n) const noexcept { return n; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

private:
  SyncSeparatorConfig cfg_;
  double peak_ = 0.0; // running sync-tip level
  double floor_ = 0.0; // running white level
  bool sync_ = false; // current sliced state (hysteresis)
  bool seeded_ = false;
  Buffer<SyncSample> out_;
};

// === Horizontal sweep (AFC + flywheel) ===

// Output of the horizontal sweep: the beam's horizontal position as a phase in
// [0, 1) (0 at the locked line start), and line_start true on the single sample
// where the sweep locks a new line. The envelope is NOT here — it rides the
// picture branch; the renderer joins the two rails by index.
struct BeamSample {
  float h_phase = 0.0f;
  bool line_start = false;
};

struct HorizontalSweepConfig {
  double sample_rate_hz;
  double nominal_line_hz = 15625.0;
  // Pulse-width window the AFC accepts, as fractions of a line. Line sync is
  // ~4.7 us (~0.073 line); equalising pulses ~2.35 us (~0.037); broad pulses
  // ~27 us (~0.43). The (0.05, 0.15) window passes line sync and rejects both
  // vertical-interval pulse kinds — the period-correct pulse-width slicer.
  double min_pulse_fraction = 0.05;
  double max_pulse_fraction = 0.15;
  // Horizontal hold: reject a sync edge arriving sooner than this fraction of
  // a line after the last accepted one (chroma-ripple retriggers, etc.). The
  // flywheel free-runs across the gap, which is the whole point of a flywheel.
  double min_line_fraction = 0.85;
  // AFC PI loop. kp snaps phase to the sync anchor; ki tracks the recording's
  // true line rate. omega is anti-windup clamped to +/- omega_clamp of nominal
  // so a sustained biased error during acquisition can't drive it to zero
  // (which would invert the lockout-gap formula min_line_fraction / omega).
  double pll_kp = 1.0;
  double pll_ki = 1.0e-5;
  double omega_clamp = 0.2;
};

// The horizontal timebase: a free-running phase oscillator (the flywheel)
// pulled into lock by a PI loop on the line-sync edges the separator found.
// Pulse-width and horizontal-hold gating keep vertical-interval pulses and
// chroma retriggers out of the loop; between accepted edges the oscillator
// coasts, so dropouts and rejected pulses don't disturb the sweep.
class HorizontalSweep {
public:
  explicit HorizontalSweep(const HorizontalSweepConfig &cfg);

  void prepare(std::size_t max_in);
  [[nodiscard]] std::span<const BeamSample> process(std::span<const SyncSample> in);

  [[nodiscard]] std::size_t max_output_for(std::size_t n) const noexcept { return n; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

  // Diagnostics: accepted edges drove the AFC; rejected edges were gated out;
  // omega is the current oscillator rate in cycles/sample (× input rate = the
  // locked line rate in Hz).
  [[nodiscard]] std::size_t accepted_edges() const noexcept { return accepted_; }
  [[nodiscard]] std::size_t rejected_edges() const noexcept { return rejected_; }
  [[nodiscard]] double omega() const noexcept { return omega_; }

private:
  HorizontalSweepConfig cfg_;
  double omega_; // cycles/sample, == nominal_line_hz / sample_rate_hz at construction
  double phase_ = 0.0; // [0, 1), advances by omega_ each sample
  double leading_edge_phase_ = 0.0; // phase_ captured at the current pulse's leading edge
  bool prev_sync_ = false; // sync bit at the previous sample, for edge detection
  std::size_t sample_index_ = 0; // total samples across all process() calls
  std::size_t leading_edge_sample_ = 0; // sample_index_ at the current pulse's leading edge
  std::size_t last_accepted_sample_ = 0; // sample_index_ of the last accepted line-sync edge
  bool have_accepted_ = false; // any edge accepted yet? (the hold gate has no prior edge before the first)
  std::size_t accepted_ = 0;
  std::size_t rejected_ = 0;
  Buffer<BeamSample> out_;
};

// === Vertical sync ===

// Output of the vertical sync: the beam's vertical position as a phase in
// [0, 1) (0 at the locked field start), and field_start true on the single
// sample where a new field is locked. A second timing rail off the same sync
// bit the separator produces — it joins the horizontal rail at the renderer.
struct VSample {
  float v_phase = 0.0f;
  bool field_start = false;
};

struct VerticalSyncConfig {
  double sample_rate_hz;
  double nominal_field_hz = 50.0;
  double nominal_line_hz = 15625.0; // sets the integrator time constant in lines
  // The detector low-passes the sync bit toward its duty cycle: ~7% on normal
  // lines (short sync), ~84% during the broad-pulse train. Slicing at vsync_level
  // detects the vertical interval. The time constant averages over roughly
  // integrator_tc_lines of a line so it rises within the broad-pulse train but
  // ignores individual line-sync pulses.
  double integrator_tc_lines = 0.5;
  double vsync_level = 0.4;
  // Vertical flywheel: PI loop + hold gate, mirroring the horizontal sweep but
  // per field. min_field_fraction rejects a second crossing within one field.
  // ki is scaled to the field oscillator's omega (~3e-6 cycles/sample), ~300x
  // smaller than the horizontal sweep's, so a single field's phase error nudges
  // omega by a few percent rather than slamming it into the clamp.
  double min_field_fraction = 0.7;
  double pll_kp = 1.0;
  double pll_ki = 2.0e-8;
  double omega_clamp = 0.2;
};

// The vertical timebase: an integrator turns the sync bit into a vertical-sync
// detection (the broad-pulse train charges it past threshold; line sync never
// does), and a free-running field oscillator (flywheel) is pulled into lock by
// a PI loop on those detections. Same shape as the horizontal sweep, one field
// at a time.
class VerticalSync {
public:
  explicit VerticalSync(const VerticalSyncConfig &cfg);

  void prepare(std::size_t max_in);
  [[nodiscard]] std::span<const VSample> process(std::span<const SyncSample> in);

  [[nodiscard]] std::size_t max_output_for(std::size_t n) const noexcept { return n; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

  [[nodiscard]] std::size_t detected_fields() const noexcept { return fields_; }
  [[nodiscard]] double omega() const noexcept { return omega_; }

private:
  VerticalSyncConfig cfg_;
  double alpha_; // integrator coefficient (per-sample low-pass toward sync duty)
  double omega_; // field oscillator rate, cycles/sample
  double integ_ = 0.0; // leaky-integrated sync bit
  double v_phase_ = 0.0; // [0, 1), advances by omega_ each sample
  bool in_vsync_ = false; // hysteresis state for the integrator slice
  std::size_t sample_index_ = 0;
  std::size_t last_field_sample_ = 0;
  bool have_field_ = false;
  std::size_t fields_ = 0;
  Buffer<VSample> out_;
};

// === Chroma decoder (the colour channel) ===

// One decoded sample on the colour rail: the chroma-notched luma (still in
// envelope units — negatively modulated, so a *lower* value is whiter) plus the
// two colour-difference components recovered by synchronous demodulation. u and
// v are zero on a monochrome line (no burst lock) and on the grey rail.
struct ChromaSample {
  float luma = 0.0f;
  float u = 0.0f;
  float v = 0.0f;
};

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
  std::size_t bandpass_taps = 81;
  std::size_t demod_lp_taps = 41;
  // Burst gate as an h_phase window (0 = line-sync leading edge): the back porch
  // where the burst sits, after the chroma path's group delay. It is rate-
  // dependent — the same delay is a larger fraction of a shorter line — so a
  // 10 MS/s capture wants a slightly later window (~0.16) than a 16 MS/s one.
  double burst_gate_lo = 0.11;
  double burst_gate_hi = 0.14;
  bool delay_line = true; // PAL-D line-pair (1H) comb on U/V
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

  // Diagnostics: the crystal frequency (Hz), a smoothed burst amplitude (the ACC
  // / colour-killer signal), and the running PAL-switch self-consistency in
  // degrees (≈0 when the V-inversion parity is confidently resolved).
  [[nodiscard]] double subcarrier_hz() const noexcept { return nco_omega_ * cfg_.sample_rate_hz; }
  [[nodiscard]] double burst_amplitude() const noexcept { return burst_amp_; }
  [[nodiscard]] double parity_consistency_deg() const noexcept { return consistency_deg_; }

private:
  void finalize_line(); // per-line burst measurement + class assignment at gate close

  ChromaDecoderConfig cfg_;
  dsp::Fir bandpass_; // isolates the chroma subcarrier from the composite
  dsp::Fir lp_i_, lp_q_; // post-demod low-pass on the two quadratures (raw U, V)
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
  double burst_amp_ = 0.0; // smoothed |burst| (ACC)
  double burst_ref_ = 0.0; // slow-decay peak burst level for the colour-killer
  double psi_cos_ = 1.0, psi_sin_ = 0.0; // this line's rotation R(ψ)
  double v_flip_ = 1.0; // +1 on NTSC-style lines, −1 on PAL-style (V inversion)

  // Automatic phase control: a slow complex EMA of the back-porch burst locks the
  // reference onto the −U axis. The ±135° (−U±V) swing alternates every line, so
  // it averages out of this loop, exactly as a real set's APC averages it to lock
  // its crystal phase. The rotation derives from this axis (held in psi_cos_/sin_).
  std::complex<double> apc_phasor_{0.0, 0.0};

  // PAL-switch bistable + ident. parity_ toggles every line (the V-switch); the
  // V-inversion is the intrinsic 2-fold ambiguity. The ident leaky-integrates
  // whether the burst's measured swing sense agrees with the bistable's claim,
  // and flips polarity_ on persistent disagreement — a local, bounded loop, like
  // the ident/killer in a real decoder, not a whole-signal vote.
  bool parity_ = false;
  bool polarity_ = false; // which bistable phase is the V-inverted (PAL) line
  double ident_ = 0.0; // leaky agreement: < 0 means the bistable is mis-phased
  double consistency_deg_ = 90.0; // this line's |swing|; ~45 once locked

  // Line-length tracking, for the 1H comb delay depth.
  std::size_t sample_index_ = 0;
  std::size_t last_line_start_ = 0;
  std::size_t line_len_; // samples in the last line (comb delay)

  // The delay line: a ring of the final U/V, one line deep, for the comb.
  std::vector<float> u_ring_, v_ring_;
  std::size_t ring_cap_;
  std::size_t ring_pos_ = 0; // running write position into the comb ring

  // Scratch (reused across calls): the demodulated quadratures pass 1 produces.
  Buffer<float> mix_i_, mix_q_;
  Buffer<ChromaSample> out_;
};

// === Screen (phosphor, the 3-way join sink) ===

struct ScreenConfig {
  std::size_t width;
  std::size_t height;
  double sample_rate_hz;
  double field_hz = 50.0;
  double nominal_line_hz = 15625.0; // sets the deflection-yoke shear (see below)
  // Phosphor persistence as a multiple of the field period. The beam deposits
  // brightness where it lands; each pixel then decays exponentially with this
  // time constant. ~1 field means roughly the last two fields survive — so an
  // interlaced frame builds up and older content (and the startup junk) fades.
  double persistence_fields = 1.2;
  // Beam-spot size: each sample is splatted vertically with a Gaussian of this
  // standard deviation, in output rows. It fills the gaps between scanlines (the
  // focus) and is the hook a wider kernel turns into bloom later. 0 => a single
  // pixel (no splat).
  double beam_sigma_rows = 0.8;
  // Electron-gun gamma: beam current rises as drive^gamma. The source pre-distorts
  // its video (~1/2.2) expecting a CRT to undo it, so ~2.2-2.8 restores the
  // midtones/contrast; 1.0 is linear (no gamma). Readout normalisation makes the
  // absolute drive scale irrelevant, so no white reference is needed.
  double gamma = 1.0;
  // Colour: a three-phosphor (RGB) triad driven by three guns matrixed from
  // Y/U/V, vs a single grey phosphor (u/v ignored). saturation scales the
  // colour-difference signals into the matrix before the per-gun gamma.
  bool colour = false;
  double saturation = 1.0;
  // Contrast: the readout white point, as a fraction of the AGC-tracked peak
  // luma. 1.0 puts tracked white at full scale; lower dims, higher clips into
  // white. The analog of the contrast pot in front of a set's IF AGC.
  double contrast = 1.0;
};

// The picture tube. A join sink fed three aligned rails — the picture (luma +
// chroma) and the two timebases (horizontal phase, vertical phase) — it drives
// each luma sample through the electron gun (DC-restored black sets the cutoff),
// matrixes in the colour difference for the three guns, and deposits the beam
// current at (x = h_phase*width, y from the yoke shear) onto a phosphor that
// decays over time. Interlace falls out for free: field 1's vertical sync is
// half a line late, so its lines land one output row below field 0's.
class Screen {
public:
  explicit Screen(const ScreenConfig &cfg);

  void prepare(std::size_t max_in);

  struct Frame {
    std::vector<std::uint8_t> pixels; // width*height*channels, row-major
    std::size_t width;
    std::size_t height;
    std::size_t channels; // 1 = grey, 3 = interleaved RGB
  };

  // on_field fires once per field boundary (the field_start sample), with the
  // phosphor snapshot as completed at that instant — the hook for a per-field
  // PNG sequence. (Field, not frame, until even/odd parity tracking lands with
  // colour; then this can fire per full frame.) Empty by default = no snapshots.
  using FieldCallback = std::function<void(const Frame &)>;
  void process(std::span<const ChromaSample> picture, std::span<const BeamSample> hbeam,
      std::span<const VSample> vbeam, const FieldCallback &on_field = {});

  // Decay the phosphor to the current instant and read it out, scaled by the
  // AGC-tracked white reference (one shared scale across R/G/B so hue is
  // preserved). Const — snapshotting doesn't disturb the running sim, so the
  // per-field callback can call it at every field boundary.
  [[nodiscard]] Frame snapshot() const;

private:
  // luma envelope -> beam drive (gun cutoff). h_phase locates the back porch for
  // the black DC-restore. The colour matrix then derives the three gun drives.
  [[nodiscard]] float drive_of(float luma, float h_phase);

  ScreenConfig cfg_;
  std::size_t channels_; // 1 (grey) or 3 (RGB), from cfg_.colour
  double log_decay_; // ln(per-sample phosphor decay factor)
  // IF-style AGC: a fast-attack, slow-release peak tracker on the luma gun drive,
  // advanced every sample. It sets the readout white point the way a real set's
  // IF AGC fixes the carrier-to-drive mapping — causal, no per-frame statistic.
  double white_ref_ = 0.0; // tracked peak luma gun output
  double agc_release_; // per-sample release factor (multi-field time constant)
  double phosphor_gain_; // steady-state accumulation of a per-frame re-deposit
  std::vector<float> bright_; // per-pixel-per-channel phosphor charge at last_[]
  std::vector<std::size_t> last_; // sample_index_ of each pixel's last deposit
  std::size_t sample_index_ = 0;

  // Deflection-yoke model + vertical beam splat. A real TV's yoke is rotated a
  // hair so each line traces straight across X even though the beam creeps
  // downward during the horizontal sweep. We apply the same shear continuously,
  // every sample:   row = v_phase*height - yoke_tilt_rows_ * h_phase   — the
  // second term cancels the in-line vertical creep, so a scanline traces flat
  // instead of smearing into a diagonal. yoke_tilt_rows_ is the rows the beam
  // steps per line (from the nominal scan geometry, like a real fixed yoke).
  // Each sample is then splatted vertically with a Gaussian spot centred on that
  // sheared row, filling the gaps between scanlines.
  double yoke_tilt_rows_ = 0.0; // vertical rows the beam advances per line
  std::size_t splat_radius_ = 0; // Gaussian half-width in rows
  std::vector<double> row_weights_; // scratch splat weights, size 2*radius+1

  // Levels, simulated rather than clamped. black_ tracks the back-porch blanking
  // shelf (DC restoration — a real TV's keyed-clamp circuit), which sets the
  // gun's cutoff: drive = black_ - env, so whiter (lower envelope) drives more
  // beam, while sync/blanking (at or above black) cut the beam off (the one
  // floor that's physical — electrons can't go negative). No upper bound: an
  // over-white deposits more charge and blooms. The only normalisation is the
  // peak scale at readout, for the 8-bit PNG.
  double black_ = 0.0;
  bool seeded_ = false;
};

// === Decoder (the composite graph node) ===

struct DecoderConfig {
  double sample_rate_hz;
  std::size_t width;
  std::size_t height;
  // The sync separator slices a low-passed copy of the envelope, not the full
  // one: sync only needs the slow pulse shapes, and the chroma/HF that the
  // picture rail keeps would otherwise chatter the slicer (badly so on a noisy
  // complex-baseband capture). The picture rail is untouched, so this trades
  // nothing on the image. Cutoff is a fraction of a sync pulse's bandwidth.
  double sync_lp_cutoff_hz = 1.2e6;
  // The sub-stage configs, so every knob is reachable from one place (the
  // interactive tuner, and eventually live control). Their sample_rate_hz is
  // filled in from this config's at construction, so leave it 0 here.
  SyncSeparatorConfig sep{};
  HorizontalSweepConfig hsweep{};
  VerticalSyncConfig vsync{};
  // Phosphor persistence, in field periods (see ScreenConfig). Higher evens out
  // the brightness between the two interlaced fields; lower sharpens motion.
  double persistence_fields = 1.2;
  // Beam-spot vertical size in output rows (see ScreenConfig::beam_sigma_rows).
  double beam_sigma_rows = 0.8;
  double gamma = 1.0; // electron-gun gamma (see ScreenConfig::gamma)
  // Colour: decode chroma and render an RGB triad. Off => the grey rail (the
  // luma envelope straight to the screen, chroma untouched). saturation scales
  // the colour-difference signals into the gun matrix (see ScreenConfig).
  bool colour = false;
  double saturation = 1.0;
  double contrast = 1.0; // readout white point (see ScreenConfig::contrast)
  ChromaDecoderConfig chroma{}; // sample_rate_hz filled in at construction
};

// The whole video graph as one streaming node: it owns the separator, the two
// sweeps and the screen, and hides the fan-out (the sync bit feeds both
// timebases) and the 3-way join (the screen). Feed it envelope blocks; read a
// frame off snapshot(). This is the reified branching graph — the wiring lives
// here, once, instead of in each consumer's block lambda.
class Decoder {
public:
  explicit Decoder(const DecoderConfig &cfg);

  void prepare(std::size_t max_in);
  // on_field, if set, fires the phosphor snapshot at every field boundary (the
  // hook for a per-field PNG sequence); otherwise just integrates the screen.
  void process(std::span<const float> envelope, const Screen::FieldCallback &on_field = {});
  [[nodiscard]] Screen::Frame snapshot() const { return screen_.snapshot(); }

  [[nodiscard]] std::size_t accepted_edges() const noexcept { return hsweep_.accepted_edges(); }
  [[nodiscard]] std::size_t rejected_edges() const noexcept { return hsweep_.rejected_edges(); }
  [[nodiscard]] double line_omega() const noexcept { return hsweep_.omega(); }
  [[nodiscard]] std::size_t detected_fields() const noexcept { return vsync_.detected_fields(); }
  [[nodiscard]] double field_omega() const noexcept { return vsync_.omega(); }
  // Colour diagnostics (meaningful only when colour is enabled).
  [[nodiscard]] double subcarrier_hz() const noexcept { return chroma_.subcarrier_hz(); }
  [[nodiscard]] double burst_amplitude() const noexcept { return chroma_.burst_amplitude(); }
  [[nodiscard]] double parity_consistency_deg() const noexcept { return chroma_.parity_consistency_deg(); }

private:
  bool colour_;
  dsp::Fir sync_lp_; // narrow low-pass on the sync branch only (picture rail untouched)
  SyncSeparator sep_;
  HorizontalSweep hsweep_;
  VerticalSync vsync_;
  ChromaDecoder chroma_;
  Screen screen_;
  Buffer<ChromaSample> grey_pic_; // luma-only wrapper for the monochrome path
};

} // namespace palindrome::video
