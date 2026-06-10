#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/video_types.hpp"

#include <cstddef>
#include <span>

namespace palindrome::video {

struct SyncSeparatorConfig {
  double sample_rate_hz;
  // The period scheme (default): with the front-end AGC holding the sync tip
  // at 1.0, slice a fixed slice_depth below it - a circuit value, never a
  // measurement of picture content. System I sync is 0.24 of the tip deep, but
  // under-modulated sources (console RF modulators) can run much shallower
  // (the SMS corpus measures ~0.135), so the default sits well inside both.
  double slice_depth = 0.08;
  // The modern convenience this replaced: track a running peak (sync tip) and
  // floor (white) and slice at sync_level of that range, so the slice point
  // follows whatever arrives - no AGC needed, but picture content moves it.
  bool adaptive = false;
  double sync_level = 0.85; // adaptive-mode slice, fraction of floor->peak
};

// Slices the composite envelope into a clean one-bit sync signal, with
// hysteresis so chroma ripple on a transition doesn't chatter the output. The
// slice level is a fixed depth below the AGC'd sync tip (or, in adaptive mode,
// a fraction of tracked floor/peak levels). No timing loop here - it just
// slices.
class SyncSeparator {
public:
  explicit SyncSeparator(const SyncSeparatorConfig &cfg);

  void prepare(std::size_t max_in);
  [[nodiscard]] std::span<const SyncSample> process(std::span<const float> envelope);

  [[nodiscard]] std::size_t max_output_for(std::size_t n) const noexcept { return n; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

private:
  std::span<const SyncSample> process_fixed(std::span<const float> envelope);
  std::span<const SyncSample> process_adaptive(std::span<const float> envelope);

  SyncSeparatorConfig cfg_;
  double peak_ = 0.0; // running sync-tip level (adaptive mode)
  double floor_ = 0.0; // running white level (adaptive mode)
  bool sync_ = false; // current sliced state (hysteresis)
  bool seeded_ = false;
  Buffer<SyncSample> out_;
};

} // namespace palindrome::video
