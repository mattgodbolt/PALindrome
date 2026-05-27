#include "palindrome/biquad.hpp"
#include "palindrome/dc_blocker.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/fir.hpp"

#include <cstddef>
#include <random>
#include <span>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

namespace dsp = palindrome::dsp;
namespace demod = palindrome::demod;

namespace {
// Representative PAL capture parameters (see the corpus .sigmf-meta files).
constexpr double fs = 32.0e6;
constexpr double carrier = 3.5688e6;
constexpr double cutoff = 5.0e6;
constexpr double sound_if = 9.569e6;
// Matches the CLI's read size; each benchmarked call processes this many samples,
// so mean-time / block gives throughput (block / mean = samples per second).
constexpr std::size_t block = std::size_t{1} << 16;

std::vector<float> noise(std::size_t n) {
  std::mt19937 rng{0xC0FFEE};
  std::uniform_real_distribution<float> dist{-1.0f, 1.0f};
  std::vector<float> x(n);
  for (float &v: x)
    v = dist(rng);
  return x;
}
} // namespace

// 65536 samples per call, so e.g. a 400 us mean = 65536 / 400us ~= 164 MS/s.
TEST_CASE("demod throughput (64k-sample blocks)") {
  const auto input = noise(block);

  BENCHMARK("AmEnvelope::process (mix + FIR x2 + magnitude)") {
    static demod::AmEnvelope env = [] {
      demod::AmEnvelope e{fs, carrier, cutoff};
      e.prepare(block);
      return e;
    }();
    return env.process(input).size();
  };

  BENCHMARK("full pre-detection + envelope (sound trap, DC block, envelope)") {
    static dsp::Biquad trap = [] {
      dsp::Biquad b = dsp::notch(fs, sound_if, 10.0);
      b.prepare(block);
      return b;
    }();
    static dsp::DcBlocker dc = [] {
      dsp::DcBlocker d;
      d.prepare(block);
      return d;
    }();
    static demod::AmEnvelope env = [] {
      demod::AmEnvelope e{fs, carrier, cutoff};
      e.prepare(block);
      return e;
    }();
    return env.process(dc.process(trap.process(input))).size();
  };
}
