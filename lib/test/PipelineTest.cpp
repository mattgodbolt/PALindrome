#include "palindrome/pipeline.hpp"

#include "palindrome/biquad.hpp"
#include "palindrome/dc_blocker.hpp"
#include "palindrome/fir.hpp"

#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace dsp = palindrome::dsp;
using Catch::Matchers::WithinAbs;
using palindrome::Chain;

namespace {
std::vector<float> ramp(std::size_t n) {
  std::vector<float> x(n);
  for (std::size_t k = 0; k < n; ++k)
    x[k] = static_cast<float>(k) - static_cast<float>(n) / 2.0f; // some DC + slope
  return x;
}
} // namespace

TEST_CASE("an empty Chain passes input straight through") {
  Chain chain;
  CHECK(chain.empty());
  const std::vector<float> x{1.0f, 2.0f, 3.0f};
  const std::span<const float> out = chain.process(x);
  REQUIRE(out.size() == x.size());
  CHECK(out[1] == 2.0f);
  CHECK(chain.max_output_for(99) == 99);
}

TEST_CASE("a Chain reproduces the same stages run by hand") {
  constexpr double fs = 1000.0;
  const auto x = ramp(2000);

  // By hand: DC blocker then a decimating low-pass FIR.
  dsp::DcBlocker dc;
  dsp::Fir fir{dsp::lowpass_kernel(31, fs, 100.0), 2};
  dc.prepare(x.size());
  fir.prepare(x.size()); // DcBlocker is 1:1, so its output is x.size() wide
  std::vector<float> expected;
  {
    const std::span<const float> a = dc.process(x);
    const std::span<const float> b = fir.process(a);
    expected.assign(b.begin(), b.end());
  }

  Chain chain;
  chain.add(dsp::DcBlocker{});
  chain.add(dsp::Fir{dsp::lowpass_kernel(31, fs, 100.0), 2});
  chain.prepare(x.size());
  const std::span<const float> got = chain.process(x);

  CHECK(got.size() <= chain.max_output_for(x.size()));
  REQUIRE(got.size() == expected.size());
  for (std::size_t k = 0; k < got.size(); ++k)
    CHECK_THAT(got[k], WithinAbs(expected[k], 1e-6));
}

TEST_CASE("a Chain streams identically regardless of block size") {
  constexpr double fs = 1000.0;
  const auto x = ramp(1500);

  Chain whole;
  whole.add(dsp::notch(fs, 100.0, 5.0));
  whole.add(dsp::Fir{dsp::lowpass_kernel(31, fs, 100.0), 3});
  whole.prepare(x.size());
  const std::span<const float> ref = whole.process(x);
  const std::vector<float> expected{ref.begin(), ref.end()};

  Chain chunked;
  chunked.add(dsp::notch(fs, 100.0, 5.0));
  chunked.add(dsp::Fir{dsp::lowpass_kernel(31, fs, 100.0), 3});
  constexpr std::size_t chunk = 64;
  chunked.prepare(chunk);
  std::vector<float> got;
  for (std::size_t off = 0; off < x.size(); off += chunk) {
    const auto block = std::span{x}.subspan(off, std::min(chunk, x.size() - off));
    const std::span<const float> piece = chunked.process(block);
    got.insert(got.end(), piece.begin(), piece.end());
  }

  REQUIRE(got.size() == expected.size());
  for (std::size_t k = 0; k < got.size(); ++k)
    CHECK(got[k] == expected[k]);
}
