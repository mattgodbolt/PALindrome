#include "palindrome/cpu_deposit.hpp"
#include "palindrome/deposit_backend.hpp"
#include "palindrome/splat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using palindrome::video::CpuDepositBackend;
using palindrome::video::Frame;
using palindrome::video::Readout;
using palindrome::video::SplatKernels;
using palindrome::video::SplatRecord;

namespace {
constexpr std::size_t kWidth = 16;
constexpr std::size_t kHeight = 8;
constexpr std::size_t kChannels = 1;
constexpr float kOne = 1.0f; // a 1x1 spot of unit weight: a record lands its gun value exactly
constexpr Readout kLinear{.white = 4.0, .gain = 1.0, .gamma = 1.0}; // 4.0 of charge reads as 255

SplatKernels make_kernels() {
  SplatKernels k;
  k.bins = 1;
  k.classes.push_back(
      SplatKernels::Class{.radius_x = 0, .radius_y = 0, .stride_x = 1, .stride_y = 1, .lut_x = &kOne, .lut_y = &kOne});
  return k;
}

CpuDepositBackend make_backend() { return CpuDepositBackend{kWidth, kHeight, kChannels, 1, make_kernels()}; }

void splat(CpuDepositBackend &b, std::size_t x, std::size_t y, float gun) {
  const auto slab = b.acquire(1);
  slab[0] = SplatRecord{.x_pixel = static_cast<std::int16_t>(x),
      .y_pixel = static_cast<std::int16_t>(y),
      .x_bin = 0,
      .y_bin = 0,
      .gun = {gun, 0.0f, 0.0f},
      .klass = 0};
  b.commit(1);
}

std::vector<std::uint8_t> live(CpuDepositBackend &b) {
  Frame got;
  b.readout(kLinear, [&](Frame f) { got = std::move(f); });
  return got.pixels;
}

std::vector<std::uint8_t> latched(CpuDepositBackend &b) {
  Frame got;
  b.readout_latched(kLinear, [&](Frame f) { got = std::move(f); });
  return got.pixels;
}

// What an eager backend would read out after the same calls.
std::vector<std::uint8_t> expect(const std::vector<float> &charge) {
  std::vector<std::uint8_t> px(charge.size());
  for (std::size_t i = 0; i < charge.size(); ++i)
    px[i] = static_cast<std::uint8_t>(std::clamp(charge[i] * 255.0f / 4.0f, 0.0f, 255.0f) + 0.5f);
  return px;
}

constexpr std::size_t at(std::size_t x, std::size_t y) { return y * kWidth + x; }
} // namespace

TEST_CASE("CpuDepositBackend lands the owed fade before later records, and fades an empty field too") {
  auto b = make_backend();
  std::vector<float> ref(kWidth * kHeight, 0.0f);

  splat(b, 1, 1, 2.0f);
  b.end_field(0.5f);
  ref[at(1, 1)] = 2.0f * 0.5f;
  // An empty field: nothing lands between the two boundaries, and the second
  // boundary's fade must still land rather than replace the first.
  b.end_field(0.5f);
  ref[at(1, 1)] *= 0.5f;
  splat(b, 2, 2, 4.0f);
  ref[at(2, 2)] = 4.0f;
  CHECK(live(b) == expect(ref));
  CHECK(live(b) == expect(ref)); // a second read lands nothing new
}

TEST_CASE("CpuDepositBackend latch keeps the boundary state, copying only when the live phosphor moves on") {
  auto b = make_backend();
  std::vector<float> boundary(kWidth * kHeight, 0.0f);

  // Never latched: the latched readout is the live one.
  splat(b, 3, 3, 4.0f);
  boundary[at(3, 3)] = 4.0f;
  CHECK(latched(b) == expect(boundary));

  // The single-image driver's shape: latch at the boundary, end the field,
  // deposit the next field on top, and only then read the latch.
  b.latch();
  b.end_field(0.5f);
  splat(b, 4, 4, 4.0f);
  CHECK(latched(b) == expect(boundary));

  // Reading the live phosphor lands the fade and the record beneath the latch,
  // which must survive it.
  auto after = boundary;
  after[at(3, 3)] *= 0.5f;
  after[at(4, 4)] = 4.0f;
  CHECK(live(b) == expect(after));
  CHECK(latched(b) == expect(boundary));

  // A later latch supersedes the copied one.
  b.latch();
  CHECK(latched(b) == expect(after));
  b.end_field(0.5f);
  splat(b, 5, 5, 4.0f);
  CHECK(latched(b) == expect(after));
}

TEST_CASE("CpuDepositBackend latch every field, read the last: the single-image render's shape") {
  auto b = make_backend();
  std::vector<float> ref(kWidth * kHeight, 0.0f);

  // Three fields, each latched at its boundary with the previous field's fade
  // and records still unlanded, and nothing reading the live phosphor between.
  for (std::size_t field = 0; field < 3; ++field) {
    splat(b, field, field, 4.0f);
    ref[at(field, field)] = 4.0f;
    b.latch();
    b.end_field(0.5f);
    for (float &c: ref)
      c *= 0.5f;
  }
  splat(b, 7, 7, 4.0f); // the partial field after the last boundary
  // The latch is the last boundary: before its fade, without the partial field.
  auto last_boundary = ref;
  for (float &c: last_boundary)
    c *= 2.0f;
  CHECK(latched(b) == expect(last_boundary));

  // A live read with nothing owed or pending has nothing to land, and the
  // latch stays live through it.
  auto solo = make_backend();
  splat(solo, 1, 1, 4.0f);
  solo.latch();
  const auto at_latch = latched(solo);
  CHECK(live(solo) == at_latch);
  solo.end_field(0.5f);
  CHECK(live(solo) != at_latch); // the fade lands beneath the latch...
  CHECK(latched(solo) == at_latch); // ...which survives it
}
