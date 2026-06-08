#include "palindrome/phase.hpp"

#include <numbers>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using palindrome::dsp::wrap_angle;
using palindrome::dsp::wrap_error;

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
