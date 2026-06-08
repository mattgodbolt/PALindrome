#pragma once

#include "palindrome/video_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace palindrome::video {

struct ScreenConfig {
  std::size_t width;
  std::size_t height;
  double sample_rate_hz;
  double field_hz = kNominalFieldHz;
  double nominal_line_hz = kNominalLineHz; // sets the deflection-yoke shear (see below)
  // Phosphor persistence as a multiple of the field period. The beam deposits
  // brightness where it lands; each pixel then decays exponentially with this
  // time constant. ~1 field means roughly the last two fields survive — so an
  // interlaced frame builds up and older content (and the startup junk) fades.
  double persistence_fields = 1.2;
  // Beam-spot size: each sample is splatted with a round 2-D Gaussian of this
  // standard deviation. beam_sigma_rows is the vertical extent in output rows (it
  // fills the gaps between scanlines — the focus); beam_sigma_cols is the
  // horizontal extent in output columns and defaults to match (a round spot) when
  // negative. 0 => a single pixel (no splat) on that axis.
  double beam_sigma_rows = 0.8;
  double beam_sigma_cols = -1.0; // < 0 => match beam_sigma_rows (round spot)
  // Electron-gun gamma: beam current rises as drive^gamma. The source pre-distorts
  // its video (~1/2.2) expecting a CRT to undo it, so ~2.2-2.8 restores the
  // midtones/contrast; 1.0 is linear (no gamma). Readout normalisation makes the
  // absolute drive scale irrelevant, so no white reference is needed.
  double gamma = 1.0;
  // Colour: a three-phosphor (RGB) triad driven by three guns matrixed from
  // Y/U/V, vs a single grey phosphor (u/v ignored). saturation scales the
  // colour-difference signals into the matrix before the per-gun gamma.
  bool colour = false;
  double saturation = 1.0;
  // Contrast: the readout white point, as a fraction of the AGC-tracked peak
  // luma. 1.0 puts tracked white at full scale; lower dims, higher clips into
  // white. The analog of the contrast pot in front of a set's IF AGC.
  double contrast = 1.0;
  // Retrace blanking: the beam is held off for h_phase below this — the sync,
  // back porch and colour burst — exactly as a real set's line-blanking pulse
  // keeps the flyback (and the burst) off the screen. h_phase = 0 is the sync
  // leading edge; active video starts after the back porch.
  double h_blank = 0.16;
  // How many samples the picture rail lags the sync-locked timing rails (the
  // chroma path's group delay, in colour). The screen shifts the picture back by
  // this so colour registers with mono instead of sliding right — the luminance
  // delay line, in reverse. 0 in mono (the picture is the raw envelope).
  double picture_lag_samples = 0.0;
};

// The picture tube. A join sink fed three aligned rails — the picture (luma +
// chroma) and the two timebases (horizontal phase, vertical phase) — it drives
// each luma sample through the electron gun (DC-restored black sets the cutoff),
// matrixes in the colour difference for the three guns, and deposits the beam
// current at (x = h_phase*width, y from the yoke shear) onto a phosphor that
// decays over time. Interlace falls out for free: field 1's vertical sync is
// half a line late, so its lines land one output row below field 0's.
class Screen {
public:
  explicit Screen(const ScreenConfig &cfg);

  void prepare(std::size_t max_in);

  struct Frame {
    std::vector<std::uint8_t> pixels; // width*height*channels, row-major
    std::size_t width;
    std::size_t height;
    std::size_t channels; // 1 = grey, 3 = interleaved RGB
  };

  // on_field fires once per field boundary (the field_start sample), with the
  // phosphor snapshot as completed at that instant — the hook for a per-field
  // PNG sequence. (Field, not frame, until even/odd parity tracking lands with
  // colour; then this can fire per full frame.) Empty by default = no snapshots.
  using FieldCallback = std::function<void(const Frame &)>;
  void process(std::span<const ChromaSample> picture, std::span<const BeamSample> hbeam, std::span<const VSample> vbeam,
      const FieldCallback &on_field = {});

  // Decay the phosphor to the current instant and read it out, scaled by the
  // AGC-tracked white reference (one shared scale across R/G/B so hue is
  // preserved). Const — snapshotting doesn't disturb the running sim, so the
  // per-field callback can call it at every field boundary.
  [[nodiscard]] Frame snapshot() const;

private:
  // luma envelope -> beam drive (gun cutoff). h_phase locates the back porch for
  // the black DC-restore. The colour matrix then derives the three gun drives.
  [[nodiscard]] float drive_of(float luma, float h_phase);

  ScreenConfig cfg_;
  std::size_t channels_; // 1 (grey) or 3 (RGB), from cfg_.colour
  // IF-style AGC: a fast-attack, slow-release peak tracker on the luma gun drive,
  // advanced every sample. It sets the readout white point the way a real set's
  // IF AGC fixes the carrier-to-drive mapping — causal, no per-frame statistic.
  double white_ref_ = 0.0; // tracked peak luma gun output (readout white)
  double white_drive_ = 0.0; // tracked peak luma drive (chroma reference, pre-gamma)
  double agc_release_; // per-sample release factor (multi-field time constant)
  double phosphor_gain_; // steady-state accumulation of a per-frame re-deposit
  // Phosphor framebuffer. The beam ADDS charge (no per-sample decay); the whole
  // buffer is faded by field_decay_ once per field, at the field boundary. This
  // is what a viewer's eye integrates — a field paints as one instant, so there's
  // no top-to-bottom brightness ramp from the beam's sweep down the screen, and
  // it's a single streaming multiply (cache- and GPU-friendly) instead of a
  // per-pixel lazy decay keyed on a last-touched timestamp.
  //
  // The alternative is a continuous per-sample decay (each pixel faded forward to
  // the read instant from when it was last hit). That's physically what a CRT does
  // at any single moment — what a fast-shutter camera captures — but it shows the
  // beam-sweep band a human never sees, and it costs a second random-access array
  // plus a per-deposit exp(). To bring that "camera snapshot" look back (likely as
  // a ScreenConfig mode selecting the deposit + snapshot path), the whole removed
  // implementation — last_, sample_index_, the split decay LUTs, decay_for, and
  // the fade-to-now in snapshot() — sits in the commit before "fade the phosphor
  // per field, not per sample" (git log -- lib/video.cpp; show its parent).
  std::vector<float> bright_; // per-pixel-per-channel accumulated phosphor charge
  float field_decay_ = 1.0f; // whole-buffer multiply applied once per field

  // Deflection-yoke model + vertical beam splat. A real TV's yoke is rotated a
  // hair so each line traces straight across X even though the beam creeps
  // downward during the horizontal sweep. We apply the same shear continuously,
  // every sample:   row = v_phase*height - yoke_tilt_rows_ * h_phase   — the
  // second term cancels the in-line vertical creep, so a scanline traces flat
  // instead of smearing into a diagonal. yoke_tilt_rows_ is the rows the beam
  // steps per line (from the nominal scan geometry, like a real fixed yoke).
  // Each sample is then splatted with a round 2-D Gaussian spot centred on that
  // sheared sub-pixel position, filling the gaps between scanlines (vertically)
  // and reconstructing along the line (horizontally).
  double yoke_tilt_rows_ = 0.0; // vertical rows the beam advances per line
  double picture_h_offset_ = 0.0; // h_phase shift to register the picture rail (see ScreenConfig)
  std::size_t splat_radius_y_ = 0; // Gaussian half-width in rows
  std::size_t splat_radius_x_ = 0; // Gaussian half-width in columns

  // The beam spot is a separable 2-D Gaussian: a vertical kernel over rows and a
  // horizontal one over columns, each normalised to sum 1. Both depend only on
  // the spot centre's sub-pixel fraction along that axis, so each is tabulated per
  // fraction bin (a [bin][cell] table) instead of calling exp() per sample.
  static constexpr std::size_t kGaussBins = 4096;
  std::size_t gauss_stride_y_ = 1; // 2*splat_radius_y_+1 (rows)
  std::size_t gauss_stride_x_ = 1; // 2*splat_radius_x_+1 (columns)
  std::vector<float> gauss_lut_y_; // kGaussBins * gauss_stride_y_, normalised (vertical)
  std::vector<float> gauss_lut_x_; // kGaussBins * gauss_stride_x_, normalised (horizontal)

  // The electron-gun curve drive^gamma is the last un-LUT'd per-sample
  // transcendental; tabulate it over the drive domain (built once for cfg_.gamma)
  // with linear interpolation, falling back to pow() above the table for bloom.
  static constexpr std::size_t kGunBins = 8192;
  static constexpr double kGunDriveMax = 8.0;
  std::vector<float> gun_lut_; // pow(drive, gamma) over [0, kGunDriveMax]; empty when gamma == 1
  [[nodiscard]] float gun(double drive) const;

  // Levels, simulated rather than clamped. black_ tracks the back-porch blanking
  // shelf (DC restoration — a real TV's keyed-clamp circuit), which sets the
  // gun's cutoff: drive = black_ - env, so whiter (lower envelope) drives more
  // beam, while sync/blanking (at or above black) cut the beam off (the one
  // floor that's physical — electrons can't go negative). No upper bound: an
  // over-white deposits more charge and blooms. The only normalisation is the
  // peak scale at readout, for the 8-bit PNG.
  double black_ = 0.0;
  bool seeded_ = false;
};

} // namespace palindrome::video
