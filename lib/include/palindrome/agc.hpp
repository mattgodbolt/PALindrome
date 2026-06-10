#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/video_types.hpp"

#include <cstddef>
#include <span>

namespace palindrome::video {

// How the receiver derives its level reference. sync_tip is the period scheme:
// the IF AGC normalises the carrier so the sync tip sits at 1.0 and every level
// downstream is absolute (the slicer cuts a fixed depth below the tip, white is
// the standard's geometry). adaptive is the modern convenience this replaces:
// per-stage floor/peak trackers that stretch whatever arrives to full range.
enum class AgcMode { sync_tip, adaptive };

struct AgcConfig {
  double sample_rate_hz;
  double nominal_field_hz = kNominalFieldHz;
  // Gain recovery time constant, in field periods. Attack is immediate (a
  // stronger carrier pulls the gain down within the sample), release is slow:
  // negative modulation puts the sync tip at peak carrier, so the tracked peak
  // must hold across a 64 µs line (and the picture content below it) while
  // still following a genuine level drop within a few fields.
  double decay_fields = 2.0;
};

// The IF AGC: a peak detector on the envelope servoing the gain so the carrier
// peak — the sync tip, under negative modulation — sits at 1.0. This is what
// makes the receiver's levels absolute: black is then the clamped back porch
// and white is the transmission standard's geometry, not a measurement of
// picture content. A true keyed AGC gates the detector from the line flywheel
// (better impulse-noise immunity); that needs a feedback edge into the front
// end and waits for the IF rebuild (issue #37) — until then the per-sample
// peak detector is the simple-set equivalent, and an impulse that overshoots
// the tip dims the picture for a few fields, which is what cheap sets did.
class Agc {
public:
  explicit Agc(const AgcConfig &cfg);

  void prepare(std::size_t max_in);
  [[nodiscard]] std::span<const float> process(std::span<const float> envelope);

  [[nodiscard]] std::size_t max_output_for(std::size_t n) const noexcept { return n; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

  // Diagnostic: the gain currently applied (1 / tracked tip).
  [[nodiscard]] double gain() const noexcept { return tip_ > 0.0 ? 1.0 / tip_ : 0.0; }

private:
  // The config collapses to one derived constant; nothing else is consulted
  // after construction (the keyed-AGC rebuild in issue #37 may change that).
  double release_; // per-sample tip decay factor
  double tip_ = 0.0; // tracked carrier peak (the sync tip): slow accumulator
  bool seeded_ = false;
  Buffer<float> out_;
};

} // namespace palindrome::video
