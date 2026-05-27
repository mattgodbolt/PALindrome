#include "palindrome/mixer.hpp"

#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace dsp = palindrome::dsp;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;

// The mixer's outputs are bounded by ~1 (unit-modulus phasor times a [-1, 1]
// input), so the rounding floor for a float result is one ULP at unit scale,
// i.e. FLT_EPSILON. Both comparisons below differ only by such rounding, so a
// few epsilons is the principled tolerance; measured worst case is exactly 1.
constexpr float rounding_floor = 4 * std::numeric_limits<float>::epsilon();

std::vector<float> tone(double fs, double freq, std::size_t n) {
  std::vector<float> x(n);
  for (std::size_t k = 0; k < n; ++k)
    x[k] = static_cast<float>(std::sin(two_pi * freq * static_cast<double>(k) / fs));
  return x;
}

// The textbook scalar down-converter the block oscillator must reproduce.
void reference_mix(std::span<const float> in, double fs, double carrier, std::vector<float> &i, std::vector<float> &q) {
  const double omega = two_pi * carrier / fs;
  for (std::size_t n = 0; n < in.size(); ++n) {
    const auto phasor = std::polar(1.0, -omega * static_cast<double>(n));
    i.push_back(static_cast<float>(in[n] * phasor.real()));
    q.push_back(static_cast<float>(in[n] * phasor.imag()));
  }
}
} // namespace

TEST_CASE("Mixer matches a scalar reference down-converter") {
  constexpr double fs = 32.0e6;
  constexpr double carrier = 3.5688e6;
  const auto x = tone(fs, 4.0e6, 5000);

  std::vector<float> ri, rq;
  reference_mix(x, fs, carrier, ri, rq);

  std::vector<float> i, q;
  dsp::Mixer{carrier, fs}.process(x, i, q);

  REQUIRE(i.size() == ri.size());
  REQUIRE(q.size() == rq.size());
  // The block oscillator mixes in float; the reference mixes in double and
  // rounds at the end. They therefore agree to the float rounding floor.
  for (std::size_t k = 0; k < i.size(); ++k) {
    CHECK_THAT(i[k], WithinAbs(ri[k], rounding_floor));
    CHECK_THAT(q[k], WithinAbs(rq[k], rounding_floor));
  }
}

TEST_CASE("Mixer shifts the carrier to DC") {
  constexpr double fs = 32.0e6;
  constexpr double carrier = 3.5688e6;
  // A pure carrier-frequency cosine splits into a DC term (amplitude 1/2) and a
  // term at 2*carrier. Down-conversion is correct if averaging the baseband I/Q
  // recovers the stationary DC phasor of magnitude 1/2 while the 2*carrier term
  // averages away.
  std::vector<float> x(40000);
  for (std::size_t n = 0; n < x.size(); ++n)
    x[n] = static_cast<float>(std::cos(two_pi * carrier * static_cast<double>(n) / fs));

  std::vector<float> i, q;
  dsp::Mixer{carrier, fs}.process(x, i, q);

  double mean_i = 0.0, mean_q = 0.0;
  for (std::size_t n = 0; n < i.size(); ++n) {
    mean_i += i[n];
    mean_q += q[n];
  }
  mean_i /= static_cast<double>(i.size());
  mean_q /= static_cast<double>(q.size());
  CHECK_THAT(std::hypot(mean_i, mean_q), WithinAbs(0.5, 0.01));
}

TEST_CASE("Mixer streams to within float rounding regardless of block size") {
  constexpr double fs = 32.0e6;
  constexpr double carrier = 3.5688e6;
  const auto x = tone(fs, 4.0e6, 4000);

  std::vector<float> whole_i, whole_q;
  dsp::Mixer{carrier, fs}.process(x, whole_i, whole_q);

  std::vector<float> chunk_i, chunk_q;
  dsp::Mixer dut{carrier, fs};
  for (std::size_t off = 0; off < x.size(); off += 91) // ragged blocks straddle the group
    dut.process(std::span{x}.subspan(off, std::min<std::size_t>(91, x.size() - off)), chunk_i, chunk_q);

  REQUIRE(whole_i.size() == chunk_i.size());
  // Not bit-exact: a full group advances the phase by step^kLanes, a tail one
  // step at a time, and those round differently across block boundaries. The
  // signals cross zero, so bound the absolute difference at the rounding floor
  // rather than in ULPs (which blow up near a zero crossing).
  for (std::size_t k = 0; k < whole_i.size(); ++k) {
    CHECK_THAT(chunk_i[k], WithinAbs(whole_i[k], rounding_floor));
    CHECK_THAT(chunk_q[k], WithinAbs(whole_q[k], rounding_floor));
  }
}
