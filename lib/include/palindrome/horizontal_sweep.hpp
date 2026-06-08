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
