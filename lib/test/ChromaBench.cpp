#include "palindrome/chroma_decoder.hpp"
#include "palindrome/horizontal_sweep.hpp"
#include "palindrome/sync_separator.hpp"
#include "palindrome/video_types.hpp"

#include "VideoSynth.hpp"

#include <cstddef>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

namespace video = palindrome::video;
using videotest::kRate;
using videotest::synth_colour_composite;

namespace {
// 64 lines of 1028 samples is a shade over the CLI's 64k read, so each call
// works the same block size (and cache footprint) the pipeline does.
constexpr std::size_t kLines = 64;
constexpr std::size_t kLineLen = 1028;

// The timing rail the chroma decoder joins by index, built once off the
// separator (no sync low-pass, so the burst gate sits where
// ChromaDecoderTest's killer_test_config puts it).
std::vector<video::BeamSample> timing_rail(const std::vector<float> &env) {
  video::SyncSeparator sep{video::SyncSeparatorConfig{.sample_rate_hz = kRate}};
  sep.prepare(env.size());
  const auto sync = sep.process(env);
  video::HorizontalSweep hsweep{video::HorizontalSweepConfig{.sample_rate_hz = kRate}};
  hsweep.prepare(sync.size());
  const auto hbeam = hsweep.process(sync);
  return {hbeam.begin(), hbeam.end()};
}
} // namespace

TEST_CASE("chroma decoder throughput (64-line blocks)") {
  const auto env = synth_colour_composite(kLines, kLineLen, 200.0);
  const auto hbeam = timing_rail(env);

  // The production shape: the APC pull on, so process() splits the block at
  // every burst-gate close and the per-segment work (NCO mix, demod low-pass,
  // pass 3) runs once per line.
  BENCHMARK("ChromaDecoder::process, APC pull on") {
    static video::ChromaDecoder chroma = [] {
      video::ChromaDecoder c{
          video::ChromaDecoderConfig{.sample_rate_hz = kRate, .burst_gate_lo = 0.145, .burst_gate_hi = 0.175}};
      c.prepare(kLines * kLineLen);
      return c;
    }();
    return chroma.process(env, hbeam).size();
  };

  // Pull off: nothing feeds back, the block is one segment. The difference
  // from the case above is the whole cost of segmenting.
  BENCHMARK("ChromaDecoder::process, APC pull off") {
    static video::ChromaDecoder chroma = [] {
      video::ChromaDecoder c{video::ChromaDecoderConfig{
          .sample_rate_hz = kRate, .burst_gate_lo = 0.145, .burst_gate_hi = 0.175, .apc_catch_range_hz = 0.0}};
      c.prepare(kLines * kLineLen);
      return c;
    }();
    return chroma.process(env, hbeam).size();
  };
}
