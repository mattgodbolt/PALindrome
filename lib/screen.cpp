#include "palindrome/screen.hpp"

#include "palindrome/gaussian.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace palindrome::video {

Screen::Screen(const ScreenConfig &cfg) :
    cfg_{cfg}, channels_{cfg.colour ? std::size_t{3} : std::size_t{1}},
    bright_(cfg.width * cfg.height * (cfg.colour ? 3 : 1), 0.0f) {
  if (cfg_.width == 0 || cfg_.height == 0)
    throw std::invalid_argument{"Screen: width and height must be positive"};
  if (!(cfg_.saturation >= 0.0))
    throw std::invalid_argument{"Screen: saturation must be non-negative"};
  if (!(cfg_.sample_rate_hz > 0.0 && cfg_.field_hz > 0.0))
    throw std::invalid_argument{"Screen: sample_rate_hz and field_hz must be positive"};
  // Upper bound keeps the per-field decay safely away from a degenerate 1.0
  // (no fade); 64 fields is already far longer than any real phosphor.
  if (!(cfg_.persistence_fields > 0.0 && cfg_.persistence_fields <= 64.0))
    throw std::invalid_argument{"Screen: persistence_fields must be in (0, 64]"};
  if (!(cfg_.beam_sigma >= 0.0))
    throw std::invalid_argument{"Screen: beam_sigma must be non-negative"};
  if (!(cfg_.nominal_line_hz > cfg_.field_hz))
    throw std::invalid_argument{"Screen: nominal_line_hz must exceed field_hz"};
  if (!(cfg_.h_window_lo < cfg_.h_window_hi && cfg_.v_window_lo < cfg_.v_window_hi))
    throw std::invalid_argument{"Screen: scan windows need lo < hi"};
  if (!(cfg_.readout_gamma > 0.0))
    throw std::invalid_argument{"Screen: readout_gamma must be positive"};
  if (!(cfg_.eht_sag >= 0.0 && cfg_.eht_sag < 0.5))
    throw std::invalid_argument{"Screen: eht_sag must be in [0, 0.5)"};
  // Lower bound keeps the per-line integrator step alpha = 1/(tc * lines per
  // field) at or below 1, so eht_ stays a convex combination and can't ring
  // (the same guard VerticalSync places on its integrator).
  if (!(cfg_.eht_tc_fields * cfg_.nominal_line_hz / cfg_.field_hz >= 1.0))
    throw std::invalid_argument{"Screen: eht_tc_fields too small (the integrator step would exceed 1)"};
  if (!(cfg_.eht_focus >= 0.0 && cfg_.eht_focus <= 1.0))
    throw std::invalid_argument{"Screen: eht_focus must be in [0, 1]"};
  if (!(cfg_.line_pull >= 0.0 && cfg_.line_pull < 0.1))
    throw std::invalid_argument{"Screen: line_pull must be in [0, 0.1)"};
  if (!(cfg_.bcl_threshold >= 0.0 && cfg_.bcl_threshold <= 1.5))
    throw std::invalid_argument{"Screen: bcl_threshold must be in [0, 1.5] (0 disables)"};
  // Only meaningful (and only used) when the limiter is enabled.
  if (cfg_.bcl_threshold > 0.0 && !(cfg_.bcl_tc_fields * cfg_.nominal_line_hz / cfg_.field_hz >= 1.0))
    throw std::invalid_argument{"Screen: bcl_tc_fields too small (the integrator step would exceed 1)"};

  // Decay per sample so that brightness falls by 1/e over persistence_fields
  // field periods: tau_samples = persistence * (sample_rate / field_hz).
  const double tau_samples = cfg_.persistence_fields * cfg_.sample_rate_hz / cfg_.field_hz;
  const double log_decay = -1.0 / tau_samples;
  // AGC release: hold the tracked white for a few fields, so the white point is
  // steady against picture content (the slow time constant of a real IF AGC).
  constexpr double kAgcFields = 8.0;
  agc_release_ = std::exp(-1.0 / (kAgcFields * cfg_.sample_rate_hz / cfg_.field_hz));
  // A pixel hit once per frame (two field periods, interlaced) accumulates its
  // deposit to a steady 1/(1 - decay_per_frame); white reads that, so divide it
  // out to put tracked white at full scale.
  const double decay_per_frame = std::exp(log_decay * 2.0 * cfg_.sample_rate_hz / cfg_.field_hz);
  phosphor_gain_ = 1.0 / (1.0 - decay_per_frame);
  // The phosphor fades by one field period's worth in a single whole-buffer
  // multiply at each field boundary (not per sample): exp(log_decay * samples
  // per field). Steady state for a pixel re-hit every frame (two fields) is then
  // 1/(1 - field_decay_^2) == phosphor_gain_, so the readout scale is unchanged.
  field_decay_ = static_cast<float>(std::exp(log_decay * cfg_.sample_rate_hz / cfg_.field_hz));
  // The scan-to-frame mapping: the configured window fills the output, so a
  // window narrower than the full scan magnifies (the overscan). With [0,1]
  // windows these are just width and height — the original full-scan framing.
  x_scale_ = static_cast<double>(cfg_.width) / (cfg_.h_window_hi - cfg_.h_window_lo);
  y_scale_ = static_cast<double>(cfg_.height) / (cfg_.v_window_hi - cfg_.v_window_lo);
  // Yoke shear: the beam steps (field_hz / line_hz) of a field per line, i.e.
  // this many output rows — the amount to un-creep within each line. The field
  // spans y_scale_ rows (more than the frame when overscanned), so the shear
  // scales with it.
  yoke_tilt_rows_ = y_scale_ * cfg_.field_hz / cfg_.nominal_line_hz;
  // The picture rail lags the timing rails by picture_lag_samples; one sample is
  // nominal_line_hz / sample_rate of a line in h_phase, so shift the picture back
  // by that fraction to register it with the sweep.
  picture_h_offset_ = cfg_.picture_lag_samples * cfg_.nominal_line_hz / cfg_.sample_rate_hz;
  // The beam spot is a round 2-D Gaussian, splatted out to 2.5 sigma on each axis
  // as a separable pair of 1-D kernels (vertical over rows, horizontal over
  // columns). The vertical fills the gaps between scanlines; the horizontal gives
  // the beam a real width too, so a smooth spot reconstructs the line instead of a
  // bare point splitting its charge across two columns (which leaves a faint
  // sampling-beat stripe). The horizontal sigma defaults to the vertical, so the
  // spot is round unless beam_sigma_cols is set. The weights depend only on the
  // beam centre's sub-pixel fraction, so each axis tabulates per fraction bin.
  // beam_sigma is in scanline pitches (the raster's per-field line spacing on
  // the output), so the spot keeps its physical size whatever the output
  // height or overscan window: pitch_rows = y_scale_ / lines-per-field.
  const double lines_per_field = cfg_.nominal_line_hz / cfg_.field_hz;
  const double pitch_rows = y_scale_ / lines_per_field;
  const double sigma_rows = cfg_.beam_sigma * pitch_rows;
  const double sigma_cols = cfg_.beam_sigma_cols < 0.0 ? sigma_rows : cfg_.beam_sigma_cols;
  // Focus classes: class 0 is nominal focus, the last is the spot at full EHT
  // sag (grown by eht_focus). One class when the mechanism is off.
  const std::size_t classes = (cfg_.eht_sag > 0.0 && cfg_.eht_focus > 0.0) ? kFocusClasses : 1;
  const auto build = [&](double sigma) {
    SplatLut l;
    l.radius = dsp::splat_radius_for(sigma);
    l.stride = 2 * l.radius + 1;
    l.lut = dsp::gaussian_splat_lut(sigma, l.radius, kGaussBins);
    return l;
  };
  for (std::size_t c = 0; c < classes; ++c) {
    const double grow = classes == 1 ? 1.0 : 1.0 + cfg_.eht_focus * static_cast<double>(c) / (kFocusClasses - 1);
    y_classes_.push_back(build(sigma_rows * grow));
    x_classes_.push_back(build(sigma_cols * grow));
  }
  splat_radius_y_ = y_classes_[0].radius;
  splat_radius_x_ = x_classes_[0].radius;
  gauss_stride_y_ = y_classes_[0].stride;
  gauss_stride_x_ = x_classes_[0].stride;
  gauss_lut_y_ = y_classes_[0].lut.data();
  gauss_lut_x_ = x_classes_[0].lut.data();
  limiting_ = cfg_.bcl_threshold > 0.0;
  // The line-load sensor runs for any consumer: the EHT, the pull or a limiter.
  loading_ = cfg_.eht_sag > 0.0 || cfg_.line_pull > 0.0 || limiting_;
  // The per-line effective mapping starts unloaded (eht_ = 1, no pull).
  x_scale_eff_ = x_scale_;
  x_off_eff_ = -cfg_.h_window_lo * x_scale_;
  y_scale_eff_ = y_scale_;
  y_off_eff_ = -cfg_.v_window_lo * y_scale_;
  tilt_eff_ = yoke_tilt_rows_;
  // Gun gamma table: drive^gamma sampled over [0, kGunDriveMax], read with linear
  // interpolation. Linear at gamma 1.0, so leave the table empty and skip the pow.
  if (cfg_.gamma != 1.0) {
    gun_lut_.resize(kGunBins + 1);
    for (std::size_t i = 0; i <= kGunBins; ++i) {
      const double drive = static_cast<double>(i) * (kGunDriveMax / static_cast<double>(kGunBins));
      gun_lut_[i] = static_cast<float>(std::pow(drive, cfg_.gamma));
    }
  }
}

// Phosphor bloom on an overdriven gun (drive above the table). Out of line and
// cold so the pow() never bloats gun()'s force-inlined hot path.
[[gnu::cold, gnu::noinline]] float Screen::gun_bloom(double drive) const {
  return static_cast<float>(std::pow(drive, cfg_.gamma));
}

// One electron gun: drive^gamma, the beam-current curve. 0 stays 0 (cutoff). The
// curve is tabulated over [0, kGunDriveMax] (built for cfg_.gamma); above the
// table, phosphor bloom takes the cold pow() path. Force-inlined: it's four calls
// per active sample on the deposit's critical path, so the call overhead and the
// double spill across it matter more than the code size.
[[gnu::always_inline]] inline float Screen::gun(double drive) const {
  if (!(drive > 0.0)) // <= 0 and NaN cut off
    return 0.0f;
  if (gun_lut_.empty()) // gamma == 1.0
    return static_cast<float>(drive);
  if (!(drive < kGunDriveMax)) // over the table: phosphor bloom takes the cold pow path
    return gun_bloom(drive);
  const double t = drive * (static_cast<double>(kGunBins) / kGunDriveMax);
  const auto i = static_cast<std::size_t>(t);
  const auto f = static_cast<float>(t - static_cast<double>(i));
  return gun_lut_[i] + f * (gun_lut_[i + 1] - gun_lut_[i]);
}

void Screen::prepare(std::size_t) {}

namespace {
// The back porch — the blanking shelf just after the line-sync pulse (h_phase=0
// is the sync leading edge) — sits at black every line, a clean reference for
// DC restoration. As an h_phase window: sync is ~0..0.07, the back porch ~0.09.
constexpr float kBackPorchLo = 0.09f;
constexpr float kBackPorchHi = 0.14f;
constexpr double kBlackTrack = 0.02; // black-reference tracking per back-porch sample
} // namespace

float Screen::drive_of(float luma_f, float h_phase) {
  // luma arrives in negative-modulation envelope units (lower = whiter, see
  // ChromaSample). The electron gun (see Screen::black_): DC-restore black from
  // the back-porch window, then drive = black - luma, cut off below black. Linear
  // (no gamma — the per-gun gamma is applied after the colour matrix). No upper bound.
  const auto luma = static_cast<double>(luma_f);
  if (!seeded_) {
    black_ = luma;
    seeded_ = true;
  }
  if (h_phase >= kBackPorchLo && h_phase < kBackPorchHi)
    black_ += kBlackTrack * (luma - black_);
  return static_cast<float>(std::max(0.0, black_ - luma));
}

namespace {
// BT.601 colour-difference -> RGB, in gun-drive space (luma drive is the Y term).
// These match the TDA3561A demodulator ratios: (B-Y)/(R-Y) = kBu/kRv = 1.78, and
// the (G-Y) axis within the datasheet's tolerance (see docs/TDA3561A.md).
constexpr double kRv = 1.140;
constexpr double kGu = -0.395;
constexpr double kGv = -0.581;
constexpr double kBu = 2.032;
} // namespace

// A new line begins: finalize the completed line's beam load, integrate the
// EHT, and refresh the per-line effective mapping (deflection growth about the
// picture centre, the previous line's width pull, the focus class and the
// brightness factor). Runs at line rate, keyed off the per-sample line_start
// flag, so it is chunking- and thread-invariant like the rest of the stage.
void Screen::start_line() {
  const double load = line_load_n_ > 0 && white_ref_ > 0.0
                          ? std::min(1.5, static_cast<double>(line_load_sum_) /
                                              (static_cast<double>(line_load_n_ * channels_) * white_ref_))
                          : 0.0;
  line_load_sum_ = 0.0f;
  line_load_n_ = 0;
  prev_line_load_ = load;

  if (limiting_) {
    // Beam-current limiting: smooth the load, then INTEGRAL feedback on the
    // gain — the loop contains the limiter (the sensor sees limited drive),
    // so this settles the measured average exactly at the threshold, as a
    // real BCL's negative feedback into the contrast stage does.
    if (cfg_.bcl_threshold > 0.0) {
      const double alpha = 1.0 / (cfg_.bcl_tc_fields * cfg_.nominal_line_hz / cfg_.field_hz);
      bcl_state_ += alpha * (load - bcl_state_);
      const double err = (bcl_state_ - cfg_.bcl_threshold) / cfg_.bcl_threshold;
      bcl_gain_ = std::clamp(bcl_gain_ * (1.0 - 0.05 * err), 0.05, 1.0);
    }
    video_gain_ = bcl_gain_;
  }

  if (cfg_.eht_sag <= 0.0 && cfg_.line_pull <= 0.0)
    return;

  double defl = 1.0;
  if (cfg_.eht_sag > 0.0) {
    const double lines_per_field = cfg_.nominal_line_hz / cfg_.field_hz;
    const double alpha = 1.0 / (cfg_.eht_tc_fields * lines_per_field);
    eht_ += alpha * ((1.0 - cfg_.eht_sag * load) - eht_);
    defl = 1.0 / std::sqrt(eht_); // deflection sensitivity rises as EHT sags
    bright_eff_ = static_cast<float>(eht_); // light ∝ V·I
    // Focus: the spot grows toward the full-sag class as the EHT sags.
    if (y_classes_.size() > 1) {
      const double sag = std::clamp((1.0 - eht_) / cfg_.eht_sag, 0.0, 1.0);
      const auto c = static_cast<std::size_t>(sag * static_cast<double>(y_classes_.size() - 1) + 0.5);
      splat_radius_y_ = y_classes_[c].radius;
      splat_radius_x_ = x_classes_[c].radius;
      gauss_stride_y_ = y_classes_[c].stride;
      gauss_stride_x_ = x_classes_[c].stride;
      gauss_lut_y_ = y_classes_[c].lut.data();
      gauss_lut_x_ = x_classes_[c].lut.data();
    }
  }
  const double pull = 1.0 + cfg_.line_pull * prev_line_load_;

  // Fold the growth (about the frame centre) into the same mul-add shape the
  // deposit uses: x = (h - off)*xs + xo and y = v*ys - tilt*h + yo.
  const double cx = 0.5 * static_cast<double>(cfg_.width);
  const double cy = 0.5 * static_cast<double>(cfg_.height);
  const double xs = x_scale_ * defl * pull;
  x_scale_eff_ = xs;
  x_off_eff_ = -cfg_.h_window_lo * xs + cx * (1.0 - defl * pull);
  const double ys = y_scale_ * defl;
  y_scale_eff_ = ys;
  y_off_eff_ = -cfg_.v_window_lo * ys + cy * (1.0 - defl);
  tilt_eff_ = yoke_tilt_rows_ * defl;
}

void Screen::process(std::span<const ChromaSample> picture, std::span<const BeamSample> hbeam,
    std::span<const VSample> vbeam, const FieldCallback &on_field) {
  const std::size_t n = std::min({picture.size(), hbeam.size(), vbeam.size()});
  for (std::size_t i = 0; i < n; ++i) {
    // A field boundary closes off a completed field: the buffer IS the displayed
    // image (no per-pixel fade), so snapshot it, then fade the whole phosphor by
    // one field period before the next field accumulates on top. Block-size
    // independent — field_start is per-sample, so this lands identically however
    // the input is chunked.
    //
    // The fade is per FIELD, not per frame, which leaves a latent interlace comb:
    // the two fields paint alternate lines a field apart, so at readout one
    // field's lines have faded once relative to the other (a 3:1 line-to-line
    // ripple at persistence 0.9). It's invisible only because the beam splat
    // (beam_sigma) is wide enough to spread each line's charge across its
    // neighbours, blending the fields. Shrink the beam past that and the comb
    // shows. The fix is to fade once per FRAME (every two fields) so both fields
    // paint before any decay — which needs the decay/snapshot aligned to a frame
    // boundary, i.e. the interlace even/odd parity tracking still to be added.
    if (vbeam[i].field_start) {
      if (on_field)
        on_field(FieldEvent{*this});
      for (float &b: bright_)
        b *= field_decay_;
    }
    if (hbeam[i].line_start)
      start_line();

    // Gun drives: luma sets the Y term; the matrix adds the colour difference.
    // Each gun cuts off at zero, then takes its gamma. Grey = the luma gun alone.
    // The limiter is the contrast control: it scales the drive ahead of the
    // gun (video_gain_ is exactly 1.0 when no limiter is enabled, and the
    // chroma follows automatically through the white_drive_ reference).
    const double drive = video_gain_ * static_cast<double>(drive_of(picture[i].luma, hbeam[i].h_phase));
    const float luma_gun = gun(drive);
    // IF AGC: track the peak luma gun (readout white) and the peak luma drive (the
    // chroma reference), fast up, slow down. Tracking drive ties the colour to the
    // luma scale: U/V arrive normalised to the burst, so scaling them by the white
    // drive makes saturation a bounded fraction of white, not a free multiplier.
    white_ref_ = std::max(static_cast<double>(luma_gun), white_ref_ * agc_release_);
    white_drive_ = std::max(drive, white_drive_ * agc_release_);
    // Retrace blanking: the beam is off through sync, back porch and burst, the
    // line-blanking pulse of a real set. This is why the burst — a big subcarrier
    // sitting in the back porch — never paints a coloured bar; it's blanked, not
    // gated on luma.
    if (hbeam[i].h_phase < static_cast<float>(cfg_.h_blank))
      continue;

    // R/G/B guns: luma drive plus the colour difference, each cut off then gamma'd.
    float gun_rgb[3] = {luma_gun, 0.0f, 0.0f};
    if (channels_ == 3) {
      const double cs = cfg_.saturation * white_drive_; // chroma referenced to white
      const double u = cs * static_cast<double>(picture[i].u);
      const double v = cs * static_cast<double>(picture[i].v);
      gun_rgb[0] = gun(drive + kRv * v);
      gun_rgb[1] = gun(drive + kGu * u + kGv * v);
      gun_rgb[2] = gun(drive + kBu * u);
    }

    // The beam current is this line's load on the EHT supply (summed before
    // the brightness factor — sagging volts dim the light, not the current);
    // light ∝ V·I applies the per-line EHT factor to the deposit. Skipped
    // entirely when no loading mechanism is enabled: the off path costs zero.
    if (loading_) {
      line_load_sum_ += gun_rgb[0] + gun_rgb[1] + gun_rgb[2];
      ++line_load_n_;
      gun_rgb[0] *= bright_eff_;
      gun_rgb[1] *= bright_eff_;
      gun_rgb[2] *= bright_eff_;
    }

    // Horizontal beam position. The spot has a real width, so the deposit spreads
    // over the columns under the Gaussian, centred on the sub-pixel position; the
    // normalised weights depend only on xf's fractional part, so look up the bin.
    // picture_h_offset_ shifts the picture back onto the sweep to register colour
    // with mono (sync timing — blanking, yoke — still uses the raw h_phase).
    const double xf = (static_cast<double>(hbeam[i].h_phase) - picture_h_offset_) * x_scale_eff_ + x_off_eff_;
    const double xf_floor = std::floor(xf);
    const auto base_col = static_cast<std::ptrdiff_t>(xf_floor) - static_cast<std::ptrdiff_t>(splat_radius_x_);
    auto bin_x = static_cast<std::size_t>((xf - xf_floor) * static_cast<double>(kGaussBins));
    if (bin_x >= kGaussBins)
      bin_x = kGaussBins - 1;
    const auto *cweights = &gauss_lut_x_[bin_x * gauss_stride_x_];

    // Yoke shear: un-creep the vertical so the scanline is flat (see header).
    const auto yc = static_cast<double>(vbeam[i].v_phase) * y_scale_eff_ -
                    tilt_eff_ * static_cast<double>(hbeam[i].h_phase) + y_off_eff_;

    // Gaussian beam spot, centred on the sheared sub-pixel row. The normalised
    // weights depend only on yc's fractional part, so look up the matching bin.
    const double yc_floor = std::floor(yc);
    const auto base = static_cast<std::ptrdiff_t>(yc_floor) - static_cast<std::ptrdiff_t>(splat_radius_y_);
    auto bin = static_cast<std::size_t>((yc - yc_floor) * static_cast<double>(kGaussBins));
    if (bin >= kGaussBins)
      bin = kGaussBins - 1;
    const auto *rweights = &gauss_lut_y_[bin * gauss_stride_y_];

    // Splat the round spot (rows × columns), ADDING charge (the whole-buffer fade
    // at the field boundary handles decay). The beam hits all channels of a pixel
    // at the same instant. Row and column weights each sum to 1, so the separable
    // product conserves the sample's total deposited charge.
    // Clip the column span to the frame once (base_col and the stride don't change
    // per row), so the inner loop is branchless over the ~49-cell spot.
    const auto width = static_cast<std::ptrdiff_t>(cfg_.width);
    const std::ptrdiff_t j_lo = std::max<std::ptrdiff_t>(0, -base_col);
    const std::ptrdiff_t j_hi = std::min(static_cast<std::ptrdiff_t>(gauss_stride_x_), width - base_col);
    for (std::size_t k = 0; k < gauss_stride_y_; ++k) {
      const std::ptrdiff_t row = base + static_cast<std::ptrdiff_t>(k);
      if (row < 0 || row >= static_cast<std::ptrdiff_t>(cfg_.height))
        continue;
      const auto rw = rweights[k];
      const auto row_base = static_cast<std::size_t>(row) * cfg_.width;
      for (std::ptrdiff_t j = j_lo; j < j_hi; ++j) {
        const auto pixel = row_base + static_cast<std::size_t>(base_col + j);
        const auto w = rw * cweights[j];
        // channels_ is 1 or 3; spell both out so the guns stay in registers and
        // the RGB triple isn't a variable-trip loop.
        float *cell = &bright_[pixel * channels_];
        cell[0] += gun_rgb[0] * w;
        if (channels_ == 3) {
          cell[1] += gun_rgb[1] * w;
          cell[2] += gun_rgb[2] * w;
        }
      }
    }
  }
}

Screen::Frame Screen::quantise(const std::vector<float> &bright, double white_ref) const {
  // Scale by the AGC white reference (the steady-state phosphor brightness a
  // tracked-white pixel reaches), one shared scale so hue is preserved. Contrast
  // moves the white point; cells above it clip into white, as a real tube does —
  // no per-frame statistic, so the exposure is causal and doesn't breathe.
  const double white = white_ref * phosphor_gain_;
  std::vector<std::uint8_t> pixels(bright.size());
  if (cfg_.readout_gamma == 1.0) {
    // Linear readout: the raw phosphor light, straight scale-and-quantise (the
    // buffer already holds the displayed charge; decay is applied per field).
    const float scale = white > 0.0 ? static_cast<float>(255.0 * cfg_.contrast / white) : 0.0f;
    for (std::size_t idx = 0; idx < bright.size(); ++idx)
      pixels[idx] = static_cast<std::uint8_t>(std::clamp(bright[idx] * scale, 0.0f, 255.0f) + 0.5f);
  }
  else {
    // The "camera": encode the linear light for a display that will decode it
    // with readout_gamma, so the viewer sees the phosphor's light and not a
    // double-gamma'd version. Once per emitted frame, not per sample.
    const float scale = white > 0.0 ? static_cast<float>(cfg_.contrast / white) : 0.0f;
    const auto inv = static_cast<float>(1.0 / cfg_.readout_gamma);
    for (std::size_t idx = 0; idx < bright.size(); ++idx) {
      const float lit = std::clamp(bright[idx] * scale, 0.0f, 1.0f);
      pixels[idx] = static_cast<std::uint8_t>(255.0f * std::pow(lit, inv) + 0.5f);
    }
  }
  return Frame{.pixels = std::move(pixels), .width = cfg_.width, .height = cfg_.height, .channels = channels_};
}

Screen::Frame Screen::snapshot() const { return quantise(bright_, white_ref_); }

void Screen::latch_boundary() {
  latch_bright_.assign(bright_.begin(), bright_.end());
  latch_white_ = white_ref_;
}

Screen::Frame Screen::latched_frame() const {
  if (latch_white_ < 0.0)
    return snapshot();
  return quantise(latch_bright_, latch_white_);
}

} // namespace palindrome::video
