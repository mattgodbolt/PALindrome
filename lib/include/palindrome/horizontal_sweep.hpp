#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/video_types.hpp"

#include <cstddef>
#include <span>

namespace palindrome::video {

struct HorizontalSweepConfig {
  double sample_rate_hz;
  double nominal_line_hz = kNominalLineHz;
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
  // AFC PI loop, two sets of gains switched by the coincidence detector below —
  // the flywheel sync of a real set's line-oscillator IC (TDA2593-era). kp
  // corrects phase toward the sync anchor; ki tracks the recording's true line
  // rate. The LOCKED gains are deliberately low-bandwidth (kp 0.1 ~ a 250 Hz
  // loop at PAL line rate), so single-edge noise barely moves the line — at the
  // cost of the authentic slow-tracking artifacts (top-of-picture flagging on
  // step phase errors, slow recentring). The ACQUISITION gains pull in fast.
  // Setting both kp's to 1.0 (and the ki's equal) restores the old behaviour:
  // direct triggering, every accepted edge snapping the oscillator exactly.
  // omega is anti-windup clamped to +/- omega_clamp of nominal so a sustained
  // biased error during acquisition can't drive it to zero (which would invert
  // the lockout-gap formula min_line_fraction / omega).
  double pll_kp = 0.1; // locked phase gain
  double pll_ki = 1.0e-5; // locked frequency gain
  double acq_kp = 0.5; // acquisition phase gain
  double acq_ki = 1.0e-4; // acquisition frequency gain
  double omega_clamp = 0.2;
  // Coincidence detector: an accepted edge landing within this window (line
  // fractions) of where the free-running oscillator predicted counts as
  // coincident. The detector is a saturating charge (cap 2 * coincidence_lines):
  // coincident edges add one, non-coincident edges drain several times faster
  // (see kCoincidencePenalty), and the loop runs the locked gains while the
  // charge is at or above coincidence_lines. So from cold, coincidence_lines
  // coincident edges engage the flywheel; once saturated, brief noise doesn't
  // kick it back into fast mode, but a sustained loss drains it within a
  // handful of lines.
  double coincidence_window = 0.02;
  std::size_t coincidence_lines = 16;
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
  // locked line rate in Hz); locked() reports the coincidence detector's state
  // (slow flywheel gains vs fast acquisition).
  [[nodiscard]] std::size_t accepted_edges() const noexcept { return accepted_; }
  [[nodiscard]] std::size_t rejected_edges() const noexcept { return rejected_; }
  [[nodiscard]] double omega() const noexcept { return omega_; }
  [[nodiscard]] bool locked() const noexcept { return locked_; }

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
  std::size_t coincidence_ = 0; // the detector's charge: coincident edges fill, misses drain faster
  bool locked_ = false; // detector above threshold => the slow flywheel gains
  std::size_t accepted_ = 0;
  std::size_t rejected_ = 0;
  Buffer<BeamSample> out_;
};

} // namespace palindrome::video
