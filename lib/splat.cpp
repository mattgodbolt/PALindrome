#include "palindrome/splat.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace palindrome::video {

namespace {
// Bands per worker thread: enough that the pull-based queue can even out an
// uneven row distribution, few enough that binning a record into the bands its
// spot straddles stays cheap (a spot is ~7 rows, a band tens).
constexpr std::size_t kBandsPerLane = 3;
} // namespace

SplatDeposit::SplatDeposit(std::size_t width, std::size_t height, std::size_t channels, std::size_t lanes,
    SplatKernels kernels) : width_{width}, height_{height}, channels_{channels}, kernels_{std::move(kernels)} {
  // Serial unless asked for a genuine multi-thread split; one lane is just the
  // calling thread with no queue.
  if (lanes <= 1) {
    bands_ = 1;
    return;
  }
  pool_ = std::make_unique<WorkQueue>(static_cast<unsigned>(lanes));
  bands_ = std::min(height_, lanes * kBandsPerLane);
  if (bands_ < 1)
    bands_ = 1;
  // Band boundaries: band b owns output rows [band_edges_[b], band_edges_[b+1]),
  // a near-equal split of [0, height). row_band_ is the inverse, row -> band, so
  // binning maps a spot's rows to bands with a table lookup, no per-record divide.
  band_edges_.resize(bands_ + 1);
  for (std::size_t b = 0; b <= bands_; ++b)
    band_edges_[b] = static_cast<std::int32_t>(b * height_ / bands_);
  row_band_.resize(height_);
  for (std::size_t b = 0; b < bands_; ++b)
    for (std::int32_t r = band_edges_[b]; r < band_edges_[b + 1]; ++r)
      row_band_[static_cast<std::size_t>(r)] = static_cast<std::uint16_t>(b);
  buckets_ = std::vector<Buffer<std::uint32_t>>(bands_);
}

void SplatDeposit::prepare(std::size_t max_records) {
  for (auto &bucket: buckets_)
    bucket.reserve(max_records); // worst case: a band could own every record
}

void SplatDeposit::row_span(const SplatRecord &s, std::int32_t &lo, std::int32_t &hi) const {
  const auto &kc = kernels_.classes[s.klass];
  const std::int32_t base = s.y_pixel - kc.radius_y;
  lo = std::max<std::int32_t>(0, base);
  hi = std::min<std::int32_t>(static_cast<std::int32_t>(height_), base + kc.stride_y);
}

inline void SplatDeposit::splat(
    const SplatRecord &s, std::span<float> fb, std::int32_t row_lo, std::int32_t row_hi) const {
  const auto &kc = kernels_.classes[s.klass];
  const std::int32_t base = s.y_pixel - kc.radius_y;
  const std::int32_t base_col = s.x_pixel - kc.radius_x;
  const float *const rweights = kc.lut_y + static_cast<std::size_t>(s.y_bin) * static_cast<std::size_t>(kc.stride_y);
  const float *const cweights = kc.lut_x + static_cast<std::size_t>(s.x_bin) * static_cast<std::size_t>(kc.stride_x);
  const float g0 = s.gun[0];
  const float g1 = s.gun[1];
  const float g2 = s.gun[2];
  // Hoist channels_ out of the loops: a member read the compiler otherwise repeats
  // per row. 1 or 3; spell both out so the guns stay in registers and the RGB
  // triple isn't a variable-trip loop.
  const std::size_t channels = channels_;
  // Clip the column span to the frame once; rows clip to the caller's [row_lo,
  // row_hi) (a band slab, or the whole screen for the serial path).
  const auto width = static_cast<std::int32_t>(width_);
  const std::int32_t j_lo = std::max<std::int32_t>(0, -base_col);
  const std::int32_t j_hi = std::min<std::int32_t>(kc.stride_x, width - base_col);
  const std::int32_t k_lo = std::max<std::int32_t>(0, row_lo - base);
  const std::int32_t k_hi = std::min<std::int32_t>(kc.stride_y, row_hi - base);
  for (std::int32_t k = k_lo; k < k_hi; ++k) {
    const auto rw = rweights[k];
    const auto row_base = static_cast<std::size_t>(base + k) * width_;
    for (std::int32_t j = j_lo; j < j_hi; ++j) {
      const auto pixel = row_base + static_cast<std::size_t>(base_col + j);
      const auto w = rw * cweights[j];
      float *cell = &fb[pixel * channels];
      cell[0] += g0 * w;
      if (channels == 3) {
        cell[1] += g1 * w;
        cell[2] += g2 * w;
      }
    }
  }
}

void SplatDeposit::apply(std::span<const SplatRecord> recs, std::span<float> framebuffer) {
  // Serial when there's no pool. Otherwise bin each spot into the bands it
  // touches (one sweep of the list), then run one task per band on the queue: a
  // task touches only its band's records, and bands write disjoint rows so they
  // never collide. Byte-identical to the serial loop - each output row lives in
  // one band and a band keeps its records in list (sample) order, so every
  // pixel's adds land in the same order.
  if (!pool_) {
    apply_serial(recs, framebuffer);
    return;
  }
  bin(recs);
  tasks_.clear();
  for (std::size_t b = 0; b < bands_; ++b)
    tasks_.emplace_back([this, recs, framebuffer, b] { apply_band(recs, framebuffer, static_cast<unsigned>(b)); });
  pool_->run(tasks_);
}

void SplatDeposit::bin(std::span<const SplatRecord> recs) {
  // Sort the record indices into one bucket per band; a spot straddles the 1-2
  // bands its rows fall in. prepare() has sized the buckets, so this only clears
  // and appends (the capacity guard is a one-time fallback for an unprepared
  // caller, e.g. a test - never hit on the streaming path).
  for (auto &bucket: buckets_) {
    bucket.clear();
    if (bucket.capacity() < recs.size())
      bucket.reserve(recs.size());
  }
  for (std::size_t i = 0; i < recs.size(); ++i) {
    std::int32_t lo = 0;
    std::int32_t hi = 0;
    row_span(recs[i], lo, hi);
    if (lo >= hi)
      continue; // fully off-frame: no band
    const unsigned b_lo = row_band_[static_cast<std::size_t>(lo)];
    const unsigned b_hi = row_band_[static_cast<std::size_t>(hi - 1)];
    for (unsigned b = b_lo; b <= b_hi; ++b)
      buckets_[b].push() = static_cast<std::uint32_t>(i); // capacity from prepare
  }
}

void SplatDeposit::apply_band(std::span<const SplatRecord> recs, std::span<float> fb, unsigned band) const {
  const std::int32_t row_lo = band_edges_[band];
  const std::int32_t row_hi = band_edges_[band + 1];
  for (const std::uint32_t idx: buckets_[band].view())
    splat(recs[idx], fb, row_lo, row_hi);
}

void SplatDeposit::apply_serial(std::span<const SplatRecord> recs, std::span<float> fb) const {
  const auto height = static_cast<std::int32_t>(height_);
  for (const auto &s: recs)
    splat(s, fb, 0, height);
}

} // namespace palindrome::video
