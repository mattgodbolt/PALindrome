#include "palindrome/splat.hpp"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using palindrome::video::SplatDeposit;
using palindrome::video::SplatKernels;
using palindrome::video::SplatRecord;

namespace {
// A 720x576 RGB frame's worth of splats: ~one field of visible samples (32 MS/s
// real -> /2 envelope -> ~320k samples/field, most visible), the batch the
// Screen hands the deposit at each field boundary.
constexpr std::size_t kWidth = 720;
constexpr std::size_t kHeight = 576;
constexpr std::size_t kChannels = 3;
constexpr std::size_t kFieldSplats = 270000;
constexpr std::int32_t kSpot = 7; // ~the 720-line beam spot
constexpr std::int32_t kRadius = (kSpot - 1) / 2;

const std::vector<float> kWeights(kSpot, 1.0f / kSpot);

SplatKernels make_kernels() {
  SplatKernels k;
  k.bins = 1;
  k.classes.push_back(SplatKernels::Class{.radius_x = kRadius,
      .radius_y = kRadius,
      .stride_x = kSpot,
      .stride_y = kSpot,
      .lut_x = kWeights.data(),
      .lut_y = kWeights.data()});
  return k;
}

std::vector<SplatRecord> make_field() {
  std::vector<SplatRecord> recs;
  recs.reserve(kFieldSplats);
  for (std::size_t i = 0; i < kFieldSplats; ++i)
    recs.push_back(SplatRecord{.x_pixel = static_cast<std::int16_t>(kRadius + (i * 6131) % (kWidth - kSpot)),
        .y_pixel = static_cast<std::int16_t>((i * 7919) % kHeight),
        .x_bin = 0,
        .y_bin = 0,
        .gun = {0.4f, 0.6f, 0.8f},
        .klass = 0});
  return recs;
}
} // namespace

TEST_CASE("splat apply throughput (one field, by thread count)") {
  const auto recs = make_field();
  std::vector<float> fb(kWidth * kHeight * kChannels, 0.0f);
  for (const std::size_t lanes: {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    SplatDeposit dep{kWidth, kHeight, kChannels, lanes, make_kernels()};
    BENCHMARK("apply " + std::to_string(lanes) + " thread(s)") {
      dep.apply(recs, fb);
      return fb[0]; // keep the apply from being optimised away
    };
  }
}
