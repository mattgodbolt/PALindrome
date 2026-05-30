#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/fir.hpp"

#include <cstddef>
#include <cstdint>
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
  bool seeded_ = false; // peak/floor seeded from the first sample yet?
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

// === Screen (phosphor, the 3-way join sink) ===

struct ScreenConfig {
  std::size_t width;
  std::size_t height;
  double sample_rate_hz;
  double field_hz = 50.0;
  // Phosphor persistence as a multiple of the field period. The beam deposits
  // brightness where it lands; each pixel then decays exponentially with this
  // time constant. ~1 field means roughly the last two fields survive — so an
  // interlaced frame builds up and older content (and the startup junk) fades.
  double persistence_fields = 1.2;
};

// The picture tube. A join sink fed three aligned rails — the picture
// (envelope) and the two timebases (horizontal phase, vertical phase) — it
// paints each envelope sample as a brightness deposit at the beam position
// (x = h_phase*width, y = v_phase*height) onto a phosphor that decays over
// time. Interlace falls out for free: field 1's vertical sync is half a line
// late, so its lines land one output row below field 0's. The env->brightness
// map here is a placeholder running AGC, standing in for the sync-locked
// black-level clamp (the levels stage) still to come.
class Screen {
public:
  explicit Screen(const ScreenConfig &cfg);

  void prepare(std::size_t max_in);
  void process(std::span<const float> envelope, std::span<const BeamSample> hbeam, std::span<const VSample> vbeam);

  struct Frame {
    std::vector<std::uint8_t> grey; // width*height, row-major
    std::size_t width;
    std::size_t height;
  };
  // Decay the phosphor to the current instant and read it out as a grey frame,
  // peak-normalised. Const — snapshotting doesn't disturb the running sim, so
  // a future per-frame PNG sequence can call it at every field boundary.
  [[nodiscard]] Frame snapshot() const;

private:
  [[nodiscard]] float intensity_of(float env); // env -> beam brightness via running AGC

  ScreenConfig cfg_;
  double log_decay_; // ln(per-sample phosphor decay factor)
  std::vector<float> bright_; // per-pixel phosphor charge at last_[]
  std::vector<std::size_t> last_; // sample_index_ of each pixel's last deposit
  std::size_t sample_index_ = 0;
  double peak_ = 0.0; // running sync-tip level (AGC)
  double floor_ = 0.0; // running white level (AGC)
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
  void process(std::span<const float> envelope);
  [[nodiscard]] Screen::Frame snapshot() const { return screen_.snapshot(); }

  [[nodiscard]] std::size_t accepted_edges() const noexcept { return hsweep_.accepted_edges(); }
  [[nodiscard]] std::size_t rejected_edges() const noexcept { return hsweep_.rejected_edges(); }
  [[nodiscard]] double line_omega() const noexcept { return hsweep_.omega(); }
  [[nodiscard]] std::size_t detected_fields() const noexcept { return vsync_.detected_fields(); }
  [[nodiscard]] double field_omega() const noexcept { return vsync_.omega(); }

private:
  dsp::Fir sync_lp_; // narrow low-pass on the sync branch only (picture rail untouched)
  SyncSeparator sep_;
  HorizontalSweep hsweep_;
  VerticalSync vsync_;
  Screen screen_;
};

} // namespace palindrome::video
