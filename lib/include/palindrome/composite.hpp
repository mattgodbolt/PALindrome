#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/video_types.hpp"

#include <cstddef>
#include <span>

namespace palindrome::video {

struct CompositeInputConfig {
  double sample_rate_hz;
  double nominal_line_hz = kNominalLineHz;
  // Volts at input full scale (+-1.0), and the source's sync amplitude. Only
  // their ratio reaches the gain, but two physical numbers beat one abstract
  // one. Declaring sync_amplitude_v is how a shallow-sync source is *fixed*
  // rather than merely tolerated: on RF a source that under-modulates is
  // irreducibly low-contrast, because the AGC can only anchor the carrier tip.
  // Here the map restores full geometry for a source that under-modulates
  // uniformly (sync and picture shrinking together; declaring a shallow sync
  // amplitude against a full picture span over-scales it instead), and with
  // that geometry comes back the slicer's margin -
  // left at nominal, a source with 0.135 sync puts blanking at 0.865 against a
  // 0.925 slice, three times closer to a false trigger than the standard 0.16.
  double full_scale_volts = kNominalFullScaleVolts;
  double sync_amplitude_v = kNominalSyncVolts;
  // Sync-tip clamp release, in line periods. The attack is instant, so a level
  // falling away is caught within the sample; the release only has to follow
  // drift the other way. That sets the trade, and both ends of it were
  // measured rather than reasoned: the clamp rides up between sync pulses by
  // 1 - exp(-1/clamp_lines) of the gap every line, and since the gap is the
  // distance to picture content the droop scales with brightness, landing as a
  // left-to-right shading. On a full-white field that is 4.1% of the white
  // drive at 32 lines and 1.3% at 128 (Screen's back-porch clamp removes the
  // line-to-line offset but not the within-line ramp, so it reaches the
  // picture). Against it, slowing the release deepens the noise bias below -
  // but only marginally, and the cliff does not move. 512 lines is where 50 Hz
  // hum finally walks a black picture off the slicer.
  double clamp_lines = 128.0;
};

// Baseband composite (CVBS) in, receiver envelope out.
//
// The rail this produces is the detector's rail: negatively modulated, sync tip
// at kSyncTipLevel and blanking at kBlankingLevel. So the conversion is affine
// with two anchored points, not merely an inversion - the fixed slicer and the
// absolute white reference downstream both depend on the ratio between them,
// and a stage that only flipped the sign would slice fine and render at the
// wrong contrast. Peak white then follows at kPeakWhiteLevel given a source
// whose picture span is the standard 0.7 V; only the tip and the sync amplitude
// enter the map.
//
// The tip anchor is tracked rather than assumed, which doubles as the DC
// restoration an AC-coupled capture needs: Screen clamps the black level for
// the gun, but that sits downstream of two stages (the fixed slicer, and the
// AGC's gain-to-zero) that need an absolute reference of their own. A sync-tip
// clamp needs no timing feedback to find it, which is why it is this and not
// the back-porch keyed clamp a real set uses - keying would want h_phase fed
// back into the front end, the feedback edge issue #58 defers for the keyed AGC.
//
// Two consequences of clamping on the tip, both of them the real circuit's
// too, which is exactly why sets that could afford it keyed on the back porch:
// a negative impulse seizes the clamp for as long as the release takes to climb
// back (a full sync amplitude of overshoot costs about nine lines), and under
// noise the tracked minimum sits below the true tip, depressing the whole rail
// by a DC offset the sync low-pass cannot remove. Measured, the slicer starts
// chattering within the sync pulse somewhere past 0.015 V RMS on a 0.3 V sync.
// An over-saturated source can capture it outright, if chroma troughs dip below
// sync tip - console modulators being just the population that does that.
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
