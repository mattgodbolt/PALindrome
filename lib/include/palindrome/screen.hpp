#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/splat.hpp"
#include "palindrome/video_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
  // standard deviation. beam_sigma is the vertical extent in SCANLINE PITCHES
  // (the spacing between a field's lines on the output raster) — the spot is a
  // property of the tube and the raster, so it must not change size when the
  // output height or the overscan window does. ~0.5 fills the gaps between
  // scanlines (the focus). beam_sigma_cols is the horizontal extent in output
  // columns and defaults to match the derived row sigma (a round spot) when
  // negative. 0 => a single pixel (no splat) on that axis.
  double beam_sigma = 0.43;
  double beam_sigma_cols = -1.0; // < 0 => match the derived row sigma (round spot)
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
  // Contrast: the video-amplifier gain ahead of the gun - the front-panel pot.
  // With absolute levels (tracked_white = false) it scales the gun drive
  // (pre-gamma) and the chroma with it; 1.0 maps a broadcast-standard
  // full-white drive to the readout white point, higher pushes whites toward
  // the peak-white limiter and clipping, exactly as turning the pot up does.
  // In tracked_white mode it keeps its old meaning: the readout white point as
  // a fraction of the tracked peak (the autocontrast's only remaining knob).
  double contrast = 1.0;
  // Levels. The default is the period scheme: the front-end AGC holds the sync
  // tip at 1.0, black is the clamped back porch, and WHITE IS GEOMETRY - System
  // I puts blanking at 0.76 and peak white at 0.20 of the tip, so a standard
  // full-white line drives the gun by a known 0.56 and the readout white point
  // is a constant. A source that under-modulates (console RF modulators)
  // renders dim, and the contrast pot - not an automatic - brings it back.
  // tracked_white = true restores the modern convenience this replaced:
  // fast-attack/slow-release peak trackers that stretch whatever arrives to
  // full scale (an autocontrast no real set had).
  bool tracked_white = false;
  // Peak-white limiter (the TDA3561A's): when any gun's drive exceeds
  // pwl_threshold × the standard white drive, the contrast is pulled down fast
  // until it doesn't, recovering slowly when clear - and its engagement is
  // delayed by one line, so a single bright line (an abrupt colour-to-white
  // test pattern) never triggers it. Needs absolute levels: ignored when
  // tracked_white (no absolute reference to limit against). 0 disables.
  double pwl_threshold = 1.25;
  // Readout transfer: the "camera" between the phosphor and the PNG. The
  // framebuffer is linear light; a PNG viewed on an sRGB display gets the
  // monitor's ~2.2 decode applied, so an authentic tube gamma needs the
  // readout to ENCODE with this exponent (pixel = light^(1/readout_gamma)) or
  // the picture is gamma'd twice. 1.0 = linear (the original raw readout);
  // 2.2 with a ~2.6 tube gives the broadcast chain's deliberate end-to-end
  // system gamma of ~1.2.
  double readout_gamma = 1.0;
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
  // The window of the scan mapped onto the output frame, in h_phase / v_phase.
  // [0,1]x[0,1] shows the whole scan, blanking and all. A real set overscans:
  // the raster is larger than the tube window, so the bezel hides the blanking
  // and a few percent of the picture — the driver expresses that by mapping the
  // (cropped) nominal active box here. Pure remap: deposits outside the window
  // fall to the existing frame clip, so it costs nothing per sample.
  double h_window_lo = 0.0;
  double h_window_hi = 1.0;
  double v_window_lo = 0.0;
  double v_window_hi = 1.0;
  // Beam loading — the places where picture content loads the set. The beam is
  // the EHT supply's load: a bright picture drains the aquadag reservoir faster
  // than the flyback tops it up, the final-anode voltage sags by eht_sag of its
  // unloaded value (at a sustained full-white load) with a time constant of
  // eht_tc_fields field periods, and three things follow, all period-visible:
  // the raster GROWS about its centre (deflection ∝ 1/sqrt(EHT) — the breathing
  // on scene cuts), the picture dims slightly (light ∝ V·I), and the spot
  // defocuses (the focus electrode tracks EHT imperfectly; eht_focus is the
  // fractional spot growth at full sag). Separately, line_pull is the
  // line-rate loading of the line-output stage: a line carrying a lot of white
  // scans slightly wider, so vertical edges bend next to bright content
  // (line_pull = fractional width stretch after a full-white line).
  // eht_sag = 0 disables the EHT mechanism; line_pull = 0 disables the pull.
  double eht_sag = 0.0;
  double eht_tc_fields = 2.0;
  double eht_focus = 0.3;
  double line_pull = 0.0;
  // Beam-current limiting: a real set sensed the average beam current in the
  // EHT return and pulled the video gain (the contrast) down when it exceeded
  // a safe level — a sustained bright scene dims to protect the tube and
  // supply. When the smoothed line load exceeds bcl_threshold (as a fraction
  // of a full-white load), the gun drive is scaled so the average settles at
  // the threshold, with bcl_tc_fields response. 0 disables.
  double bcl_threshold = 0.0;
  double bcl_tc_fields = 0.5;
  // Deposit threads. The per-sample control pass records one splat per visible
  // sample into a buffer; at each field boundary the field's splats are applied
  // to the phosphor, fanned across this many threads by output band (see
  // SplatDeposit). Bit-exact in the thread count - the picture never changes -
  // so this is a wall-clock knob only. 1 = serial (no pool). The caller sets it.
  std::size_t deposit_lanes = 1;
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

  // Handed to the field callback at each boundary. Quantising a Frame is the
  // expensive part of a snapshot, so the event is lazy: frame() quantises the
  // field now (the per-field PNG sequence), while latch() just copies the float
  // state aside so latched_frame() can quantise it once, later — the
  // single-image "keep the last clean boundary" shape, which would otherwise
  // quantise and discard every field but the last.
  //
  // The event is a view of the live screen, only meaningful synchronously
  // inside the callback: once it returns, the boundary fade (and further
  // deposits) move the state on, so frame()/latch() from a stashed event would
  // silently read the wrong instant. Non-copyable to keep that impossible.
  class FieldEvent {
  public:
    FieldEvent(const FieldEvent &) = delete;
    FieldEvent &operator=(const FieldEvent &) = delete;
    [[nodiscard]] Frame frame() const { return screen_->snapshot(); }
    void latch() const { screen_->latch_boundary(); }

  private:
    friend class Screen;
    explicit FieldEvent(Screen &screen) : screen_{&screen} {}
    Screen *screen_;
  };

  // on_field fires once per field boundary (the field_start sample), with the
  // phosphor as completed at that instant — the hook for a per-field PNG
  // sequence. (Field, not frame, until even/odd parity tracking lands with
  // colour; then this can fire per full frame.) Empty by default = no snapshots.
  using FieldCallback = std::function<void(const FieldEvent &)>;
  void process(std::span<const ChromaSample> picture, std::span<const BeamSample> hbeam, std::span<const VSample> vbeam,
      const FieldCallback &on_field = {});

  // Read the phosphor out, scaled by the white reference (one shared scale across
  // R/G/B so hue is preserved). The deposit is batched per field, so this first
  // materialises any splats buffered since the last field boundary into the
  // phosphor - a lazy finalise of work already committed, so snapshotting still
  // doesn't disturb the running sim and stays const.
  [[nodiscard]] Frame snapshot() const;

  // Diagnostic: the per-unit EHT (1 = unloaded; sustained full white sags it
  // toward 1 - eht_sag).
  [[nodiscard]] double eht() const noexcept { return eht_; }
  // Diagnostic: the combined gain the limiters are applying (1 = none). The
  // contrast pot is excluded - this reports protection, not the user's setting.
  [[nodiscard]] double limiter_gain() const noexcept { return bcl_gain_ * pwl_gain_; }

  // The frame as latched by the most recent FieldEvent::latch(): the phosphor
  // and white reference as they stood at that boundary, quantised now. Falls
  // back to snapshot() (the live state) if nothing was ever latched — e.g. a
  // stream too short to reach a field boundary.
  [[nodiscard]] Frame latched_frame() const;

private:
  // luma envelope -> beam drive (gun cutoff). h_phase locates the back porch for
  // the black DC-restore. The colour matrix then derives the three gun drives.
  [[nodiscard]] float drive_of(float luma, float h_phase);

  ScreenConfig cfg_;
  std::size_t channels_; // 1 (grey) or 3 (RGB), from cfg_.colour
  // The white references. Absolute mode (the default): both are constants from
  // the System I geometry, set once in the ctor - the readout white point and
  // the chroma reference never move with picture content. Tracked mode: a
  // fast-attack, slow-release peak tracker on the luma gun drive, advanced
  // every sample (the modern autocontrast the absolute scheme replaced).
  bool tracking_; // cfg_.tracked_white (set in the ctor)
  double white_ref_ = 0.0; // peak luma gun output mapped to readout white
  double white_drive_ = 0.0; // peak luma drive (chroma reference, pre-gamma)
  double agc_release_; // tracked mode: per-sample release factor (multi-field)
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
  // mutable: the phosphor is the readout, but the deposit into it is batched per
  // field and materialised lazily by flush() (called from the const snapshot), so
  // the charge committed by the splats is logically already part of the picture.
  mutable std::vector<float> bright_; // per-pixel-per-channel accumulated phosphor charge
  float field_decay_ = 1.0f; // whole-buffer multiply applied once per field

  // Deferred deposit. The per-sample control pass records one SplatRecord per
  // visible sample into splats_, accumulating across blocks within a field; at
  // each field boundary (and lazily on any read) flush() hands the whole field to
  // deposit_, which fans the splats across its threads (by output band) into
  // bright_. Batching to per-field is what lets the apply be threaded: the
  // fan/join cost is paid ~once per field, not once per block, and a full field
  // spans every row so all the deposit threads have work at once.
  mutable Buffer<SplatRecord> splats_; // this field's pending splats (accumulated across blocks)
  std::unique_ptr<SplatDeposit> deposit_; // applies a field of splats, threaded by band (built once the LUTs exist)
  void flush() const; // materialise splats_ into bright_, then clear (idempotent)

  // FieldEvent::latch() support: the phosphor and white reference copied aside
  // at a field boundary (a float memcpy — far cheaper than quantising a Frame
  // per field when only the last one is kept). latched_white_ < 0 = never.
  void latch_boundary();
  [[nodiscard]] Frame quantise(const std::vector<float> &bright, double white_ref) const;
  std::vector<float> latch_bright_;
  double latch_white_ = -1.0;

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
  double x_scale_ = 0.0; // columns per unit h_phase: width / (h_window_hi - h_window_lo)
  double y_scale_ = 0.0; // rows per unit v_phase: height / (v_window_hi - v_window_lo)
  // The beam spot is a separable 2-D Gaussian: a vertical kernel over rows and a
  // horizontal one over columns, each normalised to sum 1. Both depend only on
  // the spot centre's sub-pixel fraction along that axis, so each is tabulated per
  // fraction bin (a [bin][cell] table) instead of calling exp() per sample.
  // EHT focus softening quantises the spot into kFocusClasses sizes, one table
  // set per class; the active class is chosen per LINE (the EHT moves on field
  // timescales) and recorded on each splat so the deferred apply picks the right
  // kernel. The tables themselves are handed to deposit_ as SplatKernels.
  static constexpr std::size_t kGaussBins = 4096;
  static constexpr std::size_t kFocusClasses = 8;
  struct SplatLut {
    std::size_t radius;
    std::size_t stride; // 2*radius + 1
    std::vector<float> lut; // kGaussBins * stride, normalised
  };
  std::vector<SplatLut> y_classes_; // class 0 = nominal focus, last = full sag
  std::vector<SplatLut> x_classes_;
  std::size_t active_class_ = 0; // the focus class chosen for the current line

  // Beam loading (see ScreenConfig). The per-line mean gun output, normalised
  // by the AGC white, is the load; eht_ integrates it at line rate (a slow
  // cross-sample accumulator, hence double). The per-line effective mapping
  // below folds the deflection growth (1/sqrt(eht_)) and the previous line's
  // width pull into the same mul-add shape the per-sample deposit already
  // uses, so the hot loop is unchanged.
  double eht_ = 1.0; // per-unit final-anode voltage; 1 = unloaded
  bool loading_ = false; // any beam-loading mechanism enabled (set in the ctor)
  // The line accumulator is float, not double: it sums one line (~1000 samples,
  // values O(1)), so float error is ~1e-4 relative — and the per-sample double
  // converts plus a serial double add chain cost ~9% of the deposit. The
  // cross-line accumulator (eht_) stays double per the precision rule.
  float line_load_sum_ = 0.0f;
  std::size_t line_load_n_ = 0;
  double prev_line_load_ = 0.0; // last completed line's mean load (drives the pull)
  double x_scale_eff_ = 0.0; // per-line mapping: x = (h - off) * x_scale_eff_ + x_off_eff_
  double x_off_eff_ = 0.0;
  double y_scale_eff_ = 0.0; // per-line mapping: y = v * y_scale_eff_ - tilt_eff_ * h + y_off_eff_
  double y_off_eff_ = 0.0;
  double tilt_eff_ = 0.0;
  float bright_eff_ = 1.0f; // light ∝ V·I: the per-line EHT brightness factor
  // Beam-current limiting (see ScreenConfig): bcl_state_ smooths the per-line
  // load and bcl_gain_ integrates against the threshold. The peak-white
  // limiter senses the per-line peak gun drive (post-matrix, pre-gamma) and
  // pulls its own gain when the delayed-engagement condition holds. Both act
  // through video_gain_ - the contrast stage - alongside the pot itself:
  // video_gain_ = contrast_gain_ × bcl_gain_ × pwl_gain_, refreshed per line.
  bool limiting_ = false; // the beam-current limiter enabled (set in the ctor)
  double bcl_state_ = 0.0; // smoothed average load
  double bcl_gain_ = 1.0; // BCL gain: integral feedback, settles measured load AT the threshold
  bool pwl_on_ = false; // peak-white limiter enabled (absolute levels only)
  double pwl_level_ = 0.0; // drive ceiling: pwl_threshold × standard white drive
  double pwl_gain_ = 1.0; // PWL gain: pulled fast while over, slow recovery
  bool pwl_armed_ = false; // previous line exceeded (the one-line delay)
  double pwl_line_peak_ = 0.0; // this line's peak gun drive
  double contrast_gain_ = 1.0; // the pot: cfg_.contrast (absolute), 1 (tracked)
  double video_gain_ = 1.0; // the per-line gain applied to the drive
  void start_line(); // finalize the line load, update eht_, refresh the mapping

  // The electron-gun curve drive^gamma is the last un-LUT'd per-sample
  // transcendental; tabulate it over the drive domain (built once for cfg_.gamma)
  // with linear interpolation, falling back to pow() above the table for bloom.
  static constexpr std::size_t kGunBins = 8192;
  static constexpr double kGunDriveMax = 8.0;
  std::vector<float> gun_lut_; // pow(drive, gamma) over [0, kGunDriveMax]; empty when gamma == 1
  // gun() is the hot per-sample gamma lookup, force-inlined into the deposit (it's
  // called four times per active sample); the rare over-table bloom takes the cold
  // pow() path in gun_bloom(), kept out of line so it doesn't bloat the hot path.
  [[nodiscard]] float gun(double drive) const;
  [[nodiscard]] float gun_bloom(double drive) const;

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
