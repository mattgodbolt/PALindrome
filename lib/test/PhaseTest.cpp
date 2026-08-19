#include "palindrome/phase.hpp"

#include <cmath>
#include <numbers>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using palindrome::dsp::wrap_angle;
using palindrome::dsp::wrap_error;
using palindrome::dsp::wrap_phase;

TEST_CASE("wrap_phase matches x - floor(x) bit-for-bit across (-1, 2)") {
  // The oscillators rely on the equivalence, not just closeness: a phase that
  // wandered by an ULP would break the renders' byte-identity.
  CHECK(wrap_phase(0.0) == 0.0);
  CHECK(wrap_phase(0.999) == 0.999);
  CHECK(wrap_phase(1.0) == 0.0);
  for (int i = -999; i < 2000; ++i) {
    const double x = i * 1e-3 + 3.0e-7; // off the grid, so the subtractions round
    CHECK(wrap_phase(x) == x - std::floor(x));
  }
}

TEST_CASE("wrap_error folds a cycle error into [-0.5, 0.5)") {
  CHECK_THAT(wrap_error(0.0), WithinAbs(0.0, 1e-12));
  CHECK_THAT(wrap_error(0.25), WithinAbs(0.25, 1e-12));
  CHECK_THAT(wrap_error(0.75), WithinAbs(-0.25, 1e-12)); // past the half wraps negative
  CHECK_THAT(wrap_error(0.5), WithinAbs(-0.5, 1e-12)); // the half itself is the low end
  // Whole cycles fall away, so only the fractional offset survives.
  CHECK_THAT(wrap_error(1.25), WithinAbs(0.25, 1e-12));
  CHECK_THAT(wrap_error(-0.25), WithinAbs(-0.25, 1e-12));
  CHECK_THAT(wrap_error(-1.75), WithinAbs(0.25, 1e-12));
}

TEST_CASE("wrap_angle folds radians into [-pi, pi)") {
  constexpr double pi = std::numbers::pi;
  CHECK_THAT(wrap_angle(0.0), WithinAbs(0.0, 1e-12));
  CHECK_THAT(wrap_angle(0.5), WithinAbs(0.5, 1e-12));
  CHECK_THAT(wrap_angle(-0.5), WithinAbs(-0.5, 1e-12));
  CHECK_THAT(wrap_angle(0.5 + 2.0 * pi), WithinAbs(0.5, 1e-9)); // a full turn falls away
  CHECK_THAT(wrap_angle(-0.5 - 2.0 * pi), WithinAbs(-0.5, 1e-9));
}
