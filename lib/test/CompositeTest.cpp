#include "palindrome/composite.hpp"

#include "palindrome/sync_separator.hpp"
#include "palindrome/video_types.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
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
constexpr double kFullScaleVolts = 1.0;
constexpr double kSyncVolts = 0.3;

// Envelope units per volt, the one gain the conversion turns on.
constexpr double kEnvelopePerVolt = (video::kSyncTipLevel - video::kBlankingLevel) / kSyncVolts;

// The same crude synthetic the video tests use, written in the receiver's
// convention: sync tip at 1.0, black at 0.3, one white bar at 0.0.
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
// with the sync tip placed at -sync_amplitude so blanking lands on 0 V. Feeding
// this through CompositeInput must recover what we started with.
std::vector<float> to_baseband(std::span<const float> envelope, double dc_offset = 0.0, double gain = 1.0) {
  std::vector<float> out(envelope.size());
  for (std::size_t k = 0; k < envelope.size(); ++k) {
    const double volts = -kSyncVolts + (video::kSyncTipLevel - static_cast<double>(envelope[k])) / kEnvelopePerVolt;
    out[k] = static_cast<float>((volts * gain + dc_offset) / kFullScaleVolts);
  }
  return out;
}

video::CompositeInputConfig config(double clamp_lines) {
  return video::CompositeInputConfig{.sample_rate_hz = kRate,
      .full_scale_volts = kFullScaleVolts,
      .sync_amplitude_v = kSyncVolts,
      .clamp_lines = clamp_lines};
}

std::vector<float> convert(std::span<const float> baseband, double clamp_lines, std::size_t chunk = 0) {
  video::CompositeInput in{config(clamp_lines)};
  const std::size_t step = chunk == 0 ? baseband.size() : chunk;
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
constexpr double kHeldClamp = 1.0e6;

} // namespace

TEST_CASE("composite input inverts to the receiver's envelope convention", "[composite]") {
  const auto envelope = synth_envelope(32, kLineLen);
  const auto recovered = convert(to_baseband(envelope), kHeldClamp);

  REQUIRE(recovered.size() == envelope.size());
  for (std::size_t k = 0; k < envelope.size(); ++k)
    REQUIRE(recovered[k] == Catch::Approx(envelope[k]).margin(1.0e-5));
}

TEST_CASE("composite input restores DC, so an AC-coupled offset is invisible", "[composite]") {
  const auto envelope = synth_envelope(32, kLineLen);
  const auto reference = convert(to_baseband(envelope), kHeldClamp);

  for (const double offset: {-0.4, -0.05, 0.25, 2.0}) {
    const auto shifted = convert(to_baseband(envelope, offset), kHeldClamp);
    REQUIRE(shifted.size() == reference.size());
    for (std::size_t k = 0; k < reference.size(); ++k)
      REQUIRE(shifted[k] == Catch::Approx(reference[k]).margin(1.0e-5));
  }
}

TEST_CASE("a wrong scale costs contrast, not sync", "[composite]") {
  const auto envelope = synth_envelope(32, kLineLen);
  const auto expected = sync_pulses(envelope);
  REQUIRE(expected == 32);

  // The tip anchors on kSyncTipLevel however wrong the gain is, so the slicer
  // keeps firing as long as the sync depth still clears its slice level. Sync
  // is 0.24 deep nominally against a 0.08 slice, so a third of nominal is the
  // edge; either side of that the picture is flat or clipped, not unlocked.
  for (const double gain: {0.5, 0.75, 1.5, 3.0}) {
    const auto rail = convert(to_baseband(envelope, 0.0, gain), kHeldClamp);
    REQUIRE(sync_pulses(rail) == expected);
  }
}

TEST_CASE("composite input is block invariant", "[composite]") {
  const auto envelope = synth_envelope(16, kLineLen);
  const auto baseband = to_baseband(envelope, 0.15);
  const auto reference = convert(baseband, 32.0);

  // A single recurrence in sample order, so chunking changes nothing at all:
  // assert equality, not a tolerance.
  for (const std::size_t chunk: {std::size_t{1}, std::size_t{17}, std::size_t{4096}}) {
    const auto chunked = convert(baseband, 32.0, chunk);
    REQUIRE_THAT(chunked, Catch::Matchers::RangeEquals(reference));
  }
}

TEST_CASE("the clamp rejects mains hum on the baseband", "[composite]") {
  const auto envelope = synth_envelope(160, kLineLen);
  auto baseband = to_baseband(envelope);
  const double w = 2.0 * std::numbers::pi * 50.0 / kRate;
  for (std::size_t k = 0; k < baseband.size(); ++k)
    baseband[k] += static_cast<float>(0.15 * std::sin(w * static_cast<double>(k)));

  const auto rail = convert(baseband, 32.0);

  // Hum of half the sync amplitude would walk the black level clean through the
  // slice level if it survived; the clamp tracks it out and the slicer still
  // finds exactly one pulse per line.
  REQUIRE(sync_pulses(rail) == 160);
}

TEST_CASE("composite input survives degenerate signals", "[composite]") {
  for (const float level: {0.0f, -0.8f, 0.8f}) {
    const std::vector<float> flat(4096, level);
    const auto rail = convert(flat, 32.0);
    REQUIRE(rail.size() == flat.size());
    for (const auto v: rail) {
      REQUIRE(std::isfinite(v));
      REQUIRE(v == Catch::Approx(static_cast<float>(video::kSyncTipLevel)).margin(1.0e-5));
    }
  }
}

TEST_CASE("composite input rejects nonsense configuration", "[composite]") {
  auto bad = [](auto mutate) {
    auto cfg = config(32.0);
    mutate(cfg);
    return cfg;
  };
  REQUIRE_THROWS(video::CompositeInput{bad([](auto &c) { c.sample_rate_hz = 0.0; })});
  REQUIRE_THROWS(video::CompositeInput{bad([](auto &c) { c.nominal_line_hz = -1.0; })});
  REQUIRE_THROWS(video::CompositeInput{bad([](auto &c) { c.full_scale_volts = 0.0; })});
  REQUIRE_THROWS(video::CompositeInput{bad([](auto &c) { c.sync_amplitude_v = 0.0; })});
  REQUIRE_THROWS(video::CompositeInput{bad([](auto &c) { c.clamp_lines = 0.0; })});
}
