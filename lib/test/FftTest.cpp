#include "palindrome/fft.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace dsp = palindrome::dsp;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;

// The textbook O(N^2) DFT, the reference the fast transform must match.
std::vector<std::complex<double>> naive_dft(const std::vector<std::complex<double>> &x) {
  const auto n = x.size();
  std::vector<std::complex<double>> out(n);
  for (std::size_t k = 0; k < n; ++k) {
    std::complex<double> acc{0.0, 0.0};
    for (std::size_t j = 0; j < n; ++j)
      acc += x[j] * std::polar(1.0, -two_pi * static_cast<double>(k * j) / static_cast<double>(n));
    out[k] = acc;
  }
  return out;
}
} // namespace

TEST_CASE("fft rejects non-power-of-two sizes") {
  std::vector<std::complex<double>> x(6);
  CHECK_THROWS_AS(dsp::fft(x), std::invalid_argument);
  std::vector<std::complex<double>> ok(8);
  CHECK_NOTHROW(dsp::fft(ok)); // empty and size-1 are no-ops, power-of-two passes
  std::vector<std::complex<double>> empty;
  CHECK_NOTHROW(dsp::fft(empty));
}

TEST_CASE("fft matches the naive DFT") {
  for (const std::size_t n: {2u, 8u, 64u, 256u}) {
    std::vector<std::complex<double>> x(n);
    for (std::size_t k = 0; k < n; ++k) // a deterministic, non-trivial signal
      x[k] = std::complex<double>{std::sin(0.3 * static_cast<double>(k)) + 0.2 * static_cast<double>(k % 5),
          std::cos(0.11 * static_cast<double>(k))};
    const auto ref = naive_dft(x);
    dsp::fft(x);
    REQUIRE(x.size() == ref.size());
    for (std::size_t k = 0; k < n; ++k) {
      CHECK_THAT(x[k].real(), WithinAbs(ref[k].real(), 1e-9));
      CHECK_THAT(x[k].imag(), WithinAbs(ref[k].imag(), 1e-9));
    }
  }
}

TEST_CASE("fft of a pure bin lands all energy in that bin") {
  constexpr std::size_t n = 64;
  constexpr std::size_t bin = 7;
  std::vector<std::complex<double>> x(n);
  for (std::size_t k = 0; k < n; ++k)
    x[k] = std::polar(1.0, two_pi * static_cast<double>(bin * k) / static_cast<double>(n));
  dsp::fft(x);
  for (std::size_t k = 0; k < n; ++k)
    CHECK_THAT(std::abs(x[k]), WithinAbs(k == bin ? static_cast<double>(n) : 0.0, 1e-9));
}
