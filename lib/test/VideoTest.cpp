#include "palindrome/video.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace video = palindrome::video;

namespace {

// A crude synthetic composite envelope: `lines` lines of `line_len` samples
// each, sync tip (1.0) at the start of every line, back porch + active video
// (0.3 black) with one white bar (0.0) so there is content to slice around.
// Negatively modulated, so sync is the peak — exactly what the separator
// expects. Enough lines for the sweep to lock.
std::vector<float> synth_composite(std::size_t lines, std::size_t line_len) {
  std::vector<float> e(lines * line_len, 0.3f);
  for (std::size_t l = 0; l < lines; ++l) {
    const std::size_t base = l * line_len;
    for (std::size_t k = 0; k < line_len; ++k) {
      if (k < line_len / 14) // ~4.6 us at 1028 samples/line: line-sync width
        e[base + k] = 1.0f;
      else if (k > line_len / 2 && k < line_len / 2 + 40) // a white bar mid-line
        e[base + k] = 0.0f;
    }
  }
  return e;
}

constexpr double kRate = 16.0e6;

// A synthetic composite carrying chroma: synth_composite plus ~10 cycles of
// colour burst on the back porch and a constant-phase subcarrier across the
// active video, so the chroma decoder has a real burst to gate and a steady
// colour to recover. Block-invariance (not colour accuracy) is what we assert.
std::vector<float> synth_colour_composite(std::size_t lines, std::size_t line_len) {
  auto e = synth_composite(lines, line_len);
  constexpr double fsc = 4.43361875e6;
  const double w = 2.0 * std::numbers::pi * fsc / kRate;
  for (std::size_t l = 0; l < lines; ++l) {
    const std::size_t base = l * line_len;
    for (std::size_t k = 0; k < line_len; ++k) {
      const double phase = w * static_cast<double>(base + k);
      const bool in_burst = k >= line_len / 12 && k < line_len / 12 + 36;
      const bool in_active = k > line_len / 6;
      if (in_burst)
        e[base + k] += 0.15f * static_cast<float>(std::sin(phase)); // back-porch burst
      else if (in_active)
        e[base + k] += 0.08f * static_cast<float>(std::cos(phase)); // steady chroma
    }
  }
  return e;
}

// Run separator -> sweep over `env` fed in fixed-size chunks, returning the
// full BeamSample stream. chunk == env.size() is the single-block reference.
std::vector<video::BeamSample> run_chunked(std::span<const float> env, std::size_t chunk) {
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  video::HorizontalSweep sweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  std::vector<video::BeamSample> out;
  for (std::size_t off = 0; off < env.size(); off += chunk) {
    const std::size_t n = std::min(chunk, env.size() - off);
    const std::span<const video::SyncSample> sync = sep.process(env.subspan(off, n));
    const std::span<const video::BeamSample> beam = sweep.process(sync);
    out.insert(out.end(), beam.begin(), beam.end());
  }
  return out;
}

} // namespace

TEST_CASE("separator + sweep are block-invariant (the streaming guarantee)") {
  const auto env = synth_composite(40, 1028);
  const auto whole = run_chunked(env, env.size());

  // Feed the same signal in awkward block sizes that straddle line and pulse
  // boundaries; the output must be bit-for-bit identical to the single call.
  for (const std::size_t chunk: {std::size_t{1}, std::size_t{7}, std::size_t{333}, std::size_t{4096}}) {
    const auto chunked = run_chunked(env, chunk);
    REQUIRE(chunked.size() == whole.size());
    for (std::size_t i = 0; i < whole.size(); ++i) {
      CHECK(chunked[i].h_phase == whole[i].h_phase);
      CHECK(chunked[i].line_start == whole[i].line_start);
    }
  }
}

TEST_CASE("Screen is block-invariant") {
  const auto env = synth_composite(40, 1028);

  // Pre-compute the two timing rails once so the test isolates the Screen.
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  const std::span<const video::SyncSample> sync = sep.process(env);
  std::vector<video::SyncSample> sync_copy{sync.begin(), sync.end()};

  video::HorizontalSweep hsweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  std::vector<video::BeamSample> hbeam;
  {
    const auto b = hsweep.process(sync_copy);
    hbeam.assign(b.begin(), b.end());
  }
  video::VerticalSync vsync{video::VerticalSyncConfig{.sample_rate_hz = kRate}};
  std::vector<video::VSample> vbeam;
  {
    const auto v = vsync.process(sync_copy);
    vbeam.assign(v.begin(), v.end());
  }

  const video::ScreenConfig cfg{.width = 320, .height = 64, .sample_rate_hz = kRate};

  // The screen now joins on the chroma rail; wrap the envelope as luma-only.
  std::vector<video::ChromaSample> pic(env.size());
  for (std::size_t i = 0; i < env.size(); ++i)
    pic[i] = video::ChromaSample{.luma = env[i]};

  video::Screen whole{cfg};
  whole.process(pic, hbeam, vbeam);
  const auto frame_whole = whole.snapshot();

  video::Screen chunked{cfg};
  for (std::size_t off = 0; off < pic.size(); off += 257) {
    const std::size_t n = std::min<std::size_t>(257, pic.size() - off);
    chunked.process(std::span{pic}.subspan(off, n), std::span{hbeam}.subspan(off, n), std::span{vbeam}.subspan(off, n));
  }
  const auto frame_chunked = chunked.snapshot();

  REQUIRE(frame_whole.pixels.size() == frame_chunked.pixels.size());
  for (std::size_t i = 0; i < frame_whole.pixels.size(); ++i)
    CHECK(frame_whole.pixels[i] == frame_chunked.pixels[i]);
}

TEST_CASE("ChromaDecoder is block-invariant (the streaming guarantee)") {
  const auto env = synth_colour_composite(40, 1028);

  // Pre-compute the horizontal rail once; the chroma decoder joins it by index.
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  const auto sync = sep.process(env);
  std::vector<video::SyncSample> sync_copy{sync.begin(), sync.end()};
  video::HorizontalSweep hsweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  std::vector<video::BeamSample> hbeam;
  {
    const auto b = hsweep.process(sync_copy);
    hbeam.assign(b.begin(), b.end());
  }

  const video::ChromaDecoderConfig cfg{.sample_rate_hz = kRate};

  video::ChromaDecoder whole{cfg};
  whole.prepare(env.size());
  std::vector<video::ChromaSample> ref;
  {
    const auto out = whole.process(env, hbeam);
    ref.assign(out.begin(), out.end());
  }

  // The same signal in awkward block sizes must reproduce it sample-for-sample —
  // the NCO phasor, FIR state, burst gate, comb ring and parity all carry across.
  for (const std::size_t chunk: {std::size_t{1}, std::size_t{7}, std::size_t{333}, std::size_t{4096}}) {
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
}
