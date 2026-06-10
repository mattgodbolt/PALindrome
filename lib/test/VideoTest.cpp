#include "palindrome/chroma_decoder.hpp"
#include "palindrome/horizontal_sweep.hpp"
#include "palindrome/phase.hpp"
#include "palindrome/screen.hpp"
#include "palindrome/sync_separator.hpp"
#include "palindrome/vertical_sync.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace video = palindrome::video;

namespace {

// A crude synthetic composite envelope: `lines` lines of `line_len` samples
// each, sync tip (1.0) at the start of every line, back porch + active video
// (0.3 black) with one white bar (0.0) so there is content to slice around.
// Negatively modulated, so sync is the peak — exactly what the separator
// expects. Enough lines for the sweep to lock.
std::vector<float> synth_composite(std::size_t lines, std::size_t line_len) {
  std::vector<float> e(lines * line_len, 0.3f);
  for (std::size_t l = 0; l < lines; ++l) {
    const std::size_t base = l * line_len;
    for (std::size_t k = 0; k < line_len; ++k) {
      if (k < line_len / 14) // ~4.6 us at 1028 samples/line: line-sync width
        e[base + k] = 1.0f;
      else if (k > line_len / 2 && k < line_len / 2 + 40) // a white bar mid-line
        e[base + k] = 0.0f;
    }
  }
  return e;
}

constexpr double kRate = 16.0e6;

// A synthetic composite carrying chroma: synth_composite plus ~10 cycles of
// colour burst on the back porch and a constant-phase subcarrier across the
// active video, so the chroma decoder has a real burst to gate and a steady
// colour to recover. The burst swings ±45° about its mean axis on alternate
// lines — the PAL swinging burst, which is what the ident (and so the colour
// killer) recognises as PAL; without the swing the killer rightly treats the
// signal as not-PAL and mutes it.
std::vector<float> synth_colour_composite(std::size_t lines, std::size_t line_len, double fsc_offset_hz = 0.0) {
  auto e = synth_composite(lines, line_len);
  const double fsc = 4.43361875e6 + fsc_offset_hz;
  const double w = 2.0 * std::numbers::pi * fsc / kRate;
  for (std::size_t l = 0; l < lines; ++l) {
    const std::size_t base = l * line_len;
    const double swing = (l % 2 == 0 ? 1.0 : -1.0) * std::numbers::pi / 4.0;
    for (std::size_t k = 0; k < line_len; ++k) {
      const double phase = w * static_cast<double>(base + k);
      const bool in_burst = k >= line_len / 12 && k < line_len / 12 + 36;
      const bool in_active = k > line_len / 6;
      if (in_burst)
        e[base + k] += 0.15f * static_cast<float>(std::sin(phase + swing)); // ±45° swinging burst
      else if (in_active)
        e[base + k] += 0.08f * static_cast<float>(std::cos(phase)); // steady chroma
    }
  }
  return e;
}

// Run separator -> sweep over `env` fed in fixed-size chunks, returning the
// full BeamSample stream. chunk == env.size() is the single-block reference.
std::vector<video::BeamSample> run_chunked(std::span<const float> env, std::size_t chunk) {
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  video::HorizontalSweep sweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  sep.prepare(chunk);
  sweep.prepare(chunk);
  std::vector<video::BeamSample> out;
  for (std::size_t off = 0; off < env.size(); off += chunk) {
    const std::size_t n = std::min(chunk, env.size() - off);
    const std::span<const video::SyncSample> sync = sep.process(env.subspan(off, n));
    const std::span<const video::BeamSample> beam = sweep.process(sync);
    out.insert(out.end(), beam.begin(), beam.end());
  }
  return out;
}

} // namespace

TEST_CASE("separator + sweep are block-invariant (the streaming guarantee)") {
  const auto env = synth_composite(40, 1028);
  const auto whole = run_chunked(env, env.size());

  // Feed the same signal in awkward block sizes that straddle line and pulse
  // boundaries; the output must be bit-for-bit identical to the single call.
  for (const std::size_t chunk: {std::size_t{1}, std::size_t{7}, std::size_t{333}, std::size_t{4096}}) {
    const auto chunked = run_chunked(env, chunk);
    REQUIRE(chunked.size() == whole.size());
    for (std::size_t i = 0; i < whole.size(); ++i) {
      CHECK(chunked[i].h_phase == whole[i].h_phase);
      CHECK(chunked[i].line_start == whole[i].line_start);
    }
  }
}

TEST_CASE("HorizontalSweep flywheel: locks, rides a phase step slowly, re-acquires") {
  // A bare sync-bit train: one `pulse`-wide line-sync pulse at the start of
  // every `line_len`-sample line. 1024 samples = exactly nominal at 16 MS/s.
  constexpr std::size_t kLine = 1024;
  constexpr std::size_t kPulse = 73; // ~4.6 us: inside the line-sync width window
  const auto one_line = [] {
    std::vector<video::SyncSample> line(kLine);
    for (std::size_t k = 0; k < kPulse; ++k)
      line[k].sync = true;
    return line;
  }();
  const std::vector<video::SyncSample> silence(kLine / 10); // 0.1 line of no sync

  // Feed one line; return the oscillator's phase error at the leading edge
  // (the BeamSample there is written before this line's trailing-edge
  // correction, so it is the pre-correction error the loop sees).
  const auto feed_line = [&](video::HorizontalSweep &sweep) {
    const auto beam = sweep.process(one_line);
    const double hp = beam.front().h_phase;
    return std::abs(palindrome::dsp::wrap_error(hp));
  };

  video::HorizontalSweep sweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  sweep.prepare(kLine);

  // Acquisition: clean nominal lines bring the coincidence detector up.
  CHECK(!sweep.locked());
  for (int l = 0; l < 30; ++l)
    feed_line(sweep);
  CHECK(sweep.locked());

  // A 0.1-line phase step (all later edges arrive late). A locked flywheel
  // must NOT snap: the error decays over many lines (the top-of-picture
  // flagging of a real set), and the sustained miss drains the detector back
  // to acquisition before the loop pulls in fast and re-locks.
  static_cast<void>(sweep.process(silence));
  const double err0 = feed_line(sweep);
  CHECK(err0 > 0.08); // the step arrived
  double err = err0;
  bool unlocked_seen = false;
  for (int l = 0; l < 3; ++l)
    err = feed_line(sweep);
  CHECK(err > 0.04); // still mostly uncorrected three lines on: no snap
  for (int l = 0; l < 60; ++l) {
    feed_line(sweep);
    unlocked_seen = unlocked_seen || !sweep.locked();
  }
  CHECK(unlocked_seen); // the detector dropped to acquisition...
  CHECK(sweep.locked()); // ...and the loop re-locked
  CHECK(feed_line(sweep) < 0.01); // back on the edge

  // Direct triggering (both gain sets at the old kp = 1) snaps the same step
  // within one line — the pre-flywheel behaviour, still reachable by knob.
  video::HorizontalSweep direct{video::HorizontalSweepConfig{
      .sample_rate_hz = kRate, .pll_kp = 1.0, .pll_ki = 1.0e-5, .acq_kp = 1.0, .acq_ki = 1.0e-5}};
  direct.prepare(kLine);
  for (int l = 0; l < 30; ++l)
    feed_line(direct);
  static_cast<void>(direct.process(silence));
  CHECK(feed_line(direct) > 0.08); // the step, seen once...
  CHECK(feed_line(direct) < 0.01); // ...and gone the next line
}

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

TEST_CASE("Screen snapshots a field before fading it, exactly once per field_start") {
  // persistence 2 fields => a per-field fade of exp(-1/persistence); gamma 1 keeps
  // the gun linear so the deposit is simple to reason about.
  const double persistence = 2.0;
  const video::ScreenConfig cfg{.width = 8,
      .height = 8,
      .sample_rate_hz = kRate,
      .persistence_fields = persistence,
      .beam_sigma = 0.0,
      .gamma = 1.0};
  const float field_decay = static_cast<float>(std::exp(-1.0 / persistence));

  video::Screen screen{cfg};

  // Field 0: a back-porch sample seeds the black reference, then active white
  // samples paint a few pixels. The gun stays driven at a constant level the whole
  // time, so the AGC white reference is pinned and the readout scale is identical
  // across the snapshots below — any change in pixel value is purely the fade.
  std::vector<video::ChromaSample> pic;
  std::vector<video::BeamSample> hbeam;
  std::vector<video::VSample> vbeam;
  const auto add = [&](float h_phase, float v_phase, float luma, bool field_start) {
    pic.push_back(video::ChromaSample{.luma = luma});
    hbeam.push_back(video::BeamSample{.h_phase = h_phase});
    vbeam.push_back(video::VSample{.v_phase = v_phase, .field_start = field_start});
  };
  add(0.10f, 0.0f, 1.0f, true); // back porch: seed black_ = 1.0 (field 0 start)
  for (const float vp: {0.25f, 0.5f, 0.75f})
    add(0.5f, vp, 0.0f, false); // active white: deposit gun drive 1.0 into a pixel
  screen.process(pic, hbeam, vbeam);

  const auto deposited = screen.snapshot(); // the un-faded charge
  REQUIRE(std::ranges::any_of(deposited.pixels, [](auto p) { return p > 0; }));

  // One field_start, beam blanked (h_phase below h_blank) but the gun still driven
  // so white stays pinned. It must snapshot BEFORE applying a single fade.
  video::Screen::Frame at_boundary;
  const std::array boundary_pic{video::ChromaSample{.luma = 0.0f}};
  const std::array boundary_hbeam{video::BeamSample{.h_phase = 0.0f}};
  const std::array boundary_vbeam{video::VSample{.v_phase = 0.0f, .field_start = true}};
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

TEST_CASE("Screen latch defers quantisation without changing the boundary frame") {
  const video::ScreenConfig cfg{.width = 8, .height = 8, .sample_rate_hz = kRate, .beam_sigma = 0.0, .gamma = 1.0};
  video::Screen screen{cfg};

  // Before any latch, latched_frame() falls back to the live snapshot.
  CHECK(screen.latched_frame().pixels == screen.snapshot().pixels);

  std::vector<video::ChromaSample> pic;
  std::vector<video::BeamSample> hbeam;
  std::vector<video::VSample> vbeam;
  const auto add = [&](float h_phase, float v_phase, float luma, bool field_start) {
    pic.push_back(video::ChromaSample{.luma = luma});
    hbeam.push_back(video::BeamSample{.h_phase = h_phase});
    vbeam.push_back(video::VSample{.v_phase = v_phase, .field_start = field_start});
  };
  add(0.10f, 0.0f, 1.0f, false); // back porch: seed the black reference
  for (const float vp: {0.25f, 0.5f, 0.75f})
    add(0.5f, vp, 0.0f, false); // active white into three pixels
  screen.process(pic, hbeam, vbeam);

  // At the boundary, quantise now (frame) AND latch; then deposit more on top.
  // The latched frame must still equal the boundary-time quantisation exactly —
  // that equivalence is what lets the single-image driver defer the quantise.
  video::Screen::Frame at_boundary;
  pic.clear();
  hbeam.clear();
  vbeam.clear();
  add(0.10f, 0.0f, 1.0f, true); // the field boundary
  add(0.5f, 0.125f, 0.0f, false); // post-boundary deposit, must not leak into the latch
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
    const video::ScreenConfig rc{.width = 8,
        .height = 8,
        .sample_rate_hz = kRate,
        .beam_sigma = 0.0,
        .gamma = 1.0,
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
void feed_lines(video::Screen &screen, std::size_t lines, float luma, double probe_h = -1.0, double probe_v = 0.5) {
  constexpr std::size_t kActive = 24;
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
    for (std::size_t k = 0; k < kActive; ++k) {
      pic.push_back(video::ChromaSample{.luma = luma});
      hbeam.push_back(video::BeamSample{.h_phase = 0.2f + 0.7f * static_cast<float>(k) / kActive});
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
  const video::ScreenConfig cfg{.width = 64,
      .height = 256,
      .sample_rate_hz = kRate,
      .beam_sigma = 0.0,
      .gamma = 1.0,
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
  const video::ScreenConfig cfg{.width = 64,
      .height = 64,
      .sample_rate_hz = kRate,
      .beam_sigma = 0.0,
      .gamma = 1.0,
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

TEST_CASE("ChromaDecoder is block-invariant (the streaming guarantee)") {
  const auto env = synth_colour_composite(40, 1028);

  // Pre-compute the horizontal rail once; the chroma decoder joins it by index.
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  sep.prepare(env.size());
  const auto sync = sep.process(env);
  std::vector<video::SyncSample> sync_copy{sync.begin(), sync.end()};
  video::HorizontalSweep hsweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  hsweep.prepare(sync_copy.size());
  std::vector<video::BeamSample> hbeam;
  {
    const auto b = hsweep.process(sync_copy);
    hbeam.assign(b.begin(), b.end());
  }

  // Every comb mode must be block-invariant — the NCO phasor, FIR state, burst
  // gate, comb ring and parity all carry across calls in each.
  for (const auto mode:
      {video::CombMode::post, video::CombMode::delay_line, video::CombMode::off, video::CombMode::glass}) {
    const video::ChromaDecoderConfig cfg{.sample_rate_hz = kRate, .comb_mode = mode};

    video::ChromaDecoder whole{cfg};
    whole.prepare(env.size());
    std::vector<video::ChromaSample> ref;
    {
      const auto out = whole.process(env, hbeam);
      ref.assign(out.begin(), out.end());
    }

    // The same signal in awkward block sizes must reproduce it sample-for-sample.
    for (const std::size_t chunk: {std::size_t{1}, std::size_t{7}, std::size_t{333}, std::size_t{4096}}) {
      video::ChromaDecoder chunked{cfg};
      chunked.prepare(chunk);
      std::vector<video::ChromaSample> got;
      for (std::size_t off = 0; off < env.size(); off += chunk) {
        const std::size_t n = std::min(chunk, env.size() - off);
        const auto out = chunked.process(std::span{env}.subspan(off, n), std::span{hbeam}.subspan(off, n));
        got.insert(got.end(), out.begin(), out.end());
      }
      REQUIRE(got.size() == ref.size());
      for (std::size_t i = 0; i < ref.size(); ++i) {
        CHECK(got[i].luma == ref[i].luma);
        CHECK(got[i].u == ref[i].u);
        CHECK(got[i].v == ref[i].v);
      }
    }
  }
}

namespace {
// Run separator -> sweep -> chroma over `env`, returning the chroma stream and
// the decoder's killer gate after the run.
struct ChromaRun {
  std::vector<video::ChromaSample> out;
  double killer_gain;
  double subcarrier_hz;
};
ChromaRun run_chroma(std::span<const float> env, const video::ChromaDecoderConfig &cfg) {
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  sep.prepare(env.size());
  const auto sync = sep.process(env);
  video::HorizontalSweep hsweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  hsweep.prepare(sync.size());
  const auto hbeam = hsweep.process(sync);
  video::ChromaDecoder chroma{cfg};
  chroma.prepare(env.size());
  const auto out = chroma.process(env, hbeam);
  return ChromaRun{
      .out = {out.begin(), out.end()}, .killer_gain = chroma.killer_gain(), .subcarrier_hz = chroma.subcarrier_hz()};
}

float peak_chroma(std::span<const video::ChromaSample> s) {
  float peak = 0.0f;
  for (const auto &c: s)
    peak = std::max({peak, std::abs(c.u), std::abs(c.v)});
  return peak;
}

// The killer tests build their timing rail straight off the separator (no
// sync-branch low-pass), so unlike the full decoder the rail carries no group
// delay — the demodulated burst lands ~60 samples (the chroma path's delay)
// after the synth's back-porch position. Place the gate there.
video::ChromaDecoderConfig killer_test_config() {
  return video::ChromaDecoderConfig{.sample_rate_hz = kRate, .burst_gate_lo = 0.145, .burst_gate_hi = 0.175};
}
} // namespace

TEST_CASE("colour killer mutes a burst-free (mono) signal") {
  // synth_composite carries no subcarrier at all — a mono transmission (or the
  // CRTC-abuse trick that suppresses the burst). The ident never finds a PAL
  // swing, so the gate must stay shut and no spurious colour painted.
  const auto env = synth_composite(60, 1028);
  const auto run = run_chroma(env, killer_test_config());
  CHECK(run.killer_gain < 0.05);
  CHECK(peak_chroma(run.out) < 1e-3f);
}

TEST_CASE("colour killer mutes white noise") {
  // Deterministic LCG noise across the whole envelope: sync barely locks and
  // any burst-gate measurement is random. Noise can pass an amplitude test but
  // can't fake a bistable-consistent ±45° swing, so the killer holds the
  // chroma off — the picture decodes as (noisy) monochrome.
  std::vector<float> env(600 * 1028);
  std::uint32_t lcg = 1;
  for (auto &e: env) {
    lcg = lcg * 1664525u + 1013904223u;
    e = static_cast<float>(lcg >> 8) / static_cast<float>(1u << 24);
  }
  const auto run = run_chroma(env, killer_test_config());
  CHECK(run.killer_gain < 0.1);
  CHECK(peak_chroma(run.out) < 0.05f);
}

TEST_CASE("colour killer fades a valid burst in slowly, and can be disabled") {
  const auto env = synth_colour_composite(300, 1028);

  // A real burst identifies within a few lines, but the gate opens on the slow
  // saturation-control ramp: well off zero after 300 lines, nowhere near full.
  const auto period = run_chroma(env, killer_test_config());
  CHECK(period.killer_gain > 0.05);
  CHECK(period.killer_gain < 0.6);
  // The switch-on is visible in the output: the early lines are still fully
  // muted (the gate hasn't reached the switch point), the late ones carry
  // chroma — colour pops on and fades up, as a set at switch-on.
  const std::size_t fifth = period.out.size() / 5;
  CHECK(peak_chroma(std::span{period.out}.first(fifth)) < 1e-3f);
  CHECK(peak_chroma(std::span{period.out}.last(fifth)) > 0.05f);

  // killer_threshold <= 0 disables the gate entirely (the pre-killer decode).
  auto open_cfg = killer_test_config();
  open_cfg.killer_threshold = 0.0;
  const auto open = run_chroma(env, open_cfg);
  CHECK(open.killer_gain == 1.0);
  CHECK(peak_chroma(open.out) > peak_chroma(period.out));
}

TEST_CASE("APC pulls the crystal onto an in-range subcarrier offset") {
  // A source 200 Hz off the crystal — inside the catching range. The frequency
  // pull should walk the NCO onto the source: the reported crystal frequency
  // converges to the source's, and colour survives (killer gate opening).
  constexpr double offset = 200.0;
  const auto env = synth_colour_composite(300, 1028, offset);
  const auto run = run_chroma(env, killer_test_config());
  CHECK(std::abs(run.subcarrier_hz - (4.43361875e6 + offset)) < 20.0);
  CHECK(run.killer_gain > 0.05);
}

TEST_CASE("a source beyond the APC catching range fails to lock colour") {
  // 2 kHz off crystal: a real set's crystal can't be pulled that far. The NCO
  // pins at the catch-range rail, the reference can't track the residual, and
  // the ident/killer drop the colour — a mono picture, the authentic failure.
  constexpr double offset = 2000.0;
  const auto env = synth_colour_composite(600, 1028, offset);
  const auto run = run_chroma(env, killer_test_config());
  CHECK(run.subcarrier_hz > 4.43361875e6 + 400.0); // pinned at (or near) the +500 Hz rail
  CHECK(run.subcarrier_hz < 4.43361875e6 + 501.0);
  CHECK(run.killer_gain < 0.1);
  const std::size_t fifth = run.out.size() / 5;
  CHECK(peak_chroma(std::span{run.out}.last(fifth)) < 1e-3f); // muted, not garbage colour
}

TEST_CASE("the APC pull is block-invariant (the feedback reaches the same sample)") {
  // The retune feeds back into the demod mix, so process() internally segments
  // at gate closes — a retune must land at the same sample whatever the
  // caller's chunking, or the loop's delay would be the block size.
  const auto env = synth_colour_composite(120, 1028, 200.0);

  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  sep.prepare(env.size());
  const auto sync = sep.process(env);
  video::HorizontalSweep hsweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  hsweep.prepare(sync.size());
  std::vector<video::BeamSample> hbeam;
  {
    const auto b = hsweep.process(sync);
    hbeam.assign(b.begin(), b.end());
  }

  video::ChromaDecoder whole{killer_test_config()};
  whole.prepare(env.size());
  std::vector<video::ChromaSample> ref;
  {
    const auto o = whole.process(env, hbeam);
    ref.assign(o.begin(), o.end());
  }

  for (const std::size_t chunk: {std::size_t{7}, std::size_t{333}, std::size_t{4096}}) {
    video::ChromaDecoder chunked{killer_test_config()};
    chunked.prepare(chunk);
    std::vector<video::ChromaSample> got;
    for (std::size_t off = 0; off < env.size(); off += chunk) {
      const std::size_t m = std::min(chunk, env.size() - off);
      const auto o = chunked.process(std::span{env}.subspan(off, m), std::span{hbeam}.subspan(off, m));
      got.insert(got.end(), o.begin(), o.end());
    }
    REQUIRE(got.size() == ref.size());
    for (std::size_t i = 0; i < ref.size(); ++i) {
      CHECK(got[i].u == ref[i].u);
      CHECK(got[i].v == ref[i].v);
    }
    CHECK(chunked.subcarrier_hz() == whole.subcarrier_hz());
  }
}

TEST_CASE("APC catch range 0 pins the crystal (the pre-pull behaviour)") {
  const auto env = synth_colour_composite(300, 1028, 200.0);
  auto cfg = killer_test_config();
  cfg.apc_catch_range_hz = 0.0;
  const auto run = run_chroma(env, cfg);
  CHECK(run.subcarrier_hz == 4.43361875e6); // never retuned
  CHECK(run.killer_gain > 0.05); // the per-line rotation still locks an in-range source
}

TEST_CASE("the glass comb is the delay line at the real fixed geometry") {
  // The glass block is 283.5 subcarrier cycles = 1023 samples at 16 MS/s. On a
  // source whose lines are exactly that long, the adaptive delay-line comb
  // measures the same depth, so the two modes must agree bit-for-bit (after
  // the adaptive mode's first line-length measurement settles). On the
  // off-nominal 1028-sample lines they must diverge: the fixed block pairs
  // chroma displaced 5 samples along the line, ghosting colour transitions —
  // the real PAL-D off-spec misregistration the adaptive convenience hides.
  // Killer disabled: the comb is under test.
  const auto run_mode = [](std::span<const float> env, video::CombMode mode) {
    auto cfg = killer_test_config();
    cfg.killer_threshold = 0.0;
    cfg.comb_mode = mode;
    return run_chroma(env, cfg);
  };

  const auto on_spec = synth_colour_composite(60, 1023);
  const auto glass_on = run_mode(on_spec, video::CombMode::glass);
  const auto adaptive_on = run_mode(on_spec, video::CombMode::delay_line);
  REQUIRE(glass_on.out.size() == adaptive_on.out.size());
  for (std::size_t k = 5 * 1023; k < glass_on.out.size(); ++k) {
    CHECK(glass_on.out[k].u == adaptive_on.out[k].u);
    CHECK(glass_on.out[k].v == adaptive_on.out[k].v);
  }

  const auto off_spec = synth_colour_composite(60, 1028);
  const auto glass_off = run_mode(off_spec, video::CombMode::glass);
  const auto adaptive_off = run_mode(off_spec, video::CombMode::delay_line);
  REQUIRE(glass_off.out.size() == adaptive_off.out.size());
  float max_diff = 0.0f;
  for (std::size_t k = 5 * 1028; k < glass_off.out.size(); ++k)
    max_diff = std::max({max_diff, std::abs(glass_off.out[k].u - adaptive_off.out[k].u),
        std::abs(glass_off.out[k].v - adaptive_off.out[k].v)});
  CHECK(max_diff > 0.1f); // the mistimed pairing visibly corrupts the chroma
}

TEST_CASE("ChromaDecoder rejects an out-of-range ref_tc_lines") {
  const auto cfg = [](double tc) { return video::ChromaDecoderConfig{.sample_rate_hz = kRate, .ref_tc_lines = tc}; };
  CHECK_THROWS_AS(video::ChromaDecoder{cfg(1.0)}, std::invalid_argument); // below the floor
  CHECK_THROWS_AS(video::ChromaDecoder{cfg(150.0)}, std::invalid_argument); // above the ceiling
  CHECK_THROWS_AS(video::ChromaDecoder{cfg(std::nan(""))}, std::invalid_argument); // NaN fails the range test
  CHECK_NOTHROW(video::ChromaDecoder{cfg(10.0)}); // the default, in range
}
