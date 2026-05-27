#include "palindrome/fir.hpp"

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

TEST_CASE("lowpass_kernel is unity-gain, symmetric, and validated") {
  const auto k = dsp::lowpass_kernel(65, 1000.0, 100.0);
  REQUIRE(k.size() == 65);

  double sum = 0.0;
  for (const float t: k)
    sum += t;
  CHECK_THAT(sum, WithinAbs(1.0, 1e-5)); // unity DC gain

  for (std::size_t i = 0; i < k.size() / 2; ++i)
    CHECK_THAT(k[i], WithinAbs(k[k.size() - 1 - i], 1e-6)); // symmetric => linear phase

  CHECK_THROWS_AS(dsp::lowpass_kernel(0, 1000.0, 100.0), std::invalid_argument);
  CHECK_THROWS_AS(dsp::lowpass_kernel(65, 1000.0, 0.0), std::invalid_argument);
  CHECK_THROWS_AS(dsp::lowpass_kernel(65, 1000.0, 600.0), std::invalid_argument);
}

TEST_CASE("Fir convolves an impulse to its kernel") {
  const auto taps = dsp::lowpass_kernel(31, 1000.0, 100.0);
  dsp::Fir fir{taps};

  std::vector<float> impulse(64, 0.0f);
  impulse[0] = 1.0f;
  std::vector<float> out;
  fir.process(impulse, out);

  REQUIRE(out.size() == impulse.size());
  for (std::size_t k = 0; k < taps.size(); ++k)
    CHECK(out[k] == taps[k]);
}

TEST_CASE("Fir passes DC at unity gain") {
  const auto taps = dsp::lowpass_kernel(31, 1000.0, 100.0);
  dsp::Fir fir{taps};
  const std::vector<float> dc(200, 1.0f);
  std::vector<float> out;
  fir.process(dc, out);
  CHECK_THAT(out.back(), WithinAbs(1.0, 1e-5));
}

TEST_CASE("Fir passes in-band tones and rejects out-of-band tones") {
  constexpr double fs = 1000.0;
  constexpr std::size_t n = 4000;
  dsp::Fir fir{dsp::lowpass_kernel(101, fs, 100.0)};

  std::vector<float> pass_out;
  fir.process(tone(fs, 20.0, n), pass_out); // well inside the passband
  std::vector<float> stop_out;
  dsp::Fir{dsp::lowpass_kernel(101, fs, 100.0)}.process(tone(fs, 300.0, n), stop_out); // deep in the stopband

  const auto settled = std::span{pass_out}.subspan(200);
  CHECK_THAT(rms(settled), WithinRel(1.0 / std::sqrt(2.0), 0.02)); // ~unchanged
  CHECK(rms(std::span{stop_out}.subspan(200)) < 0.02); // strongly attenuated
}

TEST_CASE("Fir streams identically regardless of block size") {
  constexpr double fs = 1000.0;
  const auto x = tone(fs, 40.0, 2000);
  const auto taps = dsp::lowpass_kernel(57, fs, 120.0);

  std::vector<float> whole;
  dsp::Fir{taps}.process(x, whole);

  std::vector<float> chunked;
  dsp::Fir dut{taps};
  for (std::size_t off = 0; off < x.size(); off += 91) // ragged blocks
    dut.process(std::span{x}.subspan(off, std::min<std::size_t>(91, x.size() - off)), chunked);

  REQUIRE(whole.size() == chunked.size());
  for (std::size_t k = 0; k < whole.size(); ++k)
    CHECK(whole[k] == chunked[k]);
}

TEST_CASE("decimating FIR keeps exactly every Nth full-rate output") {
  constexpr double fs = 1000.0;
  const auto taps = dsp::lowpass_kernel(31, fs, 100.0);
  const auto x = tone(fs, 40.0, 500);

  std::vector<float> full;
  dsp::Fir{taps, 1}.process(x, full);
  std::vector<float> dec;
  dsp::Fir dut{taps, 3};
  dut.process(x, dec);

  CHECK(dut.decimation() == 3);
  REQUIRE(dec.size() == (full.size() + 2) / 3); // ceil(500 / 3)
  for (std::size_t k = 0; k < dec.size(); ++k)
    CHECK(dec[k] == full[3 * k]); // a decimating FIR == filter then keep every 3rd
}

TEST_CASE("decimating FIR streams identically regardless of block size") {
  constexpr double fs = 1000.0;
  const auto taps = dsp::lowpass_kernel(31, fs, 100.0);
  const auto x = tone(fs, 50.0, 777); // not a multiple of the decimation or block size

  std::vector<float> whole;
  dsp::Fir{taps, 4}.process(x, whole);

  std::vector<float> chunked;
  dsp::Fir dut{taps, 4};
  for (std::size_t off = 0; off < x.size(); off += 50) // ragged blocks straddle the phase
    dut.process(std::span{x}.subspan(off, std::min<std::size_t>(50, x.size() - off)), chunked);

  REQUIRE(whole.size() == chunked.size());
  for (std::size_t k = 0; k < whole.size(); ++k)
    CHECK(whole[k] == chunked[k]);
}

TEST_CASE("Fir rejects bad construction") {
  CHECK_THROWS_AS(dsp::Fir{std::vector<float>{}}, std::invalid_argument);
  CHECK_THROWS_AS((dsp::Fir{dsp::lowpass_kernel(5, 1000.0, 100.0), 0}), std::invalid_argument);
  CHECK(dsp::Fir{dsp::lowpass_kernel(5, 1000.0, 100.0)}.decimation() == 1); // default
}
