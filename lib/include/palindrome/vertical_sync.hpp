#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/video_types.hpp"

#include <cstddef>
#include <span>

namespace palindrome::video {

struct VerticalSyncConfig {
  double sample_rate_hz;
  double nominal_field_hz = kNominalFieldHz;
  double nominal_line_hz = kNominalLineHz; // sets the integrator time constant in lines
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

} // namespace palindrome::video
