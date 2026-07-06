#include "palindrome/pow01.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <catch2/catch_test_macros.hpp>

using palindrome::dsp::pow01;

namespace {
// The reference side of the 8-bit readout comparison, in double.
int as_byte(double v) { return static_cast<int>(255.0 * v + 0.5); }
// The pow01 side goes through the exact float expression the shipped kernel
// uses (screen.cpp's readout_encoded), so the test pins the real output byte -
// a double round here could mask a half-integer knife edge the kernel crosses.
int as_byte_kernel(float v) { return static_cast<std::uint8_t>(255.0f * v + 0.5f); }
} // namespace

TEST_CASE("pow01 endpoints are exact") {
  for (const float e: {1.0f / 2.2f, 1.0f / 2.6f, 0.5f, 1.0f, 2.2f}) {
    CHECK(pow01(1.0f, e) == 1.0f);
    CHECK(as_byte_kernel(pow01(0.0f, e)) == 0);
    // -0.0 passes a [0, 1] clamp; without log2_poly's sign strip it would
    // read as an exponent of +129 and quantise black to full white.
    CHECK(as_byte_kernel(pow01(-0.0f, e)) == 0);
  }
}

TEST_CASE("pow01 reads tiny and subnormal inputs as black") {
  // Below ~2^(-40/e) the range clamp takes over, and subnormals mis-read as
  // ~2^-127 inside log2_poly - both must still land on byte 0 at the readout.
  for (const float e: {1.0f / 2.8f, 1.0f / 2.2f, 1.0f}) {
    CHECK(as_byte_kernel(pow01(0x1p-126f, e)) == 0); // smallest normal
    CHECK(as_byte_kernel(pow01(0x1p-149f, e)) == 0); // smallest subnormal
    CHECK(as_byte_kernel(pow01(0x1p-60f, e)) == 0); // below any readout LSB
  }
}

TEST_CASE("pow01 stays within one 8-bit LSB of double pow across the unit interval") {
  // Uniform sweep for the bulk, log-spaced for the deep blacks where the
  // gamma curve is steepest (the region the readout actually cares about).
  // 8.0 is the top of the header's advertised exponent range.
  for (const float e: {1.0f / 2.8f, 1.0f / 2.2f, 1.0f / 1.4f, 1.0f, 2.2f, 8.0f}) {
    int worst = 0;
    for (std::size_t i = 0; i <= 100000; ++i) {
      const auto x = static_cast<float>(i) / 100000.0f;
      const int want = as_byte(std::pow(static_cast<double>(x), static_cast<double>(e)));
      const int got = as_byte_kernel(pow01(x, e));
      worst = std::max(worst, std::abs(got - want));
    }
    for (int k = 1; k <= 4000; ++k) {
      const auto x = static_cast<float>(std::exp2(-30.0 * k / 4000.0));
      const int want = as_byte(std::pow(static_cast<double>(x), static_cast<double>(e)));
      const int got = as_byte_kernel(pow01(x, e));
      worst = std::max(worst, std::abs(got - want));
    }
    CHECK(worst <= 1);
  }
}

TEST_CASE("pow01 is monotonic in x") {
  // A readout must never invert two brightness levels, whatever the poly
  // rounding does between table-of-truth points.
  const float e = 1.0f / 2.2f;
  float prev = 0.0f;
  for (std::size_t i = 0; i <= 200000; ++i) {
    const auto x = static_cast<float>(i) / 200000.0f;
    const float v = pow01(x, e);
    CHECK(v >= prev);
    prev = v;
  }
}
