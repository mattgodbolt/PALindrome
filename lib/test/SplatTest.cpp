#include "palindrome/splat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using palindrome::video::SplatDeposit;
using palindrome::video::SplatKernels;
using palindrome::video::SplatRecord;

namespace {
constexpr std::size_t kWidth = 320;
constexpr std::size_t kHeight = 200;
constexpr std::size_t kChannels = 3;
constexpr std::int32_t kSpot = 5; // spot height/width in cells
constexpr std::int32_t kRadius = (kSpot - 1) / 2;

// One focus class, a single sub-pixel bin, uniform weights. The apply only
// multiplies by the weights, it never assumes a shape, so this exercises every
// path without needing a real Gaussian table.
const std::vector<float> kWeights(kSpot, 0.2f);

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

// A deterministic field of splats spread across the whole frame, so every band
// gets work. No RNG: positions and gun values come from the index.
std::vector<SplatRecord> make_field(std::size_t count) {
  std::vector<SplatRecord> recs;
  recs.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
    recs.push_back(SplatRecord{.x_pixel = static_cast<std::int16_t>(kRadius + (i * 6131) % (kWidth - kSpot)),
        .y_pixel = static_cast<std::int16_t>((i * 7919) % kHeight),
        .x_bin = 0,
        .y_bin = 0,
        .gun = {static_cast<float>(i % 13) * 0.1f, static_cast<float>(i % 7) * 0.2f, static_cast<float>(i % 5) * 0.3f},
        .klass = 0});
  return recs;
}

std::vector<float> deposit_with(std::size_t lanes, std::span<const SplatRecord> recs) {
  SplatDeposit dep{kWidth, kHeight, kChannels, lanes, make_kernels()};
  std::vector<float> fb(kWidth * kHeight * kChannels, 0.0f);
  dep.apply(recs, fb);
  return fb;
}
} // namespace

TEST_CASE("SplatDeposit is bit-exact across thread counts") {
  // The band split must be a pure wall-clock knob: every lane count must produce
  // a byte-identical framebuffer, since each output row is owned by one band and
  // a band keeps its records in list (sample) order.
  const auto recs = make_field(20000);
  const auto serial = deposit_with(1, recs);
  for (const std::size_t lanes: {std::size_t{2}, std::size_t{3}, std::size_t{4}, std::size_t{8}, std::size_t{13}}) {
    const auto threaded = deposit_with(lanes, recs);
    REQUIRE(threaded.size() == serial.size());
    bool identical = true;
    for (std::size_t i = 0; i < serial.size(); ++i)
      if (threaded[i] != serial[i]) {
        identical = false;
        break;
      }
    INFO("lanes = " << lanes);
    CHECK(identical);
  }
}

TEST_CASE("SplatDeposit clips spots that fall off the frame") {
  // A spot straddling the top edge deposits only its on-frame rows; a fully
  // off-frame spot deposits nothing. Threaded and serial agree.
  std::vector<SplatRecord> recs;
  recs.push_back(SplatRecord{.x_pixel = static_cast<std::int16_t>(10 + kRadius),
      .y_pixel = 0, // base = -kRadius: the top kRadius rows are off-screen
      .x_bin = 0,
      .y_bin = 0,
      .gun = {1.0f, 1.0f, 1.0f},
      .klass = 0});
  recs.push_back(SplatRecord{.x_pixel = static_cast<std::int16_t>(10 + kRadius),
      .y_pixel = static_cast<std::int16_t>(kHeight + kSpot), // base well below the frame
      .x_bin = 0,
      .y_bin = 0,
      .gun = {1.0f, 1.0f, 1.0f},
      .klass = 0});

  const auto serial = deposit_with(1, recs);
  const auto threaded = deposit_with(4, recs);
  REQUIRE(serial == threaded);

  const auto cell = [&](std::int32_t row, std::int32_t col) {
    return serial[(static_cast<std::size_t>(row) * kWidth + static_cast<std::size_t>(col)) * kChannels];
  };
  CHECK(cell(0, 10) > 0.0f); // the on-frame part of the first spot
  CHECK(cell(2, 10) > 0.0f);
  CHECK(cell(50, 10) == 0.0f); // nothing from the off-frame spot
}
