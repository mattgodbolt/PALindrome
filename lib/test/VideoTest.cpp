#include "palindrome/agc.hpp"
#include "palindrome/horizontal_sweep.hpp"
#include "palindrome/phase.hpp"
#include "palindrome/sync_separator.hpp"
#include "palindrome/vertical_sync.hpp"
#include "palindrome/video_types.hpp"

#include "VideoSynth.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_range_equals.hpp>

namespace video = palindrome::video;
using videotest::kRate;
using videotest::synth_composite;

namespace {

// Run separator -> sweep over `env` fed in fixed-size chunks, returning the
// full BeamSample stream. chunk == env.size() is the single-block reference.
std::vector<video::BeamSample> run_chunked(std::span<const float> env, std::size_t chunk, bool adaptive) {
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate, .adaptive = adaptive}};
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

  // Both slicer modes: the fixed slice (a single bool of state) and the
  // adaptive trackers (the peak_/floor_ double recurrences, where chunking
  // bugs would actually live). Feed the same signal in awkward block sizes
  // that straddle line and pulse boundaries; the output must be bit-for-bit
  // identical to the single call.
  const auto same_beam = [](const video::BeamSample &a, const video::BeamSample &b) {
    return a.h_phase == b.h_phase && a.line_start == b.line_start;
  };
  const auto adaptive = GENERATE(false, true);
  const auto whole = run_chunked(env, env.size(), adaptive);
  const auto chunk = GENERATE(std::size_t{1}, std::size_t{7}, std::size_t{333}, std::size_t{4096});
  CAPTURE(adaptive, chunk);
  CHECK_THAT(run_chunked(env, chunk, adaptive), Catch::Matchers::RangeEquals(whole, same_beam));
}

TEST_CASE("Agc normalises an arbitrary carrier scale to tip = 1.0") {
  // The same composite at a wildly different capture level must come out at
  // the same absolute levels: the sync tip at 1.0 and the picture levels at
  // their true fractions of it - that's the whole point of the IF AGC.
  const auto env = synth_composite(40, 1028);
  auto scaled = env;
  for (auto &s: scaled)
    s *= 0.37f;

  video::Agc agc{video::AgcConfig{.sample_rate_hz = kRate}};
  agc.prepare(scaled.size());
  const auto out = agc.process(scaled);

  // Skip the first line (cold start charges on the first tip), then the tip
  // must sit at 1.0 and blanking at its true 0.3 - absolute, not stretched.
  float tip = 0.0f;
  for (std::size_t k = 2 * 1028; k < out.size(); ++k)
    tip = std::max(tip, out[k]);
  CHECK(std::abs(tip - 1.0f) < 1e-3f);
  // A blanking-level sample mid-line, far from sync and the white bar.
  CHECK(std::abs(out[10 * 1028 + 300] - 0.3f) < 1e-3f);
}

TEST_CASE("Agc is block-invariant (the streaming guarantee)") {
  const auto env = synth_composite(40, 1028);
  video::Agc whole_agc{video::AgcConfig{.sample_rate_hz = kRate}};
  whole_agc.prepare(env.size());
  const auto whole_view = whole_agc.process(env);
  const std::vector<float> whole(whole_view.begin(), whole_view.end());

  const auto chunk = GENERATE(std::size_t{1}, std::size_t{7}, std::size_t{333}, std::size_t{4096});
  CAPTURE(chunk);
  video::Agc agc{video::AgcConfig{.sample_rate_hz = kRate}};
  agc.prepare(chunk);
  std::vector<float> chunked;
  for (std::size_t off = 0; off < env.size(); off += chunk) {
    const std::size_t n = std::min(chunk, env.size() - off);
    const auto out = agc.process(std::span{env}.subspan(off, n));
    chunked.insert(chunked.end(), out.begin(), out.end());
  }
  CHECK(chunked == whole); // per-sample recurrence: bit-exact
}

TEST_CASE("Agc recovers after the carrier level drops") {
  // Carrier fades to half strength mid-stream. The instant attack means the
  // strong opening pins the gain low; recovery is the slow release, so right
  // after the drop the output tip reads ~0.5, and within a few time constants
  // it is back at 1.0. A short decay keeps the test brisk.
  const auto strong = synth_composite(20, 1028);
  auto weak = synth_composite(200, 1028);
  for (auto &s: weak)
    s *= 0.5f;

  video::Agc agc{video::AgcConfig{.sample_rate_hz = kRate, .decay_fields = 0.1}};
  agc.prepare(std::max(strong.size(), weak.size()));
  (void)agc.process(strong);
  const auto out = agc.process(weak);

  float early_tip = 0.0f;
  for (std::size_t k = 0; k < 1028; ++k)
    early_tip = std::max(early_tip, out[k]);
  CHECK(early_tip < 0.6f); // still levelled for the strong carrier
  float late_tip = 0.0f;
  for (std::size_t k = out.size() - 5 * 1028; k < out.size(); ++k)
    late_tip = std::max(late_tip, out[k]);
  CHECK(std::abs(late_tip - 1.0f) < 1e-2f); // released back to the new level
}

TEST_CASE("fixed slice: picture content cannot move the slice point") {
  // SMS-like shallow modulation: tip 1.0, blanking 0.865 (sync only 0.135
  // deep - well under the broadcast 0.24), active video either at blanking
  // (dark) or at 0.53 (bright). The fixed slicer references only the AGC'd
  // tip, so the sync bits must be IDENTICAL whatever the picture is doing -
  // the adaptive floor tracker, by contrast, moves with the content.
  const auto make = [](float active) {
    std::vector<float> e(40 * 1028, 0.865f);
    for (std::size_t l = 0; l < 40; ++l) {
      const std::size_t base = l * 1028;
      for (std::size_t k = 0; k < 1028; ++k) {
        if (k < 73)
          e[base + k] = 1.0f;
        else if (k > 200)
          e[base + k] = active;
      }
    }
    return e;
  };
  const auto dark = make(0.865f);
  const auto bright = make(0.53f);

  video::SyncSeparator sep_dark{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  video::SyncSeparator sep_bright{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  sep_dark.prepare(dark.size());
  sep_bright.prepare(bright.size());
  const auto sync_dark = sep_dark.process(dark);
  const auto sync_bright = sep_bright.process(bright);

  REQUIRE(sync_dark.size() == sync_bright.size());
  std::size_t pulses = 0;
  for (std::size_t i = 0; i < sync_dark.size(); ++i) {
    CHECK(sync_dark[i].sync == sync_bright[i].sync);
    if (i > 0 && sync_dark[i].sync && !sync_dark[i - 1].sync)
      ++pulses;
  }
  CHECK(pulses >= 39); // the shallow sync still slices: one pulse per line
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

TEST_CASE("VerticalSync snaps v_phase to the detected field anchor once per field") {
  // Pin where the detector fires or it rots silently: with kp = 1 the PI snap
  // lands v_phase on exactly 0 at the detected anchor (err is the phase itself,
  // or phase - 1, so the subtraction is exact), while the free-running wrap
  // would need the accumulator to hit exactly 1.0, which this non-dyadic omega
  // never does - so an exact-zero output after sample 0 IS the anchor.
  // Synthetic sync bits: normal lines ~7% duty, then a vertical interval of
  // three broad-pulse lines (~84% duty) per field; the integrator must cross
  // its slice once per interval, and only there.
  // Everything here is ratio-driven, so run at a small rate to keep the test
  // light: 64-sample lines, 320/field = 50 Hz fields at 1.024 MS/s.
  constexpr double kVsRate = 1.024e6;
  constexpr std::size_t kLineLen = 64;
  constexpr std::size_t kLinesPerField = 320;
  constexpr std::size_t kFields = 4;
  std::vector<video::SyncSample> sync;
  for (std::size_t f = 0; f < kFields; ++f)
    for (std::size_t l = 0; l < kLinesPerField; ++l) {
      const std::size_t duty = l < 3 ? (kLineLen * 84) / 100 : kLineLen / 14;
      for (std::size_t k = 0; k < kLineLen; ++k)
        sync.push_back(video::SyncSample{.sync = k < duty});
    }

  video::VerticalSync vsync{video::VerticalSyncConfig{.sample_rate_hz = kVsRate, .pll_kp = 1.0}};
  vsync.prepare(sync.size());
  const auto out = vsync.process(sync);
  std::vector<std::size_t> anchors;
  for (std::size_t i = 1; i < out.size(); ++i)
    if (out[i].v_phase == 0.0f)
      anchors.push_back(i);
  REQUIRE(vsync.detected_fields() == kFields);
  REQUIRE(anchors.size() == kFields);
  for (std::size_t f = 0; f < kFields; ++f) {
    // Each anchor lands inside its field's vertical interval (integrator lag
    // keeps it within the broad-pulse lines, well before line 5).
    const std::size_t base = f * kLinesPerField * kLineLen;
    CHECK(anchors[f] >= base);
    CHECK(anchors[f] < base + 5 * kLineLen);
  }
}
