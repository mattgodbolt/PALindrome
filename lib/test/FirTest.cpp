#include "palindrome/fir.hpp"

#include <cmath>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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
  for (const float v: s) {
    const auto dv = static_cast<double>(v);
    energy += dv * dv;
  }
  return std::sqrt(energy / static_cast<double>(s.size()));
}
} // namespace

TEST_CASE("lowpass_kernel is unity-gain, symmetric, and validated") {
  const auto k = dsp::lowpass_kernel(65, 1000.0, 100.0);
  REQUIRE(k.size() == 65);

  double sum = 0.0;
  for (const float t: k)
    sum += static_cast<double>(t);
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
  fir.prepare(impulse.size());
  const std::span<const float> out = fir.process(impulse);

  REQUIRE(out.size() == impulse.size());
  for (std::size_t k = 0; k < taps.size(); ++k)
    CHECK(out[k] == taps[k]);
}

TEST_CASE("Fir passes DC at unity gain") {
  const auto taps = dsp::lowpass_kernel(31, 1000.0, 100.0);
  dsp::Fir fir{taps};
  const std::vector<float> dc(200, 1.0f);
  fir.prepare(dc.size());
  const std::span<const float> out = fir.process(dc);
  CHECK_THAT(out.back(), WithinAbs(1.0, 1e-5));
}

TEST_CASE("Fir passes in-band tones and rejects out-of-band tones") {
  constexpr double fs = 1000.0;
  constexpr std::size_t n = 4000;
  dsp::Fir fir{dsp::lowpass_kernel(101, fs, 100.0)};
  fir.prepare(n);
  const std::span<const float> pass_out = fir.process(tone(fs, 20.0, n)); // well inside the passband

  dsp::Fir stop_fir{dsp::lowpass_kernel(101, fs, 100.0)};
  stop_fir.prepare(n);
  const std::span<const float> stop_out = stop_fir.process(tone(fs, 300.0, n)); // deep in the stopband

  CHECK_THAT(rms(pass_out.subspan(200)), WithinRel(1.0 / std::sqrt(2.0), 0.02)); // ~unchanged
  CHECK(rms(stop_out.subspan(200)) < 0.02); // strongly attenuated
}

TEST_CASE("Fir streams identically regardless of block size") {
  constexpr double fs = 1000.0;
  const auto x = tone(fs, 40.0, 2000);
  const auto taps = dsp::lowpass_kernel(57, fs, 120.0);

  dsp::Fir whole_fir{taps};
  whole_fir.prepare(x.size());
  const std::span<const float> whole = whole_fir.process(x);

  std::vector<float> chunked;
  dsp::Fir dut{taps};
  constexpr std::size_t chunk = 91; // ragged blocks straddle the group boundary
  dut.prepare(chunk);
  for (std::size_t off = 0; off < x.size(); off += chunk) {
    const std::span<const float> piece = dut.process(std::span{x}.subspan(off, std::min(chunk, x.size() - off)));
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
  full_fir.prepare(x.size());
  const std::span<const float> full = full_fir.process(x);
  dsp::Fir dut{taps, 3};
  dut.prepare(x.size());
  const std::span<const float> dec = dut.process(x);

  CHECK(dut.decimation() == 3);
  REQUIRE(dec.size() == (full.size() + 2) / 3); // ceil(500 / 3)
  for (std::size_t k = 0; k < dec.size(); ++k)
    CHECK(dec[k] == full[3 * k]); // a decimating FIR == filter then keep every 3rd
}

TEST_CASE("decimation-2 FIR matches full-rate and streams identically (the deinterleave path)") {
  // Decimation 2 has its own AVX2 even-lane deinterleave kernel — the front end's
  // path — that neither the d=3 (scalar) nor d=4 case exercises. 500 inputs give
  // 250 outputs, so the 8-wide kernel runs and then hands its tail to the scalar
  // dot; both must agree with a full-rate filter sampled every 2nd output.
  constexpr double fs = 1000.0;
  const auto taps = dsp::lowpass_kernel(31, fs, 100.0);
  const auto x = tone(fs, 40.0, 500);

  dsp::Fir full_fir{taps, 1};
  full_fir.prepare(x.size());
  const std::span<const float> full = full_fir.process(x);
  dsp::Fir dut{taps, 2};
  dut.prepare(x.size());
  const std::span<const float> dec = dut.process(x);
  REQUIRE(dec.size() == (full.size() + 1) / 2);
  for (std::size_t k = 0; k < dec.size(); ++k)
    CHECK(dec[k] == full[2 * k]); // deinterleave == filter then keep every 2nd

  // Ragged blocks straddle the SIMD/scalar boundary differently each call, so an
  // identical stream proves the two kernels are bit-for-bit consistent.
  std::vector<float> chunked;
  dsp::Fir streamed{taps, 2};
  constexpr std::size_t chunk = 37; // straddles the SIMD/scalar boundary differently each call
  streamed.prepare(chunk);
  for (std::size_t off = 0; off < x.size(); off += chunk) {
    const std::span<const float> piece = streamed.process(std::span{x}.subspan(off, std::min(chunk, x.size() - off)));
    chunked.insert(chunked.end(), piece.begin(), piece.end());
  }
  REQUIRE(chunked.size() == dec.size());
  for (std::size_t k = 0; k < dec.size(); ++k)
    CHECK(chunked[k] == dec[k]);
}

TEST_CASE("decimating FIR streams identically regardless of block size") {
  constexpr double fs = 1000.0;
  const auto taps = dsp::lowpass_kernel(31, fs, 100.0);
  const auto x = tone(fs, 50.0, 777); // not a multiple of the decimation or block size

  dsp::Fir whole_fir{taps, 4};
  whole_fir.prepare(x.size());
  const std::span<const float> whole = whole_fir.process(x);

  std::vector<float> chunked;
  dsp::Fir dut{taps, 4};
  constexpr std::size_t chunk = 50; // ragged blocks straddle the decimation phase
  dut.prepare(chunk);
  for (std::size_t off = 0; off < x.size(); off += chunk) {
    const std::span<const float> piece = dut.process(std::span{x}.subspan(off, std::min(chunk, x.size() - off)));
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
  constexpr std::size_t chunk = 50;
  dut.prepare(chunk);
  for (std::size_t off = 0; off < x.size(); off += chunk) {
    const auto block = std::span{x}.subspan(off, std::min(chunk, x.size() - off));
    CHECK(dut.process(block).size() <= dut.max_output_for(block.size()));
  }
}

TEST_CASE("process throws on a block bigger than prepare budgeted") {
  // prepare() is the contract: the hot path doesn't grow, so an over-budget block
  // is a programming error, surfaced as a throw rather than a silent realloc.
  const auto taps = dsp::lowpass_kernel(31, 1000.0, 100.0);
  const auto x = tone(1000.0, 40.0, 600);

  dsp::Fir unprepared{taps}; // prepare() never called -> no capacity
  CHECK_THROWS_AS(unprepared.process(x), std::length_error);

  dsp::Fir tight{taps};
  tight.prepare(x.size() / 2); // budgeted for half; a full block overflows
  CHECK_THROWS_AS(tight.process(x), std::length_error);
}

TEST_CASE("notch_kernel passes DC and out-of-band tones, nulls the stop band") {
  constexpr double fs = 1000.0;
  constexpr std::size_t n = 4000;
  // A 101-tap notch around 100 Hz: the complement of a band-pass, so it passes
  // everything except that band.
  const auto taps = dsp::notch_kernel(101, fs, 80.0, 120.0);

  dsp::Fir dc_fir{taps};
  dc_fir.prepare(200);
  const std::vector<float> dc(200, 1.0f);
  CHECK_THAT(dc_fir.process(dc).back(), WithinAbs(1.0, 2e-3)); // ~unity DC gain (windowing leaks a little)

  dsp::Fir stop_fir{taps};
  stop_fir.prepare(n);
  const std::span<const float> in_band = stop_fir.process(tone(fs, 100.0, n));
  dsp::Fir pass_fir{taps};
  pass_fir.prepare(n);
  const std::span<const float> out_band = pass_fir.process(tone(fs, 300.0, n));

  CHECK(rms(in_band.subspan(200)) < 0.05); // nulled in the notch
  CHECK_THAT(rms(out_band.subspan(200)), WithinRel(1.0 / std::sqrt(2.0), 0.05)); // passed
}

TEST_CASE("Fir rejects bad construction") {
  CHECK_THROWS_AS(dsp::Fir{std::vector<float>{}}, std::invalid_argument);
  CHECK_THROWS_AS((dsp::Fir{dsp::lowpass_kernel(5, 1000.0, 100.0), 0}), std::invalid_argument);
  CHECK(dsp::Fir{dsp::lowpass_kernel(5, 1000.0, 100.0)}.decimation() == 1); // default
}

TEST_CASE("FirPair matches two independent Firs bit-exactly, whole and chunked") {
  // The fused pass shares the window sweep (and the d == 2 deinterleave)
  // between the two tap sets, but each output must still be the exact result
  // its own Fir would produce. The tap counts land the strip loops on all
  // three tiers (32-wide, 8-wide, scalar tail); the ragged chunking makes the
  // shared decimation phase straddle blocks.
  // 1 and 2 pin the polyphase tier's degenerate tap walks (trailing even tap
  // with an empty pair loop; exactly one even/odd pair).
  const auto num_taps = GENERATE(std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{64}, std::size_t{255});
  const auto decimation = GENERATE(std::size_t{1}, std::size_t{2}, std::size_t{3});
  const auto re_taps = dsp::lowpass_kernel(num_taps, 1000.0, 120.0);
  const auto im_taps = dsp::bandpass_kernel(num_taps, 1000.0, 60.0, 200.0);
  const auto x = tone(1000.0, 40.0, 2000);

  dsp::Fir re_ref{re_taps, decimation};
  dsp::Fir im_ref{im_taps, decimation};
  re_ref.prepare(x.size());
  im_ref.prepare(x.size());
  const std::span<const float> re_want = re_ref.process(x);
  const std::span<const float> im_want = im_ref.process(x);

  dsp::FirPair whole{re_taps, im_taps, decimation};
  whole.prepare(x.size());
  const auto got = whole.process(x);
  REQUIRE(got.re.size() == re_want.size());
  REQUIRE(got.im.size() == im_want.size());
  for (std::size_t k = 0; k < re_want.size(); ++k) {
    CHECK(got.re[k] == re_want[k]);
    CHECK(got.im[k] == im_want[k]);
  }

  std::vector<float> re_chunked;
  std::vector<float> im_chunked;
  dsp::FirPair dut{re_taps, im_taps, decimation};
  constexpr std::size_t chunk = 91; // ragged blocks straddle the decimation phase
  dut.prepare(chunk);
  for (std::size_t off = 0; off < x.size(); off += chunk) {
    const auto piece = dut.process(std::span{x}.subspan(off, std::min(chunk, x.size() - off)));
    re_chunked.insert(re_chunked.end(), piece.re.begin(), piece.re.end());
    im_chunked.insert(im_chunked.end(), piece.im.begin(), piece.im.end());
  }
  REQUIRE(re_chunked.size() == re_want.size());
  REQUIRE(im_chunked.size() == im_want.size());
  for (std::size_t k = 0; k < re_want.size(); ++k) {
    CHECK(re_chunked[k] == re_want[k]);
    CHECK(im_chunked[k] == im_want[k]);
  }
}

TEST_CASE("FirPair rejects bad construction") {
  const auto taps5 = dsp::lowpass_kernel(5, 1000.0, 100.0);
  const auto taps7 = dsp::lowpass_kernel(7, 1000.0, 100.0);
  CHECK_THROWS_AS((dsp::FirPair{std::vector<float>{}, std::vector<float>{}}), std::invalid_argument);
  CHECK_THROWS_AS((dsp::FirPair{taps5, taps7}), std::invalid_argument);
  CHECK_THROWS_AS((dsp::FirPair{taps5, taps5, 0}), std::invalid_argument);
  CHECK(dsp::FirPair{taps5, taps5}.decimation() == 1); // default
}
