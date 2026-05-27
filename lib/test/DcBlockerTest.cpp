#include "palindrome/dc_blocker.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace dsp = palindrome::dsp;
using Catch::Matchers::WithinRel;

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;

std::vector<float> tone(double fs, double freq, std::size_t n, float offset = 0.0f) {
  std::vector<float> x(n);
  for (std::size_t k = 0; k < n; ++k)
    x[k] = offset + static_cast<float>(std::sin(two_pi * freq * static_cast<double>(k) / fs));
  return x;
}

double rms(std::span<const float> s) {
  double energy = 0.0;
  for (const float v: s)
    energy += static_cast<double>(v) * v;
  return std::sqrt(energy / static_cast<double>(s.size()));
}
} // namespace

TEST_CASE("DcBlocker rejects an out-of-range pole") {
  CHECK_THROWS_AS(dsp::DcBlocker{0.0}, std::invalid_argument);
  CHECK_THROWS_AS(dsp::DcBlocker{1.0}, std::invalid_argument);
  CHECK_THROWS_AS(dsp::DcBlocker{-0.5}, std::invalid_argument);
}

TEST_CASE("DcBlocker removes a constant offset") {
  // A faster pole (corner ~160 Hz at 1 MS/s) settles within these samples.
  dsp::DcBlocker dut{0.999};
  const std::vector<float> dc(20000, 0.7f);
  const std::span<const float> out = dut.process(dc);
  // Once settled, the steady output of a constant input is essentially zero.
  CHECK(std::abs(out.back()) < 1e-3f);
}

TEST_CASE("DcBlocker passes a tone well above the corner") {
  constexpr double fs = 1.0e6;
  constexpr std::size_t n = 20000;
  // A tone with a fat DC pedestal: the tone survives, the pedestal does not.
  dsp::DcBlocker dut{0.999};
  const std::span<const float> out = dut.process(tone(fs, 50.0e3, n, 0.5f));
  const auto tail = out.subspan(n / 2);
  CHECK_THAT(rms(tail), WithinRel(1.0 / std::sqrt(2.0), 0.01)); // unit-amplitude sine RMS
  const double mean = std::ranges::fold_left(tail, 0.0, std::plus{}) / static_cast<double>(tail.size());
  CHECK(std::abs(mean) < 1e-3); // pedestal gone
}

TEST_CASE("DcBlocker streams identically regardless of block size") {
  constexpr double fs = 1.0e6;
  const auto x = tone(fs, 30.0e3, 4096, 0.3f);

  dsp::DcBlocker whole_dut;
  const std::span<const float> whole = whole_dut.process(x);

  std::vector<float> chunked;
  dsp::DcBlocker dut;
  for (std::size_t off = 0; off < x.size(); off += 97) { // ragged blocks
    const std::span<const float> piece =
        dut.process(std::span{x}.subspan(off, std::min<std::size_t>(97, x.size() - off)));
    chunked.insert(chunked.end(), piece.begin(), piece.end());
  }

  REQUIRE(whole.size() == chunked.size());
  for (std::size_t k = 0; k < whole.size(); ++k)
    CHECK(whole[k] == chunked[k]);
}
