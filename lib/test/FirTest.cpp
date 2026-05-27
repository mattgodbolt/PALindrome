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
  const std::span<const float> out = fir.process(impulse);

  REQUIRE(out.size() == impulse.size());
  for (std::size_t k = 0; k < taps.size(); ++k)
    CHECK(out[k] == taps[k]);
}

TEST_CASE("Fir passes DC at unity gain") {
  const auto taps = dsp::lowpass_kernel(31, 1000.0, 100.0);
  dsp::Fir fir{taps};
  const std::vector<float> dc(200, 1.0f);
  const std::span<const float> out = fir.process(dc);
  CHECK_THAT(out.back(), WithinAbs(1.0, 1e-5));
}

TEST_CASE("Fir passes in-band tones and rejects out-of-band tones") {
  constexpr double fs = 1000.0;
  constexpr std::size_t n = 4000;
  dsp::Fir fir{dsp::lowpass_kernel(101, fs, 100.0)};
  const std::span<const float> pass_out = fir.process(tone(fs, 20.0, n)); // well inside the passband

  dsp::Fir stop_fir{dsp::lowpass_kernel(101, fs, 100.0)};
  const std::span<const float> stop_out = stop_fir.process(tone(fs, 300.0, n)); // deep in the stopband

  CHECK_THAT(rms(pass_out.subspan(200)), WithinRel(1.0 / std::sqrt(2.0), 0.02)); // ~unchanged
  CHECK(rms(stop_out.subspan(200)) < 0.02); // strongly attenuated
}

TEST_CASE("Fir streams identically regardless of block size") {
  constexpr double fs = 1000.0;
  const auto x = tone(fs, 40.0, 2000);
  const auto taps = dsp::lowpass_kernel(57, fs, 120.0);

  dsp::Fir whole_fir{taps};
  const std::span<const float> whole = whole_fir.process(x);

  std::vector<float> chunked;
  dsp::Fir dut{taps};
  for (std::size_t off = 0; off < x.size(); off += 91) { // ragged blocks
    const std::span<const float> piece =
        dut.process(std::span{x}.subspan(off, std::min<std::size_t>(91, x.size() - off)));
    chunked.insert(chunked.end(), piece.begin(), piece.end());
  }

  REQUIRE(whole.size() == chunked.size());
  for (std::size_t k = 0; k < whole.size(); ++k)
    CHECK(whole[k] == chunked[k]);
}

TEST_CASE("decimating FIR keeps exactly every Nth full-rate output") {
  constexpr double fs = 1000.0;
  const auto taps = dsp::lowpass_kernel(31, fs, 100.0);
  const auto x = tone(fs, 40.0, 500);

  dsp::Fir full_fir{taps, 1};
  const std::span<const float> full = full_fir.process(x);
  dsp::Fir dut{taps, 3};
  const std::span<const float> dec = dut.process(x);

  CHECK(dut.decimation() == 3);
  REQUIRE(dec.size() == (full.size() + 2) / 3); // ceil(500 / 3)
  for (std::size_t k = 0; k < dec.size(); ++k)
    CHECK(dec[k] == full[3 * k]); // a decimating FIR == filter then keep every 3rd
}

TEST_CASE("decimating FIR streams identically regardless of block size") {
  constexpr double fs = 1000.0;
  const auto taps = dsp::lowpass_kernel(31, fs, 100.0);
  const auto x = tone(fs, 50.0, 777); // not a multiple of the decimation or block size

  dsp::Fir whole_fir{taps, 4};
  const std::span<const float> whole = whole_fir.process(x);

  std::vector<float> chunked;
  dsp::Fir dut{taps, 4};
  for (std::size_t off = 0; off < x.size(); off += 50) { // ragged blocks straddle the phase
    const std::span<const float> piece =
        dut.process(std::span{x}.subspan(off, std::min<std::size_t>(50, x.size() - off)));
    chunked.insert(chunked.end(), piece.begin(), piece.end());
  }

  REQUIRE(whole.size() == chunked.size());
  for (std::size_t k = 0; k < whole.size(); ++k)
    CHECK(whole[k] == chunked[k]);
}

TEST_CASE("max_output_for is a phase-independent upper bound on every call") {
  const auto taps = dsp::lowpass_kernel(31, 1000.0, 100.0);
  dsp::Fir dut{taps, 3}; // decimating, so phase carries between calls
  CHECK(dut.input_multiple() == 3);
  CHECK(dut.max_output_for(30) == 10); // ceil(30 / 3)
  CHECK(dut.max_output_for(31) == 11);

  // No actual call, at any phase, may exceed the bound used to size storage.
  const auto x = tone(1000.0, 50.0, 1000);
  for (std::size_t off = 0; off < x.size(); off += 50) {
    const auto block = std::span{x}.subspan(off, std::min<std::size_t>(50, x.size() - off));
    CHECK(dut.process(block).size() <= dut.max_output_for(block.size()));
  }
}

TEST_CASE("prepare lets a budgeted block stream without changing the result") {
  const auto taps = dsp::lowpass_kernel(31, 1000.0, 100.0);
  const auto x = tone(1000.0, 40.0, 600);

  dsp::Fir plain{taps};
  const std::span<const float> ref = plain.process(x);

  dsp::Fir prepped{taps};
  prepped.prepare(x.size());
  const std::span<const float> got = prepped.process(x);

  REQUIRE(got.size() == ref.size());
  for (std::size_t k = 0; k < got.size(); ++k)
    CHECK(got[k] == ref[k]);
}

TEST_CASE("Fir rejects bad construction") {
  CHECK_THROWS_AS(dsp::Fir{std::vector<float>{}}, std::invalid_argument);
  CHECK_THROWS_AS((dsp::Fir{dsp::lowpass_kernel(5, 1000.0, 100.0), 0}), std::invalid_argument);
  CHECK(dsp::Fir{dsp::lowpass_kernel(5, 1000.0, 100.0)}.decimation() == 1); // default
}
