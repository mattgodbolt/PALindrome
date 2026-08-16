#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/video_types.hpp"

#include <cstddef>
#include <span>

namespace palindrome::video {

struct CompositeInputConfig {
  double sample_rate_hz;
  double nominal_line_hz = kNominalLineHz;
  // Volts at input full scale (+-1.0), and the source's sync amplitude. Between
  // them they fix the one gain the conversion needs; a capture that declares
  // neither gets the nominal 1 V pk-pk with 0.3 V of sync.
  double full_scale_volts = kNominalFullScaleVolts;
  double sync_amplitude_v = kNominalSyncVolts;
  // Sync-tip clamp release, in line periods. The attack is instant, so a DC
  // level falling away is caught within the sample; the release only has to
  // follow drift the other way. That sets the trade: too fast and the clamp
  // rides up between sync pulses (the gap closes by 1 - exp(-1/clamp_lines)
  // every line, which lands straight on the picture as a per-line level
  // wobble), too slow and mains hum walks the black level. 32 lines is ~2 ms,
  // an order under a 20 ms hum cycle, for ~3% of inter-sync droop.
  double clamp_lines = 32.0;
};

// Baseband composite (CVBS) in, receiver envelope out.
//
// The rail this produces is the detector's rail: negatively modulated, sync tip
// at kSyncTipLevel, blanking and peak white on the standard's geometry. So the
// conversion is affine with two anchored points, not merely an inversion - the
// fixed slicer and the absolute white reference downstream both depend on the
// ratio between them, and a stage that only flipped the sign would slice fine
// and render at the wrong contrast.
//
// The tip anchor is tracked rather than assumed, which doubles as the DC
// restoration an AC-coupled capture needs: Screen clamps the black level for
// the gun, but that sits downstream of two stages (the fixed slicer, and the
// AGC's gain-to-zero) that need an absolute reference of their own. A sync-tip
// clamp needs no timing feedback to find it, which is why it is this and not
// the back-porch keyed clamp a real set uses - keying would want h_phase fed
// back into the front end, the feedback edge issue #58 defers for the keyed AGC.
//
// A wrong scale cannot break sync: the tip lands on kSyncTipLevel by
// construction, so the slicer fires as long as the sync depth clears its slice
// level. Getting the scale wrong shows up as contrast, which has a pot.
class CompositeInput {
public:
  explicit CompositeInput(const CompositeInputConfig &cfg);

  void prepare(std::size_t max_in);
  [[nodiscard]] std::span<const float> process(std::span<const float> composite);

  [[nodiscard]] std::size_t max_output_for(std::size_t n) const noexcept { return n; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

  // Diagnostics: the clamp's current sync-tip reference, in input units, and
  // the derived gain in envelope units per input unit.
  [[nodiscard]] double clamp_level() const noexcept { return tip_; }
  [[nodiscard]] double scale() const noexcept { return scale_; }

private:
  double rise_; // per-sample fraction of the gap the clamp releases upward
  double scale_; // envelope units per input unit
  double tip_ = 0.0; // tracked sync tip of the raw input: slow accumulator
  bool seeded_ = false;
  Buffer<float> out_;
};

} // namespace palindrome::video
