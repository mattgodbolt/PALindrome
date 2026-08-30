#include "palindrome/chroma_decoder.hpp"
#include "palindrome/horizontal_sweep.hpp"
#include "palindrome/sync_separator.hpp"
#include "palindrome/video_types.hpp"

#include "VideoSynth.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace video = palindrome::video;
using videotest::kRate;
using videotest::synth_colour_composite;
using videotest::synth_composite;

TEST_CASE("ChromaDecoder is block-invariant (the streaming guarantee)") {
  const auto env = synth_colour_composite(40, 1028);

  // Pre-compute the horizontal rail once; the chroma decoder joins it by index.
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  sep.prepare(env.size());
  const auto sync = sep.process(env);
  std::vector<video::SyncSample> sync_copy{sync.begin(), sync.end()};
  video::HorizontalSweep hsweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  hsweep.prepare(sync_copy.size());
  std::vector<video::BeamSample> hbeam;
  {
    const auto b = hsweep.process(sync_copy);
    hbeam.assign(b.begin(), b.end());
  }

  // Every comb mode must be block-invariant — the NCO phasor, FIR state, burst
  // gate, comb ring and parity all carry across calls in each.
  const auto mode =
      GENERATE(video::CombMode::post, video::CombMode::delay_line, video::CombMode::off, video::CombMode::glass);
  const video::ChromaDecoderConfig cfg{.sample_rate_hz = kRate, .comb_mode = mode};

  video::ChromaDecoder whole{cfg};
  whole.prepare(env.size());
  std::vector<video::ChromaSample> ref;
  {
    const auto out = whole.process(env, hbeam);
    ref.assign(out.begin(), out.end());
  }

  // The same signal in awkward block sizes must reproduce it sample-for-sample.
  const auto chunk = GENERATE(std::size_t{1}, std::size_t{7}, std::size_t{333}, std::size_t{4096});
  CAPTURE(mode, chunk);
  video::ChromaDecoder chunked{cfg};
  chunked.prepare(chunk);
  std::vector<video::ChromaSample> got;
  for (std::size_t off = 0; off < env.size(); off += chunk) {
    const std::size_t n = std::min(chunk, env.size() - off);
    const auto out = chunked.process(std::span{env}.subspan(off, n), std::span{hbeam}.subspan(off, n));
    got.insert(got.end(), out.begin(), out.end());
  }
  REQUIRE(got.size() == ref.size());
  for (std::size_t i = 0; i < ref.size(); ++i) {
    CHECK(got[i].luma == ref[i].luma);
    CHECK(got[i].u == ref[i].u);
    CHECK(got[i].v == ref[i].v);
  }
}

namespace {
// Run separator -> sweep -> chroma over `env`, returning the chroma stream and
// the decoder's killer gate after the run.
struct ChromaRun {
  std::vector<video::ChromaSample> out;
  double killer_gain;
  double subcarrier_hz;
};
ChromaRun run_chroma(std::span<const float> env, const video::ChromaDecoderConfig &cfg) {
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  sep.prepare(env.size());
  const auto sync = sep.process(env);
  video::HorizontalSweep hsweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  hsweep.prepare(sync.size());
  const auto hbeam = hsweep.process(sync);
  video::ChromaDecoder chroma{cfg};
  chroma.prepare(env.size());
  const auto out = chroma.process(env, hbeam);
  return ChromaRun{
      .out = {out.begin(), out.end()}, .killer_gain = chroma.killer_gain(), .subcarrier_hz = chroma.subcarrier_hz()};
}

float peak_chroma(std::span<const video::ChromaSample> s) {
  float peak = 0.0f;
  for (const auto &c: s)
    peak = std::max({peak, std::abs(c.u), std::abs(c.v)});
  return peak;
}

// The killer tests build their timing rail straight off the separator (no
// sync-branch low-pass), so unlike the full decoder the rail carries no group
// delay — the demodulated burst lands ~60 samples (the chroma path's delay)
// after the synth's back-porch position. Place the gate there.
video::ChromaDecoderConfig killer_test_config() {
  return video::ChromaDecoderConfig{.sample_rate_hz = kRate, .burst_gate_lo = 0.145, .burst_gate_hi = 0.175};
}
} // namespace

TEST_CASE("colour killer mutes a burst-free (mono) signal") {
  // synth_composite carries no subcarrier at all — a mono transmission (or the
  // CRTC-abuse trick that suppresses the burst). The ident never finds a PAL
  // swing, so the gate must stay shut and no spurious colour painted.
  const auto env = synth_composite(60, 1028);
  const auto run = run_chroma(env, killer_test_config());
  CHECK(run.killer_gain < 0.05);
  CHECK(peak_chroma(run.out) < 1e-3f);
}

TEST_CASE("colour killer mutes white noise") {
  // Deterministic LCG noise across the whole envelope: sync barely locks and
  // any burst-gate measurement is random. Noise can pass an amplitude test but
  // can't fake a bistable-consistent ±45° swing, so the killer holds the
  // chroma off — the picture decodes as (noisy) monochrome.
  std::vector<float> env(600 * 1028);
  std::uint32_t lcg = 1;
  for (auto &e: env) {
    lcg = lcg * 1664525u + 1013904223u;
    e = static_cast<float>(lcg >> 8) / static_cast<float>(1u << 24);
  }
  const auto run = run_chroma(env, killer_test_config());
  CHECK(run.killer_gain < 0.1);
  CHECK(peak_chroma(run.out) < 0.05f);
}

TEST_CASE("colour killer fades a valid burst in slowly, and can be disabled") {
  const auto env = synth_colour_composite(300, 1028);

  // A real burst identifies within a few lines, but the gate opens on the slow
  // saturation-control ramp: well off zero after 300 lines, nowhere near full.
  const auto period = run_chroma(env, killer_test_config());
  CHECK(period.killer_gain > 0.05);
  CHECK(period.killer_gain < 0.6);
  // The switch-on is visible in the output: the early lines are still fully
  // muted (the gate hasn't reached the switch point), the late ones carry
  // chroma — colour pops on and fades up, as a set at switch-on.
  const std::size_t fifth = period.out.size() / 5;
  CHECK(peak_chroma(std::span{period.out}.first(fifth)) < 1e-3f);
  CHECK(peak_chroma(std::span{period.out}.last(fifth)) > 0.05f);

  // killer_threshold <= 0 disables the gate entirely (the pre-killer decode).
  auto open_cfg = killer_test_config();
  open_cfg.killer_threshold = 0.0;
  const auto open = run_chroma(env, open_cfg);
  CHECK(open.killer_gain == 1.0);
  CHECK(peak_chroma(open.out) > peak_chroma(period.out));
}

TEST_CASE("Firetrack's per-frame parity flip: a long-Tc ident kills colour, a fast one shrugs it off") {
  // One stretched line per 313-line frame inverts the burst-swing parity at
  // 25 Hz (docs/Firetrack_BW_Trick.md, measured on a real Master).
  // Which way a set goes is decided by its ident time constant alone.
  constexpr std::size_t kFrameLines = 313;
  const auto env = synth_colour_composite(kFrameLines * 6, 1028, 0.0, kFrameLines);

  // A fast ident re-phases the bistable a dozen lines into each frame: the
  // killer sees agreement for the rest of the frame, so the gate chatters
  // (its landing point is set by the killer ramps, ~0.6 at the defaults) but
  // stays well clear of the hard-mute switch: colour survives.
  const auto fast = run_chroma(env, killer_test_config());
  CHECK(fast.killer_gain > video::ChromaDecoder::kKillerSwitch);

  // A long-Tc ident integrates across frames of alternating sense: it never
  // reaches the killer threshold (nor the re-phase one), and the gate stays
  // shut - the picture goes black and white.
  auto slow_cfg = killer_test_config();
  slow_cfg.ident_tc_lines = 1000.0;
  const auto slow = run_chroma(env, slow_cfg);
  CHECK(slow.killer_gain < 0.05);

  // The same long-Tc set still colours up on an honest PAL signal - just
  // slowly: the bistable starts mis-phased, so the ident first integrates its
  // way down to the re-phase point, then back up past the killer threshold,
  // and the gate only starts opening some three frames in.
  const auto honest = synth_colour_composite(kFrameLines * 12, 1028);
  const auto locked = run_chroma(honest, slow_cfg);
  CHECK(locked.killer_gain > 0.5);
}

TEST_CASE("APC pulls the crystal onto an in-range subcarrier offset") {
  // A source 200 Hz off the crystal — inside the catching range. The frequency
  // pull should walk the NCO onto the source: the reported crystal frequency
  // converges to the source's, and colour survives (killer gate opening).
  constexpr double offset = 200.0;
  const auto env = synth_colour_composite(300, 1028, offset);
  const auto run = run_chroma(env, killer_test_config());
  CHECK(std::abs(run.subcarrier_hz - (4.43361875e6 + offset)) < 20.0);
  CHECK(run.killer_gain > 0.05);
}

TEST_CASE("a source beyond the APC catching range fails to lock colour") {
  // 2 kHz off crystal: a real set's crystal can't be pulled that far. The NCO
  // pins at the catch-range rail, the reference can't track the residual, and
  // the ident/killer drop the colour — a mono picture, the authentic failure.
  constexpr double offset = 2000.0;
  const auto env = synth_colour_composite(600, 1028, offset);
  const auto run = run_chroma(env, killer_test_config());
  CHECK(run.subcarrier_hz > 4.43361875e6 + 400.0); // pinned at (or near) the +500 Hz rail
  CHECK(run.subcarrier_hz < 4.43361875e6 + 501.0);
  CHECK(run.killer_gain < 0.1);
  const std::size_t fifth = run.out.size() / 5;
  CHECK(peak_chroma(std::span{run.out}.last(fifth)) < 1e-3f); // muted, not garbage colour
}

TEST_CASE("the APC pull is block-invariant (the feedback reaches the same sample)") {
  // The retune feeds back into the demod mix, so process() internally segments
  // at gate closes — a retune must land at the same sample whatever the
  // caller's chunking, or the loop's delay would be the block size.
  const auto env = synth_colour_composite(120, 1028, 200.0);

  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  sep.prepare(env.size());
  const auto sync = sep.process(env);
  video::HorizontalSweep hsweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  hsweep.prepare(sync.size());
  std::vector<video::BeamSample> hbeam;
  {
    const auto b = hsweep.process(sync);
    hbeam.assign(b.begin(), b.end());
  }

  video::ChromaDecoder whole{killer_test_config()};
  whole.prepare(env.size());
  std::vector<video::ChromaSample> ref;
  {
    const auto o = whole.process(env, hbeam);
    ref.assign(o.begin(), o.end());
  }

  const auto chunk = GENERATE(std::size_t{7}, std::size_t{333}, std::size_t{4096});
  CAPTURE(chunk);
  video::ChromaDecoder chunked{killer_test_config()};
  chunked.prepare(chunk);
  std::vector<video::ChromaSample> got;
  for (std::size_t off = 0; off < env.size(); off += chunk) {
    const std::size_t m = std::min(chunk, env.size() - off);
    const auto o = chunked.process(std::span{env}.subspan(off, m), std::span{hbeam}.subspan(off, m));
    got.insert(got.end(), o.begin(), o.end());
  }
  REQUIRE(got.size() == ref.size());
  for (std::size_t i = 0; i < ref.size(); ++i) {
    CHECK(got[i].u == ref[i].u);
    CHECK(got[i].v == ref[i].v);
  }
  CHECK(chunked.subcarrier_hz() == whole.subcarrier_hz());
}

TEST_CASE("APC catch range 0 pins the crystal (the pre-pull behaviour)") {
  const auto env = synth_colour_composite(300, 1028, 200.0);
  auto cfg = killer_test_config();
  cfg.apc_catch_range_hz = 0.0;
  const auto run = run_chroma(env, cfg);
  CHECK(run.subcarrier_hz == 4.43361875e6); // never retuned
  CHECK(run.killer_gain > 0.05); // the per-line rotation still locks an in-range source
}

TEST_CASE("the glass comb is the delay line at the real fixed geometry") {
  // The glass block is 283.5 subcarrier cycles = 1023 samples at 16 MS/s. On a
  // source whose lines are exactly that long, the adaptive delay-line comb
  // measures the same depth, so the two modes must agree bit-for-bit (after
  // the adaptive mode's first line-length measurement settles). On the
  // off-nominal 1028-sample lines they must diverge: the fixed block pairs
  // chroma displaced 5 samples along the line, ghosting colour transitions —
  // the real PAL-D off-spec misregistration the adaptive convenience hides.
  // Killer disabled: the comb is under test.
  const auto run_mode = [](std::span<const float> env, video::CombMode mode) {
    auto cfg = killer_test_config();
    cfg.killer_threshold = 0.0;
    cfg.comb_mode = mode;
    return run_chroma(env, cfg);
  };

  const auto on_spec = synth_colour_composite(60, 1023);
  const auto glass_on = run_mode(on_spec, video::CombMode::glass);
  const auto adaptive_on = run_mode(on_spec, video::CombMode::delay_line);
  REQUIRE(glass_on.out.size() == adaptive_on.out.size());
  for (std::size_t k = 5 * 1023; k < glass_on.out.size(); ++k) {
    CHECK(glass_on.out[k].u == adaptive_on.out[k].u);
    CHECK(glass_on.out[k].v == adaptive_on.out[k].v);
  }

  const auto off_spec = synth_colour_composite(60, 1028);
  const auto glass_off = run_mode(off_spec, video::CombMode::glass);
  const auto adaptive_off = run_mode(off_spec, video::CombMode::delay_line);
  REQUIRE(glass_off.out.size() == adaptive_off.out.size());
  float max_diff = 0.0f;
  for (std::size_t k = 5 * 1028; k < glass_off.out.size(); ++k)
    max_diff = std::max({max_diff, std::abs(glass_off.out[k].u - adaptive_off.out[k].u),
        std::abs(glass_off.out[k].v - adaptive_off.out[k].v)});
  CHECK(max_diff > 0.1f); // the mistimed pairing visibly corrupts the chroma
}

TEST_CASE("ChromaDecoder rejects an out-of-range ref_tc_lines") {
  const auto cfg = [](double tc) { return video::ChromaDecoderConfig{.sample_rate_hz = kRate, .ref_tc_lines = tc}; };
  CHECK_THROWS_AS(video::ChromaDecoder{cfg(1.0)}, std::invalid_argument); // below the floor
  CHECK_THROWS_AS(video::ChromaDecoder{cfg(150.0)}, std::invalid_argument); // above the ceiling
  CHECK_THROWS_AS(video::ChromaDecoder{cfg(std::nan(""))}, std::invalid_argument); // NaN fails the range test
  CHECK_NOTHROW(video::ChromaDecoder{cfg(10.0)}); // the default, in range
}
