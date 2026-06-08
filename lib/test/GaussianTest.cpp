#include "palindrome/gaussian.hpp"

#include <numeric>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using palindrome::dsp::gaussian_splat_lut;
using palindrome::dsp::splat_radius_for;

TEST_CASE("splat_radius_for covers the Gaussian out to 2.5 sigma") {
  CHECK(splat_radius_for(0.0) == 0); // a bare point
  CHECK(splat_radius_for(0.8) == 2); // ceil(2.0)
  CHECK(splat_radius_for(1.0) == 3); // ceil(2.5)
  CHECK(splat_radius_for(2.0) == 5); // ceil(5.0)
}

TEST_CASE("gaussian_splat_lut conserves charge: every bin's weights sum to 1") {
  constexpr std::size_t bins = 64;
  const double sigma = 0.8;
  const auto radius = splat_radius_for(sigma);
  const auto lut = gaussian_splat_lut(sigma, radius, bins);
  const std::size_t stride = 2 * radius + 1;
  REQUIRE(lut.size() == bins * stride);
  for (std::size_t bin = 0; bin < bins; ++bin) {
    const double sum = std::accumulate(lut.begin() + static_cast<std::ptrdiff_t>(bin * stride),
        lut.begin() + static_cast<std::ptrdiff_t>((bin + 1) * stride), 0.0);
    CHECK_THAT(sum, WithinAbs(1.0, 1e-6));
  }
}

TEST_CASE("gaussian_splat_lut with radius 0 is a single unit weight (a bare point)") {
  const auto lut = gaussian_splat_lut(0.0, 0, 16);
  REQUIRE(lut.size() == 16); // 16 bins * stride 1
  for (const float w: lut)
    CHECK_THAT(w, WithinAbs(1.0f, 1e-6f));
}
