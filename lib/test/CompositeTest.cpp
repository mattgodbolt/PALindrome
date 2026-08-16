#include "palindrome/composite.hpp"

#include "palindrome/sync_separator.hpp"
#include "palindrome/video_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_range_equals.hpp>

namespace video = palindrome::video;

namespace {

constexpr double kRate = 16.0e6;
constexpr std::size_t kLineLen = 1024;
constexpr std::size_t kSyncLen = kLineLen / 14;
constexpr double kFullScaleVolts = 1.0;
constexpr double kSyncVolts = 0.3;

// Envelope units per volt, the one gain the conversion turns on.
constexpr double kEnvelopePerVolt = (video::kSyncTipLevel - video::kBlankingLevel) / kSyncVolts;

// The same crude synthetic the video tests use. Only the sync tip at 1.0 is a
// real anchor; 0.3 and 0.0 are arbitrary stand-ins for dark and bright, NOT
// System I geometry (where blanking is kBlankingLevel and peak white is
// kPeakWhiteLevel). The absolute-anchors test above is what pins the standard.
std::vector<float> synth_envelope(std::size_t lines, std::size_t line_len) {
  std::vector<float> e(lines * line_len, 0.3f);
  for (std::size_t l = 0; l < lines; ++l) {
    const std::size_t base = l * line_len;
    for (std::size_t k = 0; k < line_len; ++k) {
      if (k < line_len / 14)
        e[base + k] = 1.0f;
      else if (k > line_len / 2 && k < line_len / 2 + 40)
        e[base + k] = 0.0f;
    }
  }
  return e;
}

// The exact inverse of the stage's map: envelope units back to baseband volts,
// with the sync tip placed at -sync_amplitude so blanking lands on 0 V.
std::vector<float> to_baseband(std::span<const float> envelope, double dc_offset = 0.0, double gain = 1.0) {
  std::vector<float> out(envelope.size());
  for (std::size_t k = 0; k < envelope.size(); ++k) {
    const double volts = -kSyncVolts + (video::kSyncTipLevel - static_cast<double>(envelope[k])) / kEnvelopePerVolt;
    out[k] = static_cast<float>((volts * gain + dc_offset) / kFullScaleVolts);
  }
  return out;
}

// Plain baseband volts: sync tip at -sync_v, picture at `picture` volts.
std::vector<float> baseband_lines(std::size_t lines, double picture, double sync_v = kSyncVolts) {
  std::vector<float> bb(lines * kLineLen);
  for (std::size_t l = 0; l < lines; ++l)
    for (std::size_t k = 0; k < kLineLen; ++k)
      bb[l * kLineLen + k] = static_cast<float>(k < kSyncLen ? -sync_v : picture);
  return bb;
}

video::CompositeInputConfig config(
    double clamp_lines, double full_scale_volts = kFullScaleVolts, double sync_amplitude_v = kSyncVolts) {
  return video::CompositeInputConfig{.sample_rate_hz = kRate,
      .full_scale_volts = full_scale_volts,
      .sync_amplitude_v = sync_amplitude_v,
      .clamp_lines = clamp_lines};
}

std::vector<float> convert(
    std::span<const float> baseband, const video::CompositeInputConfig &cfg, std::size_t chunk = 0) {
  video::CompositeInput in{cfg};
  const std::size_t step = chunk == 0 ? std::max<std::size_t>(baseband.size(), 1) : chunk;
  in.prepare(step);
  std::vector<float> out;
  out.reserve(baseband.size());
  for (std::size_t off = 0; off < baseband.size(); off += step) {
    const std::size_t n = std::min(step, baseband.size() - off);
    const std::span<const float> block = in.process(baseband.subspan(off, n));
    out.insert(out.end(), block.begin(), block.end());
  }
  return out;
}

std::vector<float> convert(std::span<const float> baseband, double clamp_lines, std::size_t chunk = 0) {
  return convert(baseband, config(clamp_lines), chunk);
}

// How many sync pulses a real slicer finds on a rail, at its fixed slice level.
std::size_t sync_pulses(std::span<const float> envelope) {
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  sep.prepare(envelope.size());
  const std::span<const video::SyncSample> sync = sep.process(envelope);
  std::size_t pulses = 0;
  bool prev = false;
  for (const auto &s: sync) {
    if (s.sync && !prev)
      ++pulses;
    prev = s.sync;
  }
  return pulses;
}

// Long enough that the clamp cannot drift within the test, isolating the map.
// Note this relies on rise_ underflowing to essentially nothing, which turns
// the clamp into a pure peak-hold - deliberate here, so the map is measured on
// its own.
constexpr double kHeldClamp = 1.0e6;

} // namespace

TEST_CASE("composite input lands baseband volts on the standard's anchors", "[composite]") {
  // The claim the whole design rests on, in absolute terms rather than by
  // round-tripping the stage against itself: nominal CVBS in, System I out.
  const auto rail = convert(baseband_lines(8, 0.0), kHeldClamp);
  const std::size_t settled = 4 * kLineLen;

  REQUIRE(rail[settled + 2] == Catch::Approx(video::kSyncTipLevel).margin(1.0e-5)); // -0.3 V
  REQUIRE(rail[settled + kSyncLen + 20] == Catch::Approx(video::kBlankingLevel).margin(1.0e-5)); // 0 V

  const auto white = convert(baseband_lines(8, 0.7), kHeldClamp);
  REQUIRE(white[settled + kSyncLen + 20] == Catch::Approx(video::kPeakWhiteLevel).margin(1.0e-5)); // +0.7 V
}

TEST_CASE("the declared capture scaling is what fixes a non-nominal source", "[composite]") {
  const auto reference = convert(baseband_lines(8, 0.7), config(kHeldClamp));
  const std::size_t probe = 4 * kLineLen + kSyncLen + 20;

  SECTION("a different full scale is the same physical signal") {
    // Same volts, expressed against a 2 V full scale: every sample halved.
    auto halved = baseband_lines(8, 0.7);
    for (auto &v: halved)
      v *= 0.5f;
    const auto rail = convert(halved, config(kHeldClamp, 2.0));
    for (std::size_t k = 0; k < reference.size(); ++k)
      REQUIRE(rail[k] == Catch::Approx(reference[k]).margin(1.0e-5));
  }

  SECTION("an under-modulating source declares its amplitude and recovers full geometry") {
    // Uniformly half-modulated: 0.15 V of sync AND a 0.35 V picture span. That
    // proportionality is the precondition - declaring a shallow sync amplitude
    // on a source whose picture span is still full over-scales it instead.
    const auto shallow = baseband_lines(8, 0.35, 0.15);
    const auto naive = convert(shallow, config(kHeldClamp));
    const auto declared = convert(shallow, config(kHeldClamp, kFullScaleVolts, 0.15));

    // Left at nominal the picture is crushed towards blanking; declared, peak
    // white lands where the standard says and the slicer's margin comes back.
    REQUIRE(static_cast<double>(naive[probe]) > video::kPeakWhiteLevel + 0.1);
    REQUIRE(declared[probe] == Catch::Approx(video::kPeakWhiteLevel).margin(1.0e-5));
  }
}

TEST_CASE("composite input inverts to the receiver's envelope convention", "[composite]") {
  const auto envelope = synth_envelope(32, kLineLen);
  const auto recovered = convert(to_baseband(envelope), kHeldClamp);

  REQUIRE(recovered.size() == envelope.size());
  for (std::size_t k = 0; k < envelope.size(); ++k)
    REQUIRE(recovered[k] == Catch::Approx(envelope[k]).margin(1.0e-5));
}

TEST_CASE("composite input restores a drifting DC, not just a constant one", "[composite]") {
  const auto envelope = synth_envelope(64, kLineLen);
  const auto reference = convert(to_baseband(envelope), 128.0);

  SECTION("a constant offset") {
    // Absorbed by the seed alone, so this pins the seeding rather than the
    // release path - the ramp below is what exercises the tracker.
    for (const double offset: {-0.4, 0.25, 2.0}) {
      const auto shifted = convert(to_baseband(envelope, offset), 128.0);
      for (std::size_t k = 0; k < reference.size(); ++k)
        REQUIRE(shifted[k] == Catch::Approx(reference[k]).margin(1.0e-5));
    }
  }

  SECTION("a slow ramp the seed cannot absorb") {
    auto ramped = to_baseband(envelope);
    for (std::size_t k = 0; k < ramped.size(); ++k)
      ramped[k] += static_cast<float>(0.5 * static_cast<double>(k) / static_cast<double>(ramped.size()));
    const auto rail = convert(ramped, 128.0);
    REQUIRE(sync_pulses(rail) == 64);
    // The clamp follows the ramp out. A one-pole necessarily lags a ramp, so
    // settled sync sits a little under the tip rather than exactly on it.
    REQUIRE(rail[48 * kLineLen + 2] == Catch::Approx(video::kSyncTipLevel).margin(0.06));
  }
}

TEST_CASE("a wrong scale costs contrast, not sync", "[composite]") {
  const auto envelope = synth_envelope(32, kLineLen);
  REQUIRE(sync_pulses(envelope) == 32);
  const std::size_t probe = 16 * kLineLen + kSyncLen + 20;

  // The tip anchors on kSyncTipLevel however wrong the gain is, so the slicer
  // keeps firing while sync still clears the slice. With the slicer's
  // hysteresis the entry depth is 0.075; this synthetic's sync sits 0.7 of the
  // span above black (not the standard's 0.24 above blanking), so the edge is
  // at about 0.107 of nominal gain.
  double previous_span = 0.0;
  for (const double gain: {0.5, 0.75, 1.5, 3.0}) {
    const auto rail = convert(to_baseband(envelope, 0.0, gain), kHeldClamp);
    REQUIRE(sync_pulses(rail) == 32);
    // The contrast half of the claim: the blanking-to-picture span scales.
    const double span = video::kSyncTipLevel - static_cast<double>(rail[probe]);
    REQUIRE(span > previous_span);
    previous_span = span;
  }

  // And the edge its own arithmetic predicts really is an edge: below it the
  // rail compresses until the slicer can no longer tell sync from picture, so
  // it stops recovering one pulse per line. (The count collapses towards 1
  // rather than 0, because what actually happens is that the whole line ends
  // up above the slice and sync never de-asserts.)
  REQUIRE(sync_pulses(convert(to_baseband(envelope, 0.0, 0.08), kHeldClamp)) < 32);
}

TEST_CASE("composite input is block invariant", "[composite]") {
  const auto envelope = synth_envelope(16, kLineLen);
  const auto baseband = to_baseband(envelope, 0.15);
  const auto reference = convert(baseband, 128.0);

  // A single recurrence in sample order, so chunking changes nothing at all:
  // assert equality, not a tolerance.
  for (const std::size_t chunk: {std::size_t{1}, std::size_t{17}, std::size_t{4096}}) {
    const auto chunked = convert(baseband, 128.0, chunk);
    REQUIRE_THAT(chunked, Catch::Matchers::RangeEquals(reference));
  }
}

TEST_CASE("the clamp rejects mains hum on the baseband", "[composite]") {
  // Black picture is the worst case: the largest gap for the release to climb.
  auto baseband = baseband_lines(160, 0.0);
  const double w = 2.0 * std::numbers::pi * 50.0 / kRate;
  for (std::size_t k = 0; k < baseband.size(); ++k)
    baseband[k] += static_cast<float>(0.15 * std::sin(w * static_cast<double>(k)));

  // Hum of half the sync amplitude would walk the black level clean through
  // the slice if it survived; the clamp tracks it out. Measured, 512 line
  // periods of release is where this finally breaks, so the default has room.
  REQUIRE(sync_pulses(convert(baseband, 128.0)) == 160);
}

TEST_CASE("the clamp's noise cliff stays where it was measured", "[composite]") {
  // The tracked minimum sits below the true tip under noise, depressing the
  // rail by a DC offset that the sync low-pass cannot remove; past a point the
  // slicer chatters inside the pulse. Pin where that is, so a later tweak to
  // the clamp cannot move it silently.
  auto noisy = [](double sigma) {
    auto bb = baseband_lines(160, 0.3);
    std::mt19937 rng{7};
    std::normal_distribution<double> nd{0.0, sigma};
    for (auto &v: bb)
      v += static_cast<float>(nd(rng));
    return convert(bb, 128.0);
  };
  REQUIRE(sync_pulses(noisy(0.010)) == 160);
  REQUIRE(sync_pulses(noisy(0.030)) > 160);
}

TEST_CASE("a long tip dwell does not disturb the clamp", "[composite]") {
  // The vertical interval sits at sync level for lines at a time (a Beeb's
  // unserrated vsync is the extreme). The input is at the tip throughout, so
  // the release contributes nothing and the clamp should come out of it
  // exactly as it went in.
  auto bb = baseband_lines(40, 0.3);
  for (std::size_t l = 20; l < 22; ++l) // two solid lines at sync level
    for (std::size_t k = 0; k < kLineLen; ++k)
      bb[l * kLineLen + k] = static_cast<float>(-kSyncVolts);

  const auto rail = convert(bb, 128.0);
  const std::size_t before = 18 * kLineLen + kSyncLen + 20;
  const std::size_t after = 30 * kLineLen + kSyncLen + 20;
  REQUIRE(rail[after] == Catch::Approx(rail[before]).margin(1.0e-4));
  for (std::size_t k = 20; k < kLineLen - 20; ++k)
    REQUIRE(rail[20 * kLineLen + k] == Catch::Approx(video::kSyncTipLevel).margin(1.0e-5));
}

TEST_CASE("composite input survives degenerate signals", "[composite]") {
  SECTION("a flat input pins at the tip and stays finite") {
    for (const float level: {0.0f, -0.8f, 0.8f}) {
      const std::vector<float> flat(4096, level);
      const auto rail = convert(flat, 128.0);
      REQUIRE(rail.size() == flat.size());
      for (const auto v: rail)
        REQUIRE(v == Catch::Approx(static_cast<float>(video::kSyncTipLevel)).margin(1.0e-5));
    }
  }

  SECTION("one non-finite sample cannot poison the clamp for good") {
    for (const float poison: {std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity(),
             std::numeric_limits<float>::infinity()}) {
      auto bb = baseband_lines(8, 0.3);
      bb[3 * kLineLen + 100] = poison;
      const auto rail = convert(bb, 128.0);
      const auto clean = convert(baseband_lines(8, 0.3), 128.0);
      // The guard keeps the poison out of the accumulator, so the clamp
      // converges straight back onto a clean run - not bit-identically, since
      // skipping the update costs that one release step, but to well inside
      // anything downstream can see. The offending sample itself still maps to
      // garbage, which is the one sample it costs; sanitising the output too
      // is not worth a per-sample branch.
      for (std::size_t k = 0; k < rail.size(); ++k)
        if (k != 3 * kLineLen + 100)
          REQUIRE(rail[k] == Catch::Approx(clean[k]).margin(1.0e-4));
    }
  }

  SECTION("a non-finite FIRST sample cannot poison the seed") {
    // The guard has to cover seeding too: seeding from NaN would leave every
    // later finite sample updating against a NaN tip, unrecoverably.
    for (const float poison: {std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity()}) {
      auto bb = baseband_lines(8, 0.3);
      bb[0] = poison;
      const auto rail = convert(bb, 128.0);
      for (std::size_t k = 1; k < rail.size(); ++k)
        REQUIRE(std::isfinite(rail[k]));
      REQUIRE(sync_pulses(rail) == 8);
    }
  }

  SECTION("an empty block is not a seeding event") {
    video::CompositeInput in{config(128.0)};
    in.prepare(kLineLen);
    REQUIRE(in.process({}).empty());
    const auto bb = baseband_lines(1, 0.3);
    const std::span<const float> rail = in.process(bb);
    REQUIRE(rail.size() == bb.size());
    REQUIRE(rail[2] == Catch::Approx(video::kSyncTipLevel).margin(1.0e-5));
  }
}

TEST_CASE("composite input rejects nonsense configuration", "[composite]") {
  auto bad = [](auto mutate) {
    auto cfg = config(128.0);
    mutate(cfg);
    return cfg;
  };
  REQUIRE_THROWS(video::CompositeInput{bad([](auto &c) { c.sample_rate_hz = 0.0; })});
  REQUIRE_THROWS(video::CompositeInput{bad([](auto &c) { c.nominal_line_hz = -1.0; })});
  REQUIRE_THROWS(video::CompositeInput{bad([](auto &c) { c.full_scale_volts = 0.0; })});
  REQUIRE_THROWS(video::CompositeInput{bad([](auto &c) { c.sync_amplitude_v = 0.0; })});
  REQUIRE_THROWS(video::CompositeInput{bad([](auto &c) { c.clamp_lines = 0.0; })});
}
