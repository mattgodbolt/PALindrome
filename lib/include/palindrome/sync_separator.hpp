#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/video_types.hpp"

#include <cstddef>
#include <span>

namespace palindrome::video {

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

} // namespace palindrome::video
