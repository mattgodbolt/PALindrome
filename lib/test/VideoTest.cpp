#include "palindrome/video.hpp"

#include <cstddef>
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

  video::Screen whole{cfg};
  whole.process(env, hbeam, vbeam);
  const auto frame_whole = whole.snapshot();

  video::Screen chunked{cfg};
  for (std::size_t off = 0; off < env.size(); off += 257) {
    const std::size_t n = std::min<std::size_t>(257, env.size() - off);
    chunked.process(std::span{env}.subspan(off, n), std::span{hbeam}.subspan(off, n), std::span{vbeam}.subspan(off, n));
  }
  const auto frame_chunked = chunked.snapshot();

  REQUIRE(frame_whole.grey.size() == frame_chunked.grey.size());
  for (std::size_t i = 0; i < frame_whole.grey.size(); ++i)
    CHECK(frame_whole.grey[i] == frame_chunked.grey[i]);
}
