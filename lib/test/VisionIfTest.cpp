#include "palindrome/demod.hpp"

#include "palindrome/fir.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace demod = palindrome::demod;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr double kRate = 20.0e6;
constexpr double kCarrier = 3.0e6;

// An AM signal: carrier of unit amplitude with one cosine modulation tone.
std::vector<float> am_tone(std::size_t n, double mod_hz, double depth) {
  std::vector<float> x(n);
  for (std::size_t k = 0; k < n; ++k) {
    const auto t = static_cast<double>(k) / kRate;
    x[k] = static_cast<float>((1.0 + depth * std::cos(two_pi * mod_hz * t)) * std::cos(two_pi * kCarrier * t));
  }
  return x;
}

// A carrier plus a single upper sideband `offset_hz` above it: the probe for
// the template's response at one point (the envelope ripples at offset_hz with
// amplitude = sideband * |H(carrier + offset)| / ... carrier terms, measured
// relative to the carrier response below).
std::vector<float> carrier_plus_sideband(
    std::size_t n, double offset_hz, double amp, double rate = kRate, double carrier = kCarrier) {
  std::vector<float> x(n);
  for (std::size_t k = 0; k < n; ++k) {
    const auto t = static_cast<double>(k) / rate;
    x[k] = static_cast<float>(std::cos(two_pi * carrier * t) + amp * std::cos(two_pi * (carrier + offset_hz) * t));
  }
  return x;
}

struct Ripple {
  double mean;
  double amplitude; // (max - min) / 2 over the analysed window
};

// Envelope statistics after the FIR has settled (skip the first num_taps).
Ripple measure(std::span<const float> env, std::size_t skip) {
  REQUIRE(env.size() > skip + 1000);
  const auto body = env.subspan(skip);
  double sum = 0.0;
  auto lo = body[0];
  auto hi = body[0];
  for (const auto v: body) {
    sum += static_cast<double>(v);
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  return {sum / static_cast<double>(body.size()), static_cast<double>(hi - lo) / 2.0};
}

// Response probes default to the envelope detector: it is carrier-phase
// indifferent, so the measurement reads the FILTER, not the phase loop. The
// quasi-sync tests skip further in: the phase lock spends its first few
// thousand samples acquiring from a cold phasor.
constexpr std::size_t kLockSkip = 20000;

Ripple run(const demod::IfTemplate &shape, const std::vector<float> &x, double rate = kRate, double carrier = kCarrier,
    demod::Detector det = demod::Detector::envelope, std::size_t skip = demod::kDefaultIfTaps) {
  demod::VisionIf dut{rate, carrier, shape, det};
  dut.prepare(x.size());
  return measure(dut.process(x), skip);
}
} // namespace

TEST_CASE("VisionIf rejects bad parameters") {
  const auto t = demod::saw80_template();
  CHECK_THROWS_AS(demod::VisionIf(kRate, kCarrier, t, demod::Detector::envelope, 0), std::invalid_argument);
  CHECK_THROWS_AS(demod::VisionIf(kRate, kCarrier, t, demod::Detector::envelope, 256), std::invalid_argument); // even
  CHECK_THROWS_AS(demod::VisionIf(kRate, 0.0, t), std::invalid_argument);
  CHECK_THROWS_AS(demod::VisionIf(kRate, kRate / 2.0, t), std::invalid_argument);
  auto bad = t;
  bad.flank_half_width_hz = 0.0;
  CHECK_THROWS_AS(demod::VisionIf(kRate, kCarrier, bad), std::invalid_argument);
  bad = t;
  bad.shape = {{1.0e6, 0.0}, {1.0e6, -3.0}}; // not strictly increasing
  CHECK_THROWS_AS(demod::VisionIf(kRate, kCarrier, bad), std::invalid_argument);
  bad = t;
  bad.shape = {{2.0e6, 0.0}, {2.5e6, 0.0}}; // table excludes the carrier: nothing to level against
  CHECK_THROWS_AS(demod::VisionIf(kRate, kCarrier, bad), std::invalid_argument);
}

TEST_CASE("VisionIf is one-sided: a bare carrier gives a flat envelope at the flank level") {
  // A real cosine has images at +/- the carrier. If the kernel passed any of the
  // negative-frequency image the envelope would beat at 2*carrier; one-sided, it
  // is constant - this is the property that subsumes the Hilbert stage. Level:
  // unit carrier, halved into the analytic domain, times the flank's 0.5, times
  // the detector's 2x convention = 0.5.
  std::vector<float> x(20000);
  for (std::size_t k = 0; k < x.size(); ++k)
    x[k] = static_cast<float>(std::cos(two_pi * kCarrier * static_cast<double>(k) / kRate));
  const auto r = run(demod::saw80_template(), x);
  CHECK_THAT(r.mean, WithinAbs(0.5, 0.005));
  CHECK(r.amplitude < 0.005 * r.mean);
}

TEST_CASE("Nyquist flank sums a DSB sideband pair back to flat video") {
  // Both AM sidebands of a low modulating frequency land on the flank, where
  // V(+f) + V(-f) = 1: the detected ripple must be depth * the carrier's 0.5
  // flank level even though each sideband alone is far from flat - the flank's
  // entire job. The *mean* rides a little above 0.5: the asymmetric sidebands
  // leave a quadrature term the envelope detector folds in (the genuine VSB
  // quadrature distortion a quasi-sync detector removes), so it is checked
  // loosely, high side only.
  for (const double mod_hz: {200.0e3, 312.5e3, 600.0e3}) {
    const auto r = run(demod::saw80_template(), am_tone(40000, mod_hz, 0.5));
    CHECK_THAT(r.amplitude, WithinRel(0.5 * 0.5, 0.02));
    CHECK(r.mean >= 0.5);
    CHECK(r.mean < 0.54);
  }
}

TEST_CASE("VisionIf chroma-region response follows the template's shoulder") {
  // A single sideband at the subcarrier offset: envelope ripple relative to the
  // carrier term reads off |H| there. saw80 interpolates -12 dB * (4.43-3.5)/2.4
  // = -4.66 dB on the shoulder; the flank is 1 by then.
  const auto r = run(demod::saw80_template(), carrier_plus_sideband(40000, 4.43e6, 0.2));
  const double h = (r.amplitude / r.mean) / (0.2 / 0.5); // / (sideband / carrier voltage)
  CHECK_THAT(h, WithinAbs(std::pow(10.0, -4.66 / 20.0), 0.03));

  // saw90 is flat through the chroma (a few % of realisation smear: the 4.8 MHz
  // corner is only ~370 kHz away, within the window's main lobe).
  const auto r90 = run(demod::saw90_template(), carrier_plus_sideband(40000, 4.43e6, 0.2));
  CHECK_THAT((r90.amplitude / r90.mean) / (0.2 / 0.5), WithinAbs(1.0, 0.05));
}

TEST_CASE("sound notch is deep but deliberately finite") {
  // A sound-carrier stand-in at +6 MHz. saw80's notch is -26 dB *below the
  // shoulder it sits on* (about -12.5 dB by the table there): enough to tame the
  // beat, finite enough that an intercarrier residue survives - so the ripple
  // must be well down, but measurably non-zero.
  const auto r = run(demod::saw80_template(), carrier_plus_sideband(40000, 6.0e6, 0.2));
  const double h = (r.amplitude / r.mean) / (0.2 / 0.5);
  CHECK(h < std::pow(10.0, -30.0 / 20.0));
  CHECK(h > std::pow(10.0, -50.0 / 20.0));

  // Deepening the notch via the template knob buries it further.
  auto deep = demod::saw80_template();
  deep.sound_notch_db = -50.0;
  const auto rd = run(deep, carrier_plus_sideband(40000, 6.0e6, 0.2));
  CHECK(rd.amplitude / rd.mean < 0.3 * (r.amplitude / r.mean));
}

TEST_CASE("VisionIf realises the template at the RX888 geometry too") {
  // 32 MS/s widens the realisation's smear (the window mainlobe scales with
  // fs/num_taps, ~502 kHz against the 400 kHz notch half-width), so the notch
  // lands a few dB shallower than at 20 MS/s. It must still be well down, the
  // image still gone, and the template must fit (Nyquist 16 MHz holds the
  // curve's end at carrier + 6.8 = 10.4 MHz easily).
  constexpr double kRx888Rate = 32.0e6;
  constexpr double kRx888Carrier = 3.564e6;
  const auto t = demod::saw80_template();

  std::vector<float> bare(40000);
  for (std::size_t k = 0; k < bare.size(); ++k)
    bare[k] = static_cast<float>(std::cos(two_pi * kRx888Carrier * static_cast<double>(k) / kRx888Rate));
  const auto rc = run(t, bare, kRx888Rate, kRx888Carrier);
  CHECK_THAT(rc.mean, WithinAbs(0.5, 0.005)); // one-sided: no image beat
  CHECK(rc.amplitude < 0.005 * rc.mean);

  const auto rn =
      run(t, carrier_plus_sideband(40000, 6.0e6, 0.2, kRx888Rate, kRx888Carrier), kRx888Rate, kRx888Carrier);
  const double h = (rn.amplitude / rn.mean) / (0.2 / 0.5);
  CHECK(h < std::pow(10.0, -28.0 / 20.0)); // realised shallower than 20 MS/s, still deep
  CHECK(h > std::pow(10.0, -50.0 / 20.0)); // and still finite

  // A rate that can't hold the template must refuse, not silently fold the
  // carrier image onto the picture.
  CHECK_THROWS_AS(demod::VisionIf(16.0e6, kRx888Carrier, t), std::invalid_argument);
}

TEST_CASE("VisionIf decimation keeps exactly every Nth full-rate output") {
  // Envelope detector: it is pointwise on the FIR outputs, so decimation is
  // exactly stride-sampling. (Quasi-sync is not - its loop corrects once per
  // OUTPUT sample, so the decimated lock takes a slightly different path.)
  const auto x = am_tone(40000, 312.5e3, 0.4);
  const auto t = demod::saw80_template();
  demod::VisionIf full{kRate, kCarrier, t, demod::Detector::envelope};
  demod::VisionIf dec{
      kRate, kCarrier, t, demod::Detector::envelope, demod::kDefaultIfTaps, palindrome::dsp::Window::Hamming, 4};
  full.prepare(x.size());
  dec.prepare(x.size());
  const auto whole = full.process(x);
  const auto fourth = dec.process(x);
  REQUIRE(fourth.size() == (x.size() + 3) / 4);
  for (std::size_t k = 0; k < fourth.size(); ++k)
    CHECK(fourth[k] == whole[4 * k]); // the decimating FIR evaluates the same dot products
}

TEST_CASE("VisionIf is bit-exact block-invariant") {
  // Both detectors: the FIRs are feed-forward and the quasi-sync loop is a
  // sample-order recurrence carried across calls, so == costs nothing.
  const auto x = am_tone(30000, 600.0e3, 0.5);
  const auto t = demod::saw80_template();
  for (const auto det: {demod::Detector::envelope, demod::Detector::quasi_sync}) {
    demod::VisionIf one{kRate, kCarrier, t, det};
    one.prepare(x.size());
    const auto all = one.process(x);
    const std::vector<float> whole{all.begin(), all.end()};

    demod::VisionIf chunked{kRate, kCarrier, t, det};
    chunked.prepare(x.size());
    std::vector<float> pieces;
    constexpr std::size_t kChunk = 337;
    for (std::size_t at = 0; at < x.size(); at += kChunk) {
      const auto n = std::min(kChunk, x.size() - at);
      const auto out = chunked.process(std::span{x}.subspan(at, n));
      pieces.insert(pieces.end(), out.begin(), out.end());
    }
    CHECK(pieces == whole);
  }
}

TEST_CASE("quasi-sync removes the envelope detector's VSB quadrature distortion") {
  // The same flank-region AM tones whose asymmetric sidebands lift an envelope
  // detector's mean by a few % (see the flank test): the quasi-sync detector
  // takes only the in-phase product, so the mean must sit AT the carrier level
  // and the ripple must still be depth * the 0.5 flank level.
  for (const double mod_hz: {200.0e3, 312.5e3, 600.0e3}) {
    const auto r = run(
        demod::saw80_template(), am_tone(60000, mod_hz, 0.5), kRate, kCarrier, demod::Detector::quasi_sync, kLockSkip);
    CHECK_THAT(r.mean, WithinAbs(0.5, 0.005));
    CHECK_THAT(r.amplitude, WithinRel(0.5 * 0.5, 0.02));
  }
}

TEST_CASE("quasi-sync is linear through overmodulation; the envelope folds") {
  // Depth 1.3: the modulated carrier passes through zero. A product detector
  // follows it negative; a magnitude can't, and rectifies the overshoot.
  const auto x = am_tone(60000, 312.5e3, 1.3);
  const auto qs = run(demod::saw80_template(), x, kRate, kCarrier, demod::Detector::quasi_sync, kLockSkip);
  const auto env = run(demod::saw80_template(), x, kRate, kCarrier, demod::Detector::envelope, kLockSkip);
  // Quasi-sync swings to ~0.5*(1 - 1.3) = -0.15; envelope folds at zero. The
  // ripple metric ((max-min)/2) reads the difference directly.
  CHECK_THAT(qs.amplitude, WithinRel(0.5 * 1.3, 0.03));
  CHECK(env.amplitude < 0.55); // folded: (max - 0) / 2 at most ~0.5 + slop
}

TEST_CASE("quasi-sync acquires upright from any cold-start phase") {
  // The worst case: the signal arrives exactly 180 degrees from the cold NCO.
  // That point must be a repeller, not a second lock (a Costas-style
  // sign-corrected error stabilises both - this is the regression that locked
  // the RX888 corpus inverted and starved the sync slicer of every edge).
  for (const double phase0: {0.0, 1.5, std::numbers::pi, 4.5}) {
    std::vector<float> x(60000);
    for (std::size_t k = 0; k < x.size(); ++k) {
      const auto t = static_cast<double>(k) / kRate;
      x[k] =
          static_cast<float>((1.0 + 0.5 * std::cos(two_pi * 312.5e3 * t)) * std::cos(two_pi * kCarrier * t + phase0));
    }
    const auto r = run(demod::saw80_template(), x, kRate, kCarrier, demod::Detector::quasi_sync, kLockSkip);
    CHECK_THAT(r.mean, WithinAbs(0.5, 0.005)); // +0.5, never -0.5: upright lock only
    CHECK_THAT(r.amplitude, WithinRel(0.5 * 0.5, 0.02));
  }
}

TEST_CASE("quasi-sync locks under decimation - the shipped default geometry") {
  // The render default is quasi-sync AND auto-decimation (/2 on the RX888
  // corpus), so the decimation-folding of the NCO step (omega * d per output
  // sample) is load-bearing: drop that factor and the NCO free-runs megahertz
  // off, far beyond any pull-in, while every d=1 test stays green.
  for (const std::size_t d: {std::size_t{2}, std::size_t{4}}) {
    const auto x = am_tone(120000, 312.5e3, 0.5);
    demod::VisionIf dut{kRate, kCarrier, demod::saw80_template(), demod::Detector::quasi_sync, demod::kDefaultIfTaps,
        palindrome::dsp::Window::Hamming, d};
    dut.prepare(x.size());
    const auto r = measure(dut.process(x), kLockSkip / d * 2); // fewer corrections/s: allow more settle
    CHECK_THAT(r.mean, WithinAbs(0.5, 0.005));
    CHECK_THAT(r.amplitude, WithinRel(0.5 * 0.5, 0.02));
  }
}

TEST_CASE("quasi-sync locks through a carrier-frequency error") {
  // The signal arrives 300 Hz off the nominal carrier the NCO was built for
  // (a believable metadata/scan error). The PI integrator must wind up and
  // track it: after settling, the video is as clean as on-frequency.
  constexpr double kOffset = 300.0;
  std::vector<float> x(400000);
  for (std::size_t k = 0; k < x.size(); ++k) {
    const auto t = static_cast<double>(k) / kRate;
    x[k] =
        static_cast<float>((1.0 + 0.5 * std::cos(two_pi * 312.5e3 * t)) * std::cos(two_pi * (kCarrier + kOffset) * t));
  }
  demod::VisionIf dut{kRate, kCarrier, demod::saw80_template(), demod::Detector::quasi_sync};
  dut.prepare(x.size());
  // Skip half the clip: enough for the integrator to absorb the offset.
  const auto r = measure(dut.process(x), x.size() / 2);
  CHECK_THAT(r.mean, WithinAbs(0.5, 0.01));
  CHECK_THAT(r.amplitude, WithinRel(0.5 * 0.5, 0.03));
}
