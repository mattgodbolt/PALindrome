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
// indifferent, so the measurement reads the FILTER, not the reference path.
// The quasi-sync tests skip further in: the FIR settles and the tank rings up
// from empty over the first few thousand samples.
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

TEST_CASE("find_vision_carrier recovers the carrier of an AM signal") {
  // A negative-modulated vision signal: the carrier dominates, with a video
  // pedestal of modulation, a weaker chroma line at +4.43, and a weaker sound
  // carrier at +6. The finder must pick the vision carrier, not those - and to
  // a small fraction of the ~76 Hz bin (262144-point FFT at 20 MS/s).
  constexpr double kFc = 3.0517e6; // deliberately off a bin centre
  std::vector<float> x(1u << 18);
  for (std::size_t k = 0; k < x.size(); ++k) {
    const auto t = static_cast<double>(k) / kRate;
    const double video = 0.6 + 0.4 * std::cos(two_pi * 70.0e3 * t); // modulation pedestal
    x[k] = static_cast<float>(video * std::cos(two_pi * kFc * t) //
                              + 0.15 * std::cos(two_pi * (kFc + 4.43e6) * t) // chroma
                              + 0.2 * std::cos(two_pi * (kFc + 6.0e6) * t)); // sound
  }
  const auto found = demod::find_vision_carrier(x, kRate, 1.0e6, 9.5e6);
  REQUIRE(found.has_value());
  CHECK_THAT(*found, WithinAbs(kFc, 20.0)); // sub-bin: within 20 Hz of 3.0517 MHz
}

TEST_CASE("find_vision_carrier rejects bad parameters") {
  std::vector<float> x(4096, 0.0f);
  CHECK_THROWS_AS(demod::find_vision_carrier(x, 0.0, 1.0e6, 5.0e6), std::invalid_argument); // rate
  CHECK_THROWS_AS(demod::find_vision_carrier(x, kRate, 5.0e6, 1.0e6), std::invalid_argument); // lo >= hi
  CHECK_THROWS_AS(demod::find_vision_carrier(x, kRate, 1.0e6, kRate), std::invalid_argument); // hi >= Nyquist
}

TEST_CASE("find_vision_carrier reports data it can't scan as errors, not throws") {
  // Data outcomes come back as CarrierScanError so the live path can read more
  // signal and retry; only caller errors (above) throw.
  const std::vector<float> one(1, 0.0f);
  const auto too_short = demod::find_vision_carrier(one, kRate, 1.0e6, 5.0e6);
  REQUIRE_FALSE(too_short.has_value());
  CHECK(too_short.error() == demod::CarrierScanError::too_few_samples);
  const std::vector<float> silent(4096, 0.0f);
  const auto no_signal = demod::find_vision_carrier(silent, kRate, 1.0e6, 5.0e6);
  REQUIRE_FALSE(no_signal.has_value());
  CHECK(no_signal.error() == demod::CarrierScanError::no_signal);
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

TEST_CASE("VisionIf stays bit-exact block-invariant at a standing AFC offset") {
  // A 5 kHz mistune exercises the loop with the AFC integrator moving the
  // whole run (its time constant is ~1.9M samples, far beyond this clip): the
  // tank state, the integrator and the trim all evolve per sample from
  // carried state, so chunking must not move any of it.
  std::vector<float> x(400000);
  for (std::size_t k = 0; k < x.size(); ++k) {
    const auto t = static_cast<double>(k) / kRate;
    x[k] = static_cast<float>((1.0 + 0.5 * std::cos(two_pi * 312.5e3 * t)) * std::cos(two_pi * (kCarrier + 5.0e3) * t));
  }
  const auto shape = demod::saw80_template();
  demod::VisionIf one{kRate, kCarrier, shape, demod::Detector::quasi_sync};
  one.prepare(x.size());
  const auto all = one.process(x);
  const std::vector<float> whole{all.begin(), all.end()};

  for (const std::size_t chunk: {std::size_t{1}, std::size_t{337}, std::size_t{4096}}) {
    demod::VisionIf chunked{kRate, kCarrier, shape, demod::Detector::quasi_sync};
    chunked.prepare(x.size());
    std::vector<float> pieces;
    for (std::size_t at = 0; at < x.size(); at += chunk) {
      const auto n = std::min(chunk, x.size() - at);
      const auto out = chunked.process(std::span{x}.subspan(at, n));
      pieces.insert(pieces.end(), out.begin(), out.end());
    }
    CHECK(pieces == whole);
  }
}

TEST_CASE("quasi-sync suppresses the envelope detector's VSB quadrature distortion") {
  // The same flank-region AM tones whose asymmetric sidebands lift an envelope
  // detector's mean by a few % (see the flank test): the quasi-sync detector
  // takes the in-phase product against the tank reference, so the mean sits at
  // the carrier level up to the tank's own sideband leakage (the reference LC
  // passes a sliver of the main sideband - a real set's residual, ~1-2% at the
  // flank edge, and measurably below the envelope's lift at every tone).
  for (const double mod_hz: {200.0e3, 312.5e3, 600.0e3}) {
    const auto x = am_tone(60000, mod_hz, 0.5);
    const auto r = run(demod::saw80_template(), x, kRate, kCarrier, demod::Detector::quasi_sync, kLockSkip);
    const auto env = run(demod::saw80_template(), x, kRate, kCarrier, demod::Detector::envelope, kLockSkip);
    CHECK_THAT(r.mean, WithinAbs(0.5, 0.015));
    CHECK_THAT(r.amplitude, WithinRel(0.5 * 0.5, 0.02));
    CHECK(std::abs(r.mean - 0.5) < std::abs(env.mean - 0.5)); // the residual stays below the envelope's lift
  }
}

TEST_CASE("quasi-sync rides through brief overmodulation; the envelope folds") {
  // Depth 1.3: the modulated carrier passes through zero. The tank reference's
  // flywheel (memory ~1/(2*pi*half-BW) ~ 0.3 us - the real Q~40 LC) coasts
  // through an inversion comparable to its memory - the coast is graded, not
  // thresholded - so the video follows the signal negative; a magnitude
  // detector rectifies the overshoot at zero. A SUSTAINED inversion re-rings
  // the tank on the flipped carrier and rectifies too (deep slow
  // overmodulation buzzed on real sets); probe with a fast tone, 600 kHz,
  // whose underwater stretch is 22% of the period ~ 0.37 us.
  const auto x = am_tone(60000, 600.0e3, 1.3);
  const auto min_of = [&](demod::Detector det) {
    demod::VisionIf dut{kRate, kCarrier, demod::saw80_template(), det};
    dut.prepare(x.size());
    const auto out = dut.process(x);
    return *std::ranges::min_element(out.subspan(kLockSkip));
  };
  CHECK(min_of(demod::Detector::quasi_sync) < 0.0f); // follows the signal through the fold
  CHECK(min_of(demod::Detector::envelope) > 0.1f); // rectified: never reaches zero
}

TEST_CASE("quasi-sync acquires upright from any cold-start phase") {
  // The tank reference is signal-driven, so it is structurally incapable of
  // locking inverted - it rings up on the carrier's own phase from any start.
  // Keep the sweep as the regression fence: a phase-locked reference with a
  // sign-corrected (Costas-style) error once stabilised 180 degrees too and
  // locked the RX888 corpus inverted, starving the sync slicer of every edge.
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
  // corpus), so the decimation-folding of the tank's centre step (omega * d
  // per output sample) is load-bearing: drop that factor and the tank rings
  // megahertz off the carrier, while every d=1 test stays green.
  for (const std::size_t d: {std::size_t{2}, std::size_t{4}}) {
    const auto x = am_tone(120000, 312.5e3, 0.5);
    demod::VisionIf dut{kRate, kCarrier, demod::saw80_template(), demod::Detector::quasi_sync, demod::kDefaultIfTaps,
        palindrome::dsp::Window::Hamming, d};
    dut.prepare(x.size());
    const auto r = measure(dut.process(x), kLockSkip / d * 2); // fewer corrections/s: allow more settle
    // The tank's Hz bandwidth is fixed, so decimation widens it per output
    // sample and the sideband leakage's mean shift grows a little with d.
    CHECK_THAT(r.mean, WithinAbs(0.5, 0.012));
    CHECK_THAT(r.amplitude, WithinRel(0.5 * 0.5, 0.02));
  }
}

TEST_CASE("quasi-sync locks through a carrier-frequency error") {
  // The signal arrives 300 Hz off the constructed nominal (a believable
  // metadata/scan error). The tank rides it with a sub-milliradian static
  // phase error while the AFC integrator absorbs it: after settling, the
  // video is as clean as on-frequency.
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

TEST_CASE("quasi-sync AFC pulls in a kHz-scale mistune and reports the signed offset") {
  // The nominal carrier is 5 kHz off the signal - modulator-drift scale, far
  // beyond metadata error. The AFC integrator must pull the tank onto it and
  // afc_offset_hz must report the error with the documented sign: positive =
  // the real carrier sits above the constructed nominal. The loop's time
  // constant is tank_k / kAfcKi ~ 1.9M samples, so the tone streams through
  // twelve times (~5 tau plus slack for the slower-converging sign); every
  // component is an integer number of cycles per buffer, so the tiling is
  // seamless.
  constexpr double kOffset = 5.0e3;
  std::vector<float> x(1000000);
  for (std::size_t k = 0; k < x.size(); ++k) {
    const auto t = static_cast<double>(k) / kRate;
    x[k] = static_cast<float>((1.0 + 0.5 * std::cos(two_pi * 312.5e3 * t)) * std::cos(two_pi * kCarrier * t));
  }
  for (const double expected: {kOffset, -kOffset}) {
    demod::VisionIf dut{kRate, kCarrier - expected, demod::saw80_template(), demod::Detector::quasi_sync};
    dut.prepare(x.size());
    Ripple r{};
    for (int pass = 0; pass < 12; ++pass)
      r = measure(dut.process(x), x.size() / 2);
    CHECK_THAT(r.mean, WithinAbs(0.5, 0.015)); // locked and clean after pull-in
    CHECK_THAT(dut.afc_offset_hz(), WithinAbs(expected, 300.0));
  }
  // The envelope detector has no loop; the diagnostic reads 0.
  demod::VisionIf env{kRate, kCarrier - kOffset, demod::saw80_template(), demod::Detector::envelope};
  env.prepare(x.size());
  [[maybe_unused]] const auto out = env.process(x);
  CHECK(env.afc_offset_hz() == 0.0);
}

TEST_CASE("quasi-sync AFC pulls in from the far edge of the catch range") {
  // 280 kHz stale - near the 300 kHz clamp, the regime the kHz-scale test
  // above never reaches. The tank discriminator is monotone across the whole
  // range (no cycle slips), so the pull is slew-limited and completes within
  // ~t99 0.42 s at this rate; twelve tiled passes cover it with margin. The
  // tolerance allows the fixed FIR flank's small standing bias (the taps stay
  // centred on the stale nominal - the honest mistuned-set residual).
  constexpr double kOffset = 280.0e3;
  std::vector<float> x(1000000);
  for (std::size_t k = 0; k < x.size(); ++k) {
    const auto t = static_cast<double>(k) / kRate;
    x[k] = static_cast<float>((1.0 + 0.5 * std::cos(two_pi * 312.5e3 * t)) * std::cos(two_pi * kCarrier * t));
  }
  demod::VisionIf dut{kRate, kCarrier - kOffset, demod::saw80_template(), demod::Detector::quasi_sync};
  dut.prepare(x.size());
  for (int pass = 0; pass < 12; ++pass) [[maybe_unused]]
    const auto out = dut.process(x);
  CHECK_THAT(dut.afc_offset_hz(), WithinAbs(kOffset, 5000.0));
}
