#include "palindrome/demod.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <ranges>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace demod = palindrome::demod;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinULP;

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;

// A real carrier of amplitude `amp`, amplitude-modulated by a tone of `mod_hz`
// at depth `depth`: amp * (1 + depth*cos(2*pi*mod_hz*t)) * cos(2*pi*carrier*t).
std::vector<float> am_signal(double fs, double carrier, double mod_hz, double depth, double amp, std::size_t n) {
  std::vector<float> x(n);
  for (std::size_t k = 0; k < n; ++k) {
    const double t = static_cast<double>(k) / fs;
    const double envelope = amp * (1.0 + depth * std::cos(two_pi * mod_hz * t));
    x[k] = static_cast<float>(envelope * std::cos(two_pi * carrier * t));
  }
  return x;
}
} // namespace

TEST_CASE("AmEnvelope rejects nonsensical parameters") {
  CHECK_THROWS_AS(demod::AmEnvelope(0.0, 1000.0, 100.0), std::invalid_argument);
  CHECK_THROWS_AS(demod::AmEnvelope(1000.0, 100.0, 0.0), std::invalid_argument);
  CHECK_THROWS_AS(demod::AmEnvelope(1000.0, 100.0, 600.0), std::invalid_argument); // cutoff > fs/2
}

TEST_CASE("AmEnvelope recovers the amplitude of an unmodulated carrier") {
  constexpr double fs = 1.0e6;
  constexpr double carrier = 100.0e3;
  constexpr double amp = 0.5;
  const auto x = am_signal(fs, carrier, 0.0, 0.0, amp, 5000);

  demod::AmEnvelope dut{fs, carrier, 10.0e3};
  dut.prepare(x.size());
  const std::span<const float> out = dut.process(x);

  // After the filter settles, the envelope should sit at the carrier amplitude.
  const auto tail = out.subspan(out.size() / 2);
  const double mean = std::ranges::fold_left(tail, 0.0, std::plus{}) / static_cast<double>(tail.size());
  CHECK_THAT(mean, WithinAbs(amp, 0.02));
}

TEST_CASE("AmEnvelope recovers a modulating tone") {
  constexpr double fs = 1.0e6;
  constexpr double carrier = 200.0e3;
  constexpr double mod_hz = 2.0e3;
  constexpr double depth = 0.5;
  constexpr double amp = 1.0;
  const auto x = am_signal(fs, carrier, mod_hz, depth, amp, 20000);

  // Carrier well above cutoff so the one-pole rejects the 2*carrier image.
  demod::AmEnvelope dut{fs, carrier, 10.0e3};
  dut.prepare(x.size());
  const std::span<const float> out = dut.process(x);

  // Over the settled tail the envelope should swing about `amp` with roughly
  // `depth` modulation, i.e. between amp*(1-depth) and amp*(1+depth).
  const auto tail = out.subspan(out.size() / 2);
  const auto [lo, hi] = std::ranges::minmax(tail);
  const double mean = std::ranges::fold_left(tail, 0.0, std::plus{}) / static_cast<double>(tail.size());
  CHECK_THAT(mean, WithinAbs(amp, 0.05));
  CHECK_THAT(lo, WithinAbs(amp * (1.0 - depth), 0.08));
  CHECK_THAT(hi, WithinAbs(amp * (1.0 + depth), 0.08));
}

TEST_CASE("AmEnvelope streams identically regardless of block size") {
  constexpr double fs = 1.0e6;
  constexpr double carrier = 80.0e3;
  const auto x = am_signal(fs, carrier, 1.0e3, 0.4, 0.7, 4096);

  demod::AmEnvelope whole_dut{fs, carrier, 15.0e3};
  whole_dut.prepare(x.size());
  const std::span<const float> whole = whole_dut.process(x);

  std::vector<float> chunked;
  demod::AmEnvelope dut{fs, carrier, 15.0e3};
  constexpr std::size_t chunk = 97; // deliberately ragged blocks
  dut.prepare(chunk);
  for (std::size_t off = 0; off < x.size(); off += chunk) {
    const std::span<const float> piece = dut.process(std::span{x}.subspan(off, std::min(chunk, x.size() - off)));
    chunked.insert(chunked.end(), piece.begin(), piece.end());
  }

  REQUIRE(whole.size() == chunked.size());
  // The FIRs carry state bit-exactly, but the block oscillator (dsp::Mixer)
  // advances its phase differently across a block boundary than within a full
  // group, so the two runs agree only to float rounding. Unlike the mixer's I/Q
  // (which cross zero), the envelope is a strictly positive magnitude bounded
  // away from zero here, so ULPs are well defined: the worst case is 2 ULP.
  for (std::size_t k = 0; k < whole.size(); ++k)
    CHECK_THAT(chunked[k], WithinULP(whole[k], 4));
}
