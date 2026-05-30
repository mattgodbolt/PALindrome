#pragma once

#include "palindrome/buffer.hpp"

#include <cstddef>
#include <span>

// Video reconstruction stages, modelled on the analog TV signal chain rather
// than as one monolith. A real set splits the job across parts: a sync
// separator slices the composite into a clean sync signal, a horizontal
// timebase (AFC + flywheel oscillator) locks the line sweep to it, and the
// picture tube paints brightness at the swept beam position. We mirror that
// split so each stage has one job and an inspectable output:
//
//   float envelope --[SyncSeparator]--> SyncSample --[HorizontalSweep]--> BeamSample --> render
//
// Each stage is a streaming block (prepare / process / max_output_for /
// input_multiple) with state carried across process() calls, so the result is
// independent of how the input is chunked — same contract as the dsp stages.
namespace palindrome::video {

// === Sync separator ===

// Output of the sync separator. `envelope` rides through unchanged (the
// picture rail the renderer eventually paints); `sync` is the sliced one-bit
// sync signal — true while the envelope sits in the sync region (above the
// slice level, since vision is negatively modulated so sync tips are the
// envelope peaks). It is true during EVERY sync pulse: line sync and the broad
// / equalising pulses of the vertical interval alike. Telling those pulse
// kinds apart belongs downstream, exactly as a real set slices first and
// discriminates later.
struct SyncSample {
  float envelope = 0.0f;
  bool sync = false;
};

struct SyncSeparatorConfig {
  double sample_rate_hz;
  // Slice level as a fraction of the tracked floor(white) -> peak(sync-tip)
  // range. PAL puts the sync tip at 100% of the carrier and black at ~75%, so
  // a slice around 0.85 sits safely inside the sync region.
  double sync_level = 0.85;
};

// Slices composite into a clean one-bit sync signal. Tracks a running peak
// (sync tip) and floor (active-video white) so the slice point follows the
// recording's amplitude, with hysteresis so chroma ripple on a transition
// doesn't chatter the output. No timing loop here — it just slices.
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

// Output of the horizontal sweep: the envelope still riding through, plus the
// beam's horizontal position as a phase in [0, 1) (0 at the locked line start),
// and `line_start` true on the single sample where the sweep locks a new line.
// Downstream reads line_start as the source of truth for "a line began" rather
// than inferring it from the phase float trajectory.
struct BeamSample {
  float envelope = 0.0f;
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

} // namespace palindrome::video
