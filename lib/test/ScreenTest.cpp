#include "palindrome/horizontal_sweep.hpp"
#include "palindrome/screen.hpp"
#include "palindrome/sync_separator.hpp"
#include "palindrome/vertical_sync.hpp"
#include "palindrome/video_types.hpp"

#include "VideoSynth.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace video = palindrome::video;
using videotest::kRate;
using videotest::synth_composite;

TEST_CASE("Screen is block-invariant") {
  const auto env = synth_composite(40, 1028);

  // Pre-compute the two timing rails once so the test isolates the Screen.
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  sep.prepare(env.size());
  const std::span<const video::SyncSample> sync = sep.process(env);
  std::vector<video::SyncSample> sync_copy{sync.begin(), sync.end()};

  video::HorizontalSweep hsweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  hsweep.prepare(sync_copy.size());
  std::vector<video::BeamSample> hbeam;
  {
    const auto b = hsweep.process(sync_copy);
    hbeam.assign(b.begin(), b.end());
  }
  video::VerticalSync vsync{video::VerticalSyncConfig{.sample_rate_hz = kRate}};
  vsync.prepare(sync_copy.size());
  std::vector<video::VSample> vbeam;
  {
    const auto v = vsync.process(sync_copy);
    vbeam.assign(v.begin(), v.end());
  }

  const video::ScreenConfig cfg{.width = 320, .height = 64, .sample_rate_hz = kRate};

  // The screen now joins on the chroma rail; wrap the envelope as luma-only.
  std::vector<video::ChromaSample> pic(env.size());
  for (std::size_t i = 0; i < env.size(); ++i)
    pic[i] = video::ChromaSample{.luma = env[i]};

  video::Screen whole{cfg};
  whole.process(pic, hbeam, vbeam);
  const auto frame_whole = whole.snapshot();

  video::Screen chunked{cfg};
  constexpr std::size_t chunk = 257; // ragged blocks straddle line and field boundaries
  for (std::size_t off = 0; off < pic.size(); off += chunk) {
    const std::size_t n = std::min(chunk, pic.size() - off);
    chunked.process(std::span{pic}.subspan(off, n), std::span{hbeam}.subspan(off, n), std::span{vbeam}.subspan(off, n));
  }
  const auto frame_chunked = chunked.snapshot();

  REQUIRE(frame_whole.pixels.size() == frame_chunked.pixels.size());
  for (std::size_t i = 0; i < frame_whole.pixels.size(); ++i)
    CHECK(frame_whole.pixels[i] == frame_chunked.pixels[i]);
}

TEST_CASE("Screen snapshots a field before fading it, exactly once per retrace") {
  // persistence 2 fields => a per-field fade of exp(-1/persistence); gamma 1 keeps
  // the gun linear so the deposit is simple to reason about.
  const double persistence = 2.0;
  // contrast 0.5 keeps the full-white deposit below the absolute white point
  // (gun drive 0.5 < the System I 0.56), so no pixel clips and the fade is
  // measurable in the quantised values.
  const video::ScreenConfig cfg{.width = 8,
      .height = 8,
      .sample_rate_hz = kRate,
      .persistence_fields = persistence,
      .beam_sigma = 0.0,
      .gamma = 1.0,
      .contrast = 0.5};
  const float field_decay = static_cast<float>(std::exp(-1.0 / persistence));

  video::Screen screen{cfg};

  // Field 0: a back-porch sample seeds the black reference, then active white
  // samples paint a few pixels. The gun stays driven at a constant level the whole
  // time, so the AGC white reference is pinned and the readout scale is identical
  // across the snapshots below — any change in pixel value is purely the fade.
  std::vector<video::ChromaSample> pic;
  std::vector<video::BeamSample> hbeam;
  std::vector<video::VSample> vbeam;
  const auto add = [&](float h_phase, float v_phase, float luma) {
    pic.push_back(video::ChromaSample{.luma = luma});
    hbeam.push_back(video::BeamSample{.h_phase = h_phase});
    vbeam.push_back(video::VSample{.v_phase = v_phase});
  };
  add(0.10f, 0.0f, 1.0f); // back porch: seed black_ = 1.0
  for (const float vp: {0.25f, 0.5f, 0.75f})
    add(0.5f, vp, 0.0f); // active white: deposit gun drive 1.0 into a pixel
  screen.process(pic, hbeam, vbeam);

  const auto deposited = screen.snapshot(); // the un-faded charge
  REQUIRE(std::ranges::any_of(deposited.pixels, [](auto p) { return p > 0; }));

  // One retrace (v_phase wraps 0.75 -> 0.0), beam blanked (h_phase below
  // h_blank) but the gun still driven so white stays pinned. It must snapshot
  // BEFORE applying a single fade.
  video::Screen::Frame at_boundary;
  const std::array boundary_pic{video::ChromaSample{.luma = 0.0f}};
  const std::array boundary_hbeam{video::BeamSample{.h_phase = 0.0f}};
  const std::array boundary_vbeam{video::VSample{.v_phase = 0.0f}};
  screen.process(boundary_pic, boundary_hbeam, boundary_vbeam,
      [&](const video::Screen::FieldEvent &e) { at_boundary = e.frame(); });

  // Snapshot-before-fade: the boundary frame equals the pre-fade charge exactly.
  REQUIRE(at_boundary.pixels.size() == deposited.pixels.size());
  for (std::size_t i = 0; i < deposited.pixels.size(); ++i)
    CHECK(at_boundary.pixels[i] == deposited.pixels[i]);

  // Exactly one fade applied at the boundary: the buffer is now deposit*field_decay.
  const auto faded = screen.snapshot();
  for (std::size_t i = 0; i < deposited.pixels.size(); ++i) {
    const auto expected =
        static_cast<int>(std::lround(static_cast<double>(deposited.pixels[i]) * static_cast<double>(field_decay)));
    CHECK(std::abs(static_cast<int>(faded.pixels[i]) - expected) <= 1);
  }
}

TEST_CASE("Screen fades and emits on the free-running retrace - no sync detection needed") {
  // The bug this pins: with the fade keyed on DETECTED field starts, unlocked
  // input (cold start, a mistuned carrier during AFC pull-in) deposited with
  // the decay switched off and integrated to a white screen no real set could
  // show. The retrace is the flywheel's v_phase wrap, which free-runs: with no
  // field ever detected, the phosphor must still fade each field and the
  // field callback must still fire (the rolling picture).
  const video::ScreenConfig cfg{.width = 8,
      .height = 8,
      .sample_rate_hz = kRate,
      .persistence_fields = 1.0,
      .beam_sigma = 0.0,
      .gamma = 1.0,
      .contrast = 0.5};
  video::Screen screen{cfg};

  std::vector<video::ChromaSample> pic;
  std::vector<video::BeamSample> hbeam;
  std::vector<video::VSample> vbeam;
  const auto add_field = [&] {
    pic.clear();
    hbeam.clear();
    vbeam.clear();
    pic.push_back(video::ChromaSample{.luma = 1.0f}); // back porch: pin black_
    hbeam.push_back(video::BeamSample{.h_phase = 0.10f});
    vbeam.push_back(video::VSample{.v_phase = 0.01f});
    for (const float vp: {0.25f, 0.5f, 0.75f}) { // active white deposits
      pic.push_back(video::ChromaSample{.luma = 0.0f});
      hbeam.push_back(video::BeamSample{.h_phase = 0.5f});
      vbeam.push_back(video::VSample{.v_phase = vp});
    }
  };

  // The callback count is the pin: on the detection-gated code it is zero
  // (no field is ever detected), so the fade never ran either. Charge
  // convergence itself can't be asserted through the quantised readout - its
  // white normalisation tracks the buffer, so any steady state reads
  // full-scale whether or not the fade runs.
  std::size_t retraces = 0;
  const auto count = [&](const video::Screen::FieldEvent &) { ++retraces; };
  for (int field = 0; field < 12; ++field) {
    add_field();
    screen.process(pic, hbeam, vbeam, count); // v_phase wraps 0.75 -> 0.01 between calls
  }
  CHECK(retraces == 11); // one per wrap; none for the first partial field
  const auto frame = screen.snapshot();
  CHECK(std::ranges::any_of(frame.pixels, [](auto px) { return px > 0; })); // the frames carry the picture
}

TEST_CASE("Screen latch defers quantisation without changing the boundary frame") {
  const video::ScreenConfig cfg{.width = 8, .height = 8, .sample_rate_hz = kRate, .beam_sigma = 0.0, .gamma = 1.0};
  video::Screen screen{cfg};

  // Before any latch, latched_frame() falls back to the live snapshot.
  CHECK(screen.latched_frame().pixels == screen.snapshot().pixels);

  std::vector<video::ChromaSample> pic;
  std::vector<video::BeamSample> hbeam;
  std::vector<video::VSample> vbeam;
  const auto add = [&](float h_phase, float v_phase, float luma) {
    pic.push_back(video::ChromaSample{.luma = luma});
    hbeam.push_back(video::BeamSample{.h_phase = h_phase});
    vbeam.push_back(video::VSample{.v_phase = v_phase});
  };
  add(0.10f, 0.0f, 1.0f); // back porch: seed the black reference
  for (const float vp: {0.25f, 0.5f, 0.75f})
    add(0.5f, vp, 0.0f); // active white into three pixels
  screen.process(pic, hbeam, vbeam);

  // At the boundary, quantise now (frame) AND latch; then deposit more on top.
  // The latched frame must still equal the boundary-time quantisation exactly —
  // that equivalence is what lets the single-image driver defer the quantise.
  video::Screen::Frame at_boundary;
  pic.clear();
  hbeam.clear();
  vbeam.clear();
  add(0.10f, 0.0f, 1.0f); // the field boundary: v_phase wraps 0.75 -> 0.0
  add(0.5f, 0.125f, 0.0f); // post-boundary deposit, must not leak into the latch
  screen.process(pic, hbeam, vbeam, [&](const video::Screen::FieldEvent &e) {
    at_boundary = e.frame();
    e.latch();
  });

  const auto latched = screen.latched_frame();
  REQUIRE(latched.pixels.size() == at_boundary.pixels.size());
  for (std::size_t i = 0; i < latched.pixels.size(); ++i)
    CHECK(latched.pixels[i] == at_boundary.pixels[i]);
  CHECK(latched.pixels != screen.snapshot().pixels); // the post-boundary deposit is visible live
}

TEST_CASE("Screen scan windows remap the beam (overscan) and the readout can encode") {
  // A 0-sigma beam at h_phase 0.5 / v_phase 0.5 with the full-scan windows
  // lands mid-frame; with a [0.25, 0.75] window pair the same beam position is
  // still mid-window, and h_phase 0.375 lands at a quarter of the width.
  const auto run_one = [](const video::ScreenConfig &cfg, float hp, float vp) {
    video::Screen screen{cfg};
    const std::array pic{video::ChromaSample{.luma = 1.0f}, video::ChromaSample{.luma = 0.0f}};
    const std::array hbeam{video::BeamSample{.h_phase = 0.10f}, video::BeamSample{.h_phase = hp}};
    const std::array vbeam{video::VSample{.v_phase = 0.0f}, video::VSample{.v_phase = vp}};
    screen.process(pic, hbeam, vbeam); // sample 0 seeds black; sample 1 deposits
    const auto frame = screen.snapshot();
    for (std::size_t i = 0; i < frame.pixels.size(); ++i)
      if (frame.pixels[i] > 0)
        return std::pair{i % cfg.width, i / cfg.width};
    return std::pair{std::size_t{9999}, std::size_t{9999}};
  };

  video::ScreenConfig cfg{.width = 64, .height = 64, .sample_rate_hz = kRate, .beam_sigma = 0.0, .gamma = 1.0};
  CHECK(run_one(cfg, 0.5f, 0.5f).first == 32);
  cfg.h_window_lo = 0.25;
  cfg.h_window_hi = 0.75;
  cfg.v_window_lo = 0.25;
  cfg.v_window_hi = 0.75;
  CHECK(run_one(cfg, 0.5f, 0.5f).first == 32); // window centre stays centred
  CHECK(run_one(cfg, 0.375f, 0.5f).first == 16); // quarter into the window
  CHECK(run_one(cfg, 0.5f, 0.375f).second < 32); // above centre (yoke shear offsets it slightly)

  // Readout encode: a pixel at half the drive of the brightest must read at a
  // ratio of 0.5 with the linear readout and 0.5^(1/g) with an encode of g —
  // ratios, because a once-hit pixel doesn't reach the steady-state white the
  // readout normalises to.
  const auto grey_ratio = [&](double readout_gamma) {
    // contrast 0.5: the brightest deposit (gun drive 0.5) stays below the
    // absolute white point so neither level clips and the ratio is exact.
    const video::ScreenConfig rc{.width = 8,
        .height = 8,
        .sample_rate_hz = kRate,
        .beam_sigma = 0.0,
        .gamma = 1.0,
        .contrast = 0.5,
        .readout_gamma = readout_gamma};
    video::Screen screen{rc};
    const std::array pic{
        video::ChromaSample{.luma = 1.0f}, video::ChromaSample{.luma = 0.0f}, video::ChromaSample{.luma = 0.5f}};
    const std::array hbeam{
        video::BeamSample{.h_phase = 0.10f}, video::BeamSample{.h_phase = 0.3f}, video::BeamSample{.h_phase = 0.7f}};
    const std::array vbeam{
        video::VSample{.v_phase = 0.5f}, video::VSample{.v_phase = 0.5f}, video::VSample{.v_phase = 0.5f}};
    screen.process(pic, hbeam, vbeam);
    const auto frame = screen.snapshot();
    std::uint8_t hi = 0;
    std::uint8_t mid = 0;
    for (const auto p: frame.pixels) {
      if (p > hi) {
        mid = hi;
        hi = p;
      }
      else if (p > mid) {
        mid = p;
      }
    }
    REQUIRE(hi > 0);
    return static_cast<double>(mid) / static_cast<double>(hi);
  };
  CHECK(std::abs(grey_ratio(1.0) - 0.5) < 0.02); // linear: half drive, half level
  CHECK(std::abs(grey_ratio(2.2) - std::pow(0.5, 1.0 / 2.2)) < 0.02); // encoded: brighter mids
}

TEST_CASE("beam sigma is raster-relative: the spot scales with the line pitch") {
  // One bright sample mid-screen; the number of rows its splat touches must
  // scale with the output height (the same physical spot on a denser raster),
  // which is the point of specifying sigma in scanline pitches.
  const auto lit_rows = [](std::size_t height) {
    const video::ScreenConfig cfg{
        .width = 32, .height = height, .sample_rate_hz = kRate, .beam_sigma = 0.5, .gamma = 1.0};
    video::Screen screen{cfg};
    const std::array pic{video::ChromaSample{.luma = 1.0f}, video::ChromaSample{.luma = 0.0f}};
    const std::array hbeam{video::BeamSample{.h_phase = 0.10f}, video::BeamSample{.h_phase = 0.5f}};
    const std::array vbeam{video::VSample{.v_phase = 0.5f}, video::VSample{.v_phase = 0.5f}};
    screen.process(pic, hbeam, vbeam);
    const auto frame = screen.snapshot();
    std::size_t rows = 0;
    for (std::size_t r = 0; r < height; ++r)
      for (std::size_t c = 0; c < 32; ++c)
        if (frame.pixels[r * 32 + c] > 0) {
          ++rows;
          break;
        }
    return rows;
  };
  const auto small = lit_rows(64);
  const auto big = lit_rows(256);
  CHECK(big >= 3 * small); // 4x the height => ~4x the rows (radius quantisation slack)
}

namespace {
// Feed `lines` lines of synthetic beam data straight into a Screen: each line
// opens with a back-porch sample at blanking level (0.3 — seeds/holds the DC
// restore), then `active` samples at the given luma: 0.3 is true black (zero
// drive) and 0.0 is full white (drive 0.3, which the AGC white tracker then
// treats as full scale). Optionally one probe sample at (probe_h, probe_v).
// active defaults to a compressed 24-sample micro-line (plenty for the per-line
// mechanisms); pass something broadcast-shaped (~720 at kRate) when the test
// exercises a real per-sample time constant like the PWL's sense one-pole.
void feed_lines(video::Screen &screen, std::size_t lines, float luma, double probe_h = -1.0, double probe_v = 0.5,
    std::size_t active = 24) {
  std::vector<video::ChromaSample> pic;
  std::vector<video::BeamSample> hbeam;
  std::vector<video::VSample> vbeam;
  for (std::size_t l = 0; l < lines; ++l) {
    pic.clear();
    hbeam.clear();
    vbeam.clear();
    // Back porch at BLANKING level (0.3 in these envelope units — sync would be
    // 1.0): this is the black the DC restore clamps to, so active luma 0.3 is
    // true black (no drive) and 0.0 is full white.
    pic.push_back(video::ChromaSample{.luma = 0.3f});
    hbeam.push_back(video::BeamSample{.h_phase = 0.10f, .line_start = true});
    vbeam.push_back(video::VSample{.v_phase = 0.5f});
    for (std::size_t k = 0; k < active; ++k) {
      pic.push_back(video::ChromaSample{.luma = luma});
      hbeam.push_back(video::BeamSample{.h_phase = 0.2f + 0.7f * static_cast<float>(k) / static_cast<float>(active)});
      vbeam.push_back(video::VSample{.v_phase = 0.5f});
    }
    if (probe_h >= 0.0 && l + 1 == lines) {
      pic.push_back(video::ChromaSample{.luma = 0.0f}); // a white probe dot
      hbeam.push_back(video::BeamSample{.h_phase = static_cast<float>(probe_h)});
      vbeam.push_back(video::VSample{.v_phase = static_cast<float>(probe_v)});
    }
    screen.process(pic, hbeam, vbeam);
  }
}

// The brightest pixel's (col, row) in a snapshot, searched over [row_lo,
// row_hi) so a probe dot can be found away from the main scan's deposits.
std::pair<std::size_t, std::size_t> peak_pixel(
    const video::Screen &screen, std::size_t width, std::size_t row_lo, std::size_t row_hi) {
  const auto frame = screen.snapshot();
  std::size_t best = row_lo * width;
  for (std::size_t i = row_lo * width; i < row_hi * width; ++i)
    if (frame.pixels[i] > frame.pixels[best])
      best = i;
  return {best % width, best / width};
}
} // namespace

TEST_CASE("EHT sags under beam load, recovers, and breathes the raster") {
  constexpr double kSag = 0.08;
  // The pot turned up so feed_lines' full white (drive 0.3) reaches the
  // absolute white point (0.56) - a full-white line then registers load 1.0.
  const video::ScreenConfig cfg{.width = 64,
      .height = 256,
      .sample_rate_hz = kRate,
      .beam_sigma = 0.0,
      .gamma = 1.0,
      .contrast = 0.56 / 0.3,
      .eht_sag = kSag,
      .eht_tc_fields = 1.0};
  video::Screen loaded{cfg};

  // Sustained white: the EHT integrates down toward 1 - sag (the time constant
  // is 1 field = 312.5 nominal lines; 700 lines is > 2 tc).
  feed_lines(loaded, 700, 0.0f);
  CHECK(loaded.eht() < 1.0 - 0.7 * kSag);
  CHECK(loaded.eht() > 1.0 - kSag - 1e-9);

  // Breathing: a probe dot far from the vertical centre lands further out on
  // the sagged set than on an unloaded one (deflection ~ 1/sqrt(EHT)).
  video::Screen fresh{cfg};
  feed_lines(fresh, 10, 0.3f, 0.5, 0.125); // dark lines: no load
  feed_lines(loaded, 1, 0.0f, 0.5, 0.125); // still loaded
  const auto fresh_row = peak_pixel(fresh, 64, 0, 100).second; // the probe lives near row 32
  const auto loaded_row = peak_pixel(loaded, 64, 0, 100).second;
  CHECK(fresh_row > loaded_row); // v=0.125 is above centre: growth pushes it UP (away from centre)
  CHECK(fresh_row - loaded_row >= 2);

  // Recovery: sustained black brings the EHT back up.
  feed_lines(loaded, 700, 0.3f);
  CHECK(loaded.eht() > 1.0 - 0.3 * kSag);
}

TEST_CASE("line pulling stretches the line after a bright one") {
  const video::ScreenConfig cfg{
      .width = 256, .height = 64, .sample_rate_hz = kRate, .beam_sigma = 0.0, .gamma = 1.0, .line_pull = 0.02};
  // The probe sits at v = 0.125 (row ~8), away from the scan lines at v = 0.5
  // (row 32), so the restricted peak search finds it and not the white line.
  video::Screen dark{cfg};
  feed_lines(dark, 4, 0.3f, 0.9, 0.125); // probe after dark lines: nominal position
  video::Screen bright{cfg};
  feed_lines(bright, 3, 0.3f);
  feed_lines(bright, 1, 0.0f); // a full-white line loads the line-output stage...
  feed_lines(bright, 1, 0.3f, 0.9, 0.125); // ...and the NEXT line scans wider
  const auto x_dark = peak_pixel(dark, 256, 0, 16).first;
  const auto x_bright = peak_pixel(bright, 256, 0, 16).first;
  CHECK(x_bright > x_dark); // right of centre: the pull pushes it further right
}

TEST_CASE("the beam-current limiter pulls sustained brightness to its threshold") {
  // Pot up as in the EHT test: full white = load 1.0 against absolute levels.
  const video::ScreenConfig cfg{.width = 64,
      .height = 64,
      .sample_rate_hz = kRate,
      .beam_sigma = 0.0,
      .gamma = 1.0,
      .contrast = 0.56 / 0.3,
      .bcl_threshold = 0.5,
      .bcl_tc_fields = 0.5};
  video::Screen screen{cfg};
  // Sustained full white: load 1.0 > threshold 0.5, so the limiter settles the
  // video gain near threshold/load. The loop is closed (the limited drive is
  // what the sensor measures), so the settled load is the threshold and the
  // gain ~ threshold (for a full-white source).
  feed_lines(screen, 900, 0.0f);
  CHECK(screen.limiter_gain() < 0.6);
  CHECK(screen.limiter_gain() > 0.4);
  // A true steady state: thousands more white lines must NOT ratchet the gain
  // further down (the failure mode of a reference that adapts to its own
  // limiting — exactly what sank the first peak-white limiter design).
  feed_lines(screen, 5000, 0.0f);
  CHECK(screen.limiter_gain() < 0.6);
  CHECK(screen.limiter_gain() > 0.4);
  // A dark picture releases it.
  feed_lines(screen, 900, 0.3f);
  CHECK(screen.limiter_gain() > 0.95);
}

TEST_CASE("absolute levels: a dim source stays dim (no autocontrast)") {
  // One line at a given drive; the brightest pixel read back. With absolute
  // levels a half-drive line reads half the full-white level - the readout
  // white is the System I geometry constant, not whatever arrived. The
  // tracked (legacy) mode stretches each to its own peak, reading the same.
  const auto peak_of = [](float luma, bool tracked) {
    // width 64: feed_lines' 24 active samples land one per column, so the
    // peak pixel holds exactly one deposit and the ratio is exact.
    const video::ScreenConfig cfg{
        .width = 64, .height = 32, .sample_rate_hz = kRate, .beam_sigma = 0.0, .gamma = 1.0, .tracked_white = tracked};
    video::Screen screen{cfg};
    feed_lines(screen, 1, luma);
    const auto frame = screen.snapshot();
    return static_cast<double>(*std::ranges::max_element(frame.pixels));
  };
  // Blanking is 0.3 in feed_lines' units, so luma -0.26 is a standard
  // full-white drive (0.56) and 0.02 is exactly half of it.
  const double full = peak_of(-0.26f, false);
  const double half = peak_of(0.02f, false);
  REQUIRE(full > 100.0);
  CHECK(std::abs(half / full - 0.5) < 0.02);
  // The autocontrast reads both at the same level - the behaviour replaced.
  const double tracked_ratio = peak_of(0.02f, true) / peak_of(-0.26f, true);
  CHECK(std::abs(tracked_ratio - 1.0) < 0.05);
}

TEST_CASE("peak-white limiter: one-line delay, pull to the ceiling, recovery") {
  // Ceiling at 1.0 x standard white (drive 0.56); the over-white content
  // drives 0.8. BCL off, so limiter_gain() reads the PWL gain alone.
  const video::ScreenConfig cfg{
      .width = 32, .height = 32, .sample_rate_hz = kRate, .beam_sigma = 0.0, .gamma = 1.0, .pwl_threshold = 1.0};
  constexpr float kOverWhite = 0.3f - 0.8f; // luma for drive 0.8 > the 0.56 ceiling
  // Broadcast-shaped 45 us lines: the sense one-pole is a time constant, so
  // the default 1.5 us micro-lines would leave it half-charged and the settled
  // gain off its geometric value.
  const auto feed = [](video::Screen &screen, std::size_t lines, float luma) {
    feed_lines(screen, lines, luma, -1.0, 0.5, 720);
  };

  // A single over-white line between dark ones must NOT engage it: the
  // datasheet delays the start of limiting by one line period, so test
  // patterns with abrupt colour-to-white transitions are left alone.
  video::Screen flash{cfg};
  feed(flash, 5, 0.3f);
  feed(flash, 1, kOverWhite);
  feed(flash, 5, 0.3f);
  CHECK(flash.limiter_gain() > 0.999);

  // Sustained over-white pulls the gain so the peak settles AT the ceiling:
  // gain -> 0.56 / 0.8 = 0.7.
  video::Screen cooked{cfg};
  feed(cooked, 5, 0.3f);
  feed(cooked, 50, kOverWhite);
  CHECK(cooked.limiter_gain() < 0.72);
  CHECK(cooked.limiter_gain() > 0.65);

  // Dark content lets the control capacitor discharge back to unity.
  feed(cooked, 400, 0.3f);
  CHECK(cooked.limiter_gain() > 0.99);
}

TEST_CASE("peak-white limiter: sub-microsecond ringing is ignored, a broad overload is not") {
  // Same ceiling as above (drive 0.56). Each line is legal flat drive 0.45
  // with one excursion to drive 1.0 of a chosen width, mid-line - the shape a
  // Gibbs overshoot leaves on a sharp pixel edge when narrow, genuine peak
  // white when wide.
  const video::ScreenConfig cfg{
      .width = 32, .height = 32, .sample_rate_hz = kRate, .beam_sigma = 0.0, .gamma = 1.0, .pwl_threshold = 1.0};
  const auto feed = [](video::Screen &screen, std::size_t lines, std::size_t spike_samples) {
    constexpr std::size_t kActive = 720;
    std::vector<video::ChromaSample> pic;
    std::vector<video::BeamSample> hbeam;
    std::vector<video::VSample> vbeam;
    for (std::size_t l = 0; l < lines; ++l) {
      pic.clear();
      hbeam.clear();
      vbeam.clear();
      pic.push_back(video::ChromaSample{.luma = 0.3f});
      hbeam.push_back(video::BeamSample{.h_phase = 0.10f, .line_start = true});
      vbeam.push_back(video::VSample{.v_phase = 0.5f});
      for (std::size_t k = 0; k < kActive; ++k) {
        const bool spiking = k >= kActive / 2 && k < kActive / 2 + spike_samples;
        pic.push_back(video::ChromaSample{.luma = spiking ? -0.7f : -0.15f});
        hbeam.push_back(
            video::BeamSample{.h_phase = 0.2f + 0.7f * static_cast<float>(k) / static_cast<float>(kActive)});
        vbeam.push_back(video::VSample{.v_phase = 0.5f});
      }
      screen.process(pic, hbeam, vbeam);
    }
  };

  // 2 samples = 125 ns: the raw per-sample max is nearly 2x the ceiling, but
  // the band-limited sensor barely moves - no dimming, the bug this guards.
  video::Screen ringing{cfg};
  feed(ringing, 50, 2);
  CHECK(ringing.limiter_gain() > 0.999);

  // The same excursion widened to 10 us charges the sensor to its true level,
  // so the limiter pulls the peak to the ceiling: gain -> 0.56 / 1.0.
  video::Screen overload{cfg};
  feed(overload, 50, 160);
  CHECK(overload.limiter_gain() < 0.6);
  CHECK(overload.limiter_gain() > 0.5);
}
