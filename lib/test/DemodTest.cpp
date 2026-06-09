#include "palindrome/demod.hpp"

#include "palindrome/fir.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <ranges>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace demod = palindrome::demod;
namespace dsp = palindrome::dsp;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;
} // namespace

TEST_CASE("hilbert_kernel is an antisymmetric Type III transformer") {
  CHECK_THROWS_AS(dsp::hilbert_kernel(0), std::invalid_argument);
  constexpr std::size_t taps = 31;
  const auto k = dsp::hilbert_kernel(taps);
  REQUIRE(k.size() == taps);
  const std::size_t centre = (taps - 1) / 2;
  CHECK(k[centre] == 0.0f); // zero at the centre tap (no DC component)
  for (std::size_t i = 0; i < taps; ++i) {
    CHECK_THAT(k[i], WithinAbs(-k[taps - 1 - i], 1e-7)); // antisymmetric about centre
    const std::size_t offset = i > centre ? i - centre : centre - i;
    if (offset % 2 == 0)
      CHECK_THAT(k[i], WithinAbs(0.0f, 1e-9)); // even-offset taps vanish (Type III)
  }
}

TEST_CASE("Hilbert's polyphase quadrature is bit-identical to the full-kernel FIR") {
  // The polyphase split drops only the kernel's exact-zero even-offset taps, so
  // the quadrature plane must match the full Hilbert kernel run through one FIR
  // (the pre-optimisation implementation) to the bit — same fmaf order.
  constexpr std::size_t taps = 127;
  std::vector<float> x(3000);
  for (std::size_t k = 0; k < x.size(); ++k)
    x[k] = static_cast<float>(std::sin(0.31 * static_cast<double>(k)) + 0.4 * std::cos(0.07 * static_cast<double>(k)));

  dsp::Fir full{dsp::hilbert_kernel(taps)};
  full.prepare(x.size());
  const auto qref = full.process(x);
  const std::vector<float> ref{qref.begin(), qref.end()};

  demod::Hilbert dut{taps};
  dut.prepare(x.size());
  const auto out = dut.process(x);

  REQUIRE(out.size() == ref.size());
  for (std::size_t k = 0; k < out.size(); ++k)
    CHECK(out[k].imag() == ref[k]); // bit-exact, not just close
}

TEST_CASE("Hilbert forms a one-sided analytic signal of the right sign") {
  constexpr double fs = 20.0e6;
  constexpr double f = 4.0e6; // mid-band, well inside the Hilbert passband
  constexpr std::size_t n = 8192;
  std::vector<float> x(n);
  for (std::size_t k = 0; k < n; ++k)
    x[k] = static_cast<float>(std::cos(two_pi * f * static_cast<double>(k) / fs));

  demod::Hilbert dut{}; // default taps
  dut.prepare(n);
  const auto out = dut.process(x);
  REQUIRE(out.size() == n);

  // A real cosine's analytic signal is e^{+i*2*pi*f*t}: constant magnitude (no
  // 2f ripple, i.e. the negative image is gone) and a phase that ADVANCES (the
  // sign is +f, the one that pairs with ComplexAmEnvelope's down-mix). Skip the
  // FIR warm-up at both ends.
  const std::size_t lo = 2 * dut.group_delay_samples();
  const std::size_t hi = n - lo;
  double phase_step = 0.0;
  for (std::size_t k = lo; k < hi; ++k) {
    CHECK_THAT(std::abs(out[k]), WithinAbs(1.0, 0.03)); // one-sided => |.| ~ amplitude
    phase_step += std::arg(out[k] * std::conj(out[k - 1]));
  }
  const double mean_step = phase_step / static_cast<double>(hi - lo - 1);
  CHECK_THAT(mean_step, WithinAbs(two_pi * f / fs, 0.02)); // positive, ~ +2*pi*f/fs
}

TEST_CASE("Hilbert's in-phase plane is the input delayed by the group delay (exact)") {
  std::vector<float> x(1000);
  for (std::size_t k = 0; k < x.size(); ++k)
    x[k] = static_cast<float>(std::sin(0.37 * static_cast<double>(k)) + 0.2 * static_cast<double>(k % 7));

  demod::Hilbert dut{};
  dut.prepare(x.size());
  const auto out = dut.process(x);
  const std::size_t d = dut.group_delay_samples();
  // I[k] must equal x[k-d] bit-for-bit (the matched delay line, not a refilter).
  for (std::size_t k = d; k < x.size(); ++k)
    CHECK(out[k].real() == x[k - d]);
}

TEST_CASE("Hilbert is block-invariant (bit-exact across chunkings)") {
  std::vector<float> x(5000);
  for (std::size_t k = 0; k < x.size(); ++k)
    x[k] = static_cast<float>(std::cos(0.21 * static_cast<double>(k)) - 0.5 * std::sin(0.013 * static_cast<double>(k)));

  demod::Hilbert whole_dut{};
  whole_dut.prepare(x.size());
  const auto wspan = whole_dut.process(x);
  const std::vector<std::complex<float>> whole{wspan.begin(), wspan.end()};

  for (const std::size_t chunk: {std::size_t{1}, std::size_t{7}, std::size_t{63}, std::size_t{64}, std::size_t{333}}) {
    demod::Hilbert chunked_dut{};
    chunked_dut.prepare(chunk);
    std::vector<std::complex<float>> chunked;
    for (std::size_t off = 0; off < x.size(); off += chunk) {
      const std::size_t m = std::min(chunk, x.size() - off);
      const auto out = chunked_dut.process(std::span{x}.subspan(off, m));
      chunked.insert(chunked.end(), out.begin(), out.end());
    }
    REQUIRE(chunked.size() == whole.size());
    for (std::size_t k = 0; k < whole.size(); ++k)
      CHECK(chunked[k] == whole[k]); // FIR + delay line: chunking changes nothing
  }
}

TEST_CASE("Hilbert rejects tap counts that aren't 3 (mod 4)") {
  CHECK_THROWS_AS(demod::Hilbert{64}, std::invalid_argument); // even
  CHECK_THROWS_AS(demod::Hilbert{65}, std::invalid_argument); // odd but 1 (mod 4): non-zero taps land on odd indices
  CHECK_NOTHROW(demod::Hilbert{127}); // 3 (mod 4): non-zero taps on even indices
}

namespace {
// An AM-modulated analytic test signal: a[k] * e^{+i*2*pi*fc*k/fs} with envelope
// a[k] = amp * (1 + depth*cos(2*pi*fm*k/fs)). ComplexAmEnvelope recovers 2*a[k]
// (the analytic signal of a real AM wave carries half its amplitude, and the
// envelope output restores the factor).
std::vector<std::complex<float>> am_tone(std::size_t n, double fs, double fc, double fm, double amp, double depth) {
  std::vector<std::complex<float>> x(n);
  for (std::size_t k = 0; k < n; ++k) {
    const auto t = static_cast<double>(k) / fs;
    const double a = amp * (1.0 + depth * std::cos(two_pi * fm * t));
    const double ph = two_pi * fc * t;
    x[k] = {static_cast<float>(a * std::cos(ph)), static_cast<float>(a * std::sin(ph))};
  }
  return x;
}
} // namespace

TEST_CASE("ComplexAmEnvelope recovers a known AM envelope off an offset carrier") {
  constexpr double fs = 1.0e6;
  constexpr double fc = 2.0e5; // carrier offset from DC
  constexpr double fm = 1.0e4; // modulation, well inside the cutoff
  constexpr double amp = 0.5;
  constexpr double depth = 0.5;
  constexpr std::size_t taps = 127;
  constexpr std::size_t n = 8192;
  const auto x = am_tone(n, fs, fc, fm, amp, depth);

  demod::ComplexAmEnvelope dut{fs, fc, 1.0e5, taps};
  dut.prepare(n);
  const auto out = dut.process(x);
  REQUIRE(out.size() == n);

  // The FIR delays the envelope by its (integer) group delay; check against the
  // shifted modulation law, skipping the filter warm-up at both ends. The 2%
  // tolerance absorbs the FIR passband ripple and the DC blocker's slow start-up
  // transient (its pole is ~10k samples).
  const std::size_t gd = (taps - 1) / 2;
  for (std::size_t k = 2 * gd; k + gd < n; ++k) {
    const auto t = static_cast<double>(k - gd) / fs;
    const double expected = 2.0 * amp * (1.0 + depth * std::cos(two_pi * fm * t));
    CHECK_THAT(static_cast<double>(out[k]), WithinAbs(expected, 0.02));
  }
}

TEST_CASE("ComplexAmEnvelope blocks an in-band DC spike") {
  // A constant complex offset (zero-IF LO leakage) mixes to -fc, INSIDE the
  // low-pass here (5e4 < 2e5) — only the input DC blocker removes it. Without
  // the blocker the envelope would settle at 2*|dc| ~ 1.5.
  constexpr double fs = 1.0e6;
  constexpr double fc = 5.0e4;
  constexpr std::size_t chunk = 8192;
  const std::vector<std::complex<float>> x(chunk, std::complex<float>{0.7f, 0.3f});

  demod::ComplexAmEnvelope dut{fs, fc, 2.0e5};
  dut.prepare(chunk);
  // The blocker's pole (0.9999) decays the step over ~10k-sample timescales;
  // stream enough blocks for the transient to die, then check the tail.
  float tail = 1.0f;
  for (int block = 0; block < 16; ++block) {
    const auto out = dut.process(x);
    tail = *std::ranges::max_element(out | std::views::drop(out.size() / 2));
  }
  CHECK_THAT(static_cast<double>(tail), WithinAbs(0.0, 1e-3));
}

TEST_CASE("ComplexAmEnvelope decimation equals filter-then-keep-every-Nth (bit-exact)") {
  constexpr double fs = 1.0e6;
  constexpr double fc = 2.0e5;
  constexpr std::size_t d = 4;
  constexpr std::size_t n = 4096;
  const auto x = am_tone(n, fs, fc, 1.0e4, 0.5, 0.5);

  demod::ComplexAmEnvelope full{fs, fc, 1.0e5};
  full.prepare(n);
  const auto fspan = full.process(x);
  const std::vector<float> whole{fspan.begin(), fspan.end()};

  demod::ComplexAmEnvelope dec{fs, fc, 1.0e5, demod::kDefaultVisionTaps, dsp::Window::Hamming, d};
  dec.prepare(n);
  const auto out = dec.process(x);
  REQUIRE(out.size() == n / d);
  for (std::size_t k = 0; k < out.size(); ++k)
    CHECK(out[k] == whole[k * d]); // same taps, same order; magnitude is elementwise
}

TEST_CASE("ComplexAmEnvelope is block-invariant (bit-exact across chunkings)") {
  // Every piece is a sample-order recurrence (DC blocker, mix phasor — including
  // its every-1024-samples renormalisation, which counts samples, not blocks) or
  // a chunking-invariant FIR, so the envelope must match to the bit. Decimation 2
  // exercises the FIR's output-grid phase carry across chunk boundaries.
  constexpr double fs = 1.0e6;
  constexpr double fc = 2.0e5;
  constexpr std::size_t d = 2;
  constexpr std::size_t n = 5000;
  const auto x = am_tone(n, fs, fc, 1.7e4, 0.4, 0.6);

  demod::ComplexAmEnvelope whole_dut{fs, fc, 1.0e5, demod::kDefaultVisionTaps, dsp::Window::Hamming, d};
  whole_dut.prepare(n);
  const auto wspan = whole_dut.process(x);
  const std::vector<float> whole{wspan.begin(), wspan.end()};

  for (const std::size_t chunk: {std::size_t{1}, std::size_t{7}, std::size_t{63}, std::size_t{64}, std::size_t{333}}) {
    demod::ComplexAmEnvelope chunked_dut{fs, fc, 1.0e5, demod::kDefaultVisionTaps, dsp::Window::Hamming, d};
    chunked_dut.prepare(chunk);
    std::vector<float> chunked;
    for (std::size_t off = 0; off < n; off += chunk) {
      const std::size_t m = std::min(chunk, n - off);
      const auto out = chunked_dut.process(std::span{x}.subspan(off, m));
      chunked.insert(chunked.end(), out.begin(), out.end());
    }
    REQUIRE(chunked.size() == whole.size());
    for (std::size_t k = 0; k < whole.size(); ++k)
      CHECK(chunked[k] == whole[k]);
  }
}

TEST_CASE("ComplexAmEnvelope rejects bad parameters") {
  CHECK_THROWS_AS((demod::ComplexAmEnvelope{1.0e6, 2.0e5, 0.0}), std::invalid_argument); // cutoff <= 0
  CHECK_THROWS_AS((demod::ComplexAmEnvelope{1.0e6, 2.0e5, 5.0e5}), std::invalid_argument); // cutoff >= fs/2
  CHECK_THROWS_AS((demod::ComplexAmEnvelope{1.0e6, 2.0e5, 1.0e5, 0}), std::invalid_argument); // no taps
  CHECK_THROWS_AS((demod::ComplexAmEnvelope{1.0e6, 2.0e5, 1.0e5, 127, dsp::Window::Hamming, 0}), std::invalid_argument);
}
