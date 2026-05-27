#include "palindrome/biquad.hpp"

#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace dsp = palindrome::dsp;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;

std::vector<float> tone(double fs, double freq, std::size_t n) {
  std::vector<float> x(n);
  for (std::size_t k = 0; k < n; ++k)
    x[k] = static_cast<float>(std::sin(two_pi * freq * static_cast<double>(k) / fs));
  return x;
}

double rms(std::span<const float> s) {
  double energy = 0.0;
  for (const float v: s)
    energy += static_cast<double>(v) * v;
  return std::sqrt(energy / static_cast<double>(s.size()));
}
} // namespace

TEST_CASE("notch rejects bad parameters") {
  CHECK_THROWS_AS(dsp::notch(1000.0, 0.0, 10.0), std::invalid_argument);
  CHECK_THROWS_AS(dsp::notch(1000.0, 600.0, 10.0), std::invalid_argument); // > fs/2
  CHECK_THROWS_AS(dsp::notch(1000.0, 100.0, 0.0), std::invalid_argument);
}

TEST_CASE("notch passes DC at unity gain") {
  auto dut = dsp::notch(1000.0, 100.0, 10.0);
  const std::vector<float> dc(500, 1.0f);
  std::vector<float> out;
  dut.process(dc, out);
  CHECK_THAT(out.back(), WithinAbs(1.0, 1e-4));
}

TEST_CASE("notch annihilates a tone at its centre and passes others") {
  constexpr double fs = 1000.0;
  constexpr std::size_t n = 8000;

  std::vector<float> at_centre;
  dsp::notch(fs, 100.0, 10.0).process(tone(fs, 100.0, n), at_centre);
  std::vector<float> below;
  dsp::notch(fs, 100.0, 10.0).process(tone(fs, 20.0, n), below);
  std::vector<float> above;
  dsp::notch(fs, 100.0, 10.0).process(tone(fs, 300.0, n), above);

  // Skip the settling transient, measure the steady state.
  CHECK(rms(std::span{at_centre}.subspan(2000)) < 0.02); // killed
  CHECK_THAT(rms(std::span{below}.subspan(2000)), WithinRel(1.0 / std::sqrt(2.0), 0.05)); // passed
  CHECK_THAT(rms(std::span{above}.subspan(2000)), WithinRel(1.0 / std::sqrt(2.0), 0.05)); // passed
}

TEST_CASE("higher Q gives a narrower notch") {
  constexpr double fs = 1000.0;
  constexpr std::size_t n = 8000;
  // A tone a little off-centre is rejected more by a wide (low-Q) notch than a
  // narrow (high-Q) one.
  std::vector<float> wide, narrow;
  dsp::notch(fs, 100.0, 2.0).process(tone(fs, 115.0, n), wide);
  dsp::notch(fs, 100.0, 30.0).process(tone(fs, 115.0, n), narrow);
  CHECK(rms(std::span{wide}.subspan(2000)) < rms(std::span{narrow}.subspan(2000)));
}

TEST_CASE("Biquad streams identically regardless of block size") {
  constexpr double fs = 1000.0;
  const auto x = tone(fs, 60.0, 2000);

  std::vector<float> whole;
  dsp::notch(fs, 100.0, 10.0).process(x, whole);

  std::vector<float> chunked;
  auto dut = dsp::notch(fs, 100.0, 10.0);
  for (std::size_t off = 0; off < x.size(); off += 83) // ragged blocks
    dut.process(std::span{x}.subspan(off, std::min<std::size_t>(83, x.size() - off)), chunked);

  REQUIRE(whole.size() == chunked.size());
  for (std::size_t k = 0; k < whole.size(); ++k)
    CHECK(whole[k] == chunked[k]);
}
