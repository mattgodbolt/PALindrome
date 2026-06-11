#include "palindrome/demod.hpp"

#include <cstddef>
#include <random>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

namespace demod = palindrome::demod;

namespace {
// Representative AirSpy raw-real capture parameters (see corpus/wb3_airspy.sigmf-meta):
// real IF at 20 MS/s, vision ~3.1 MHz, chroma-passing cutoff.
constexpr double fs = 20.0e6;
constexpr double carrier = 3.1e6;
constexpr double cutoff = 4.8e6;
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

  // The analytic front end on its own: a Hilbert FIR (half its taps zero) + the
  // matched in-phase delay. Compare against the full path below to see its share.
  BENCHMARK("Hilbert::process (analytic FIR + matched delay)") {
    static demod::Hilbert hilbert = [] {
      demod::Hilbert h;
      h.prepare(block);
      return h;
    }();
    return hilbert.process(input).size();
  };

  // The whole real-IF envelope: Hilbert -> ComplexAmEnvelope (DC block, mix to DC,
  // FIR low-pass x2, magnitude). This is what stream_envelope runs per block in
  // --if flat.
  BENCHMARK("real envelope path (Hilbert -> ComplexAmEnvelope)") {
    static demod::Hilbert hilbert = [] {
      demod::Hilbert h;
      h.prepare(block);
      return h;
    }();
    static demod::ComplexAmEnvelope env = [] {
      demod::ComplexAmEnvelope e{fs, carrier, cutoff};
      e.prepare(block);
      return e;
    }();
    return env.process(hilbert.process(input)).size();
  };

  // The SAW-era front end: one complex-tap FIR on the real IF + a detector.
  // Envelope is pure feed-forward (more MACs than the flat chain, no serial
  // recurrence - a wash); quasi-sync (the default) adds the per-sample phase
  // loop, a serial recurrence like the old mix. Compare per-sample here, where
  // the screen can't hide it.
  BENCHMARK("SAW IF path (VisionIf saw80, envelope)") {
    static demod::VisionIf vif = [] {
      demod::VisionIf v{fs, carrier, demod::saw80_template(), demod::Detector::envelope};
      v.prepare(block);
      return v;
    }();
    return vif.process(input).size();
  };

  BENCHMARK("SAW IF path (VisionIf saw80, quasi-sync)") {
    static demod::VisionIf vif = [] {
      demod::VisionIf v{fs, carrier, demod::saw80_template(), demod::Detector::quasi_sync};
      v.prepare(block);
      return v;
    }();
    return vif.process(input).size();
  };
}
