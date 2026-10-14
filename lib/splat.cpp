#include "palindrome/splat.hpp"

#include "palindrome/restrict_ptr.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace palindrome::video {

namespace {
// Bands per worker thread: enough that the pull-based queue can even out an
// uneven row distribution, few enough that binning a record into the bands its
// spot straddles stays cheap (a spot is ~7 rows, a band tens).
constexpr std::size_t kBandsPerLane = 3;

// The vector fast path (#61): a spot row's touched cells are contiguous floats,
// and the value added to cell i = channels*j + c is gun[c] * (rw * cw[j]) - a
// product of two per-record vectors scaled by a per-row scalar. Expanding the
// column weights into the framebuffer's interleaved layout (cwe[i] = cw[j]) and
// tiling the guns (gt[i] = gun[c]) once per record turns each row deposit into
// one contiguous stream op, fb[i] += gt[i] * (rw * cwe[i]), with exactly the
// scalar loop's operations and rounding - round(rw*cw), then one fma per
// channel - so the framebuffer stays bit-identical. Patterns are zero-padded to
// whole 8-float lanes: a runtime-width row loop costs a scalar epilogue that
// eats the vector win at these widths, so the loop trip is a compile-time
// constant and the pad lanes deposit +0.0, a no-op on a framebuffer that can
// never hold -0.0 (weights and gun outputs are non-negative; the gun cuts
// everything <= 0 to +0.0f).
constexpr std::size_t kChunkRecords = 64;
constexpr std::size_t kMaxPattern = 32; // padded pattern floats; wider spots keep the scalar path

// One interior spot's rows against its pattern pair. rweights is pre-offset to
// the first deposited row; row0 points at that row's leftmost touched cell.
template<std::size_t N>
void deposit_rows(restrict_ptr<float> row0, std::size_t row_floats, restrict_ptr<const float> rweights,
    std::int32_t rows, restrict_ptr<const float> cwe, restrict_ptr<const float> gt) {
  for (std::int32_t k = 0; k < rows; ++k) {
    const auto rw = rweights[k];
    const auto row = row0 + static_cast<std::ptrdiff_t>(k) * static_cast<std::ptrdiff_t>(row_floats);
    for (std::size_t i = 0; i < N; ++i)
      row[i] += gt[i] * (rw * cwe[i]);
  }
}
} // namespace

SplatDeposit::SplatDeposit(std::size_t width, std::size_t height, std::size_t channels, std::size_t lanes,
    SplatKernels kernels) : width_{width}, height_{height}, channels_{channels}, kernels_{std::move(kernels)} {
  padded_.reserve(kernels_.classes.size());
  for (const auto &kc: kernels_.classes) {
    const auto need = static_cast<std::size_t>(kc.stride_x) * channels_;
    padded_.push_back(need <= kMaxPattern ? static_cast<std::int32_t>((need + 7) & ~std::size_t{7}) : 0);
  }
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

void SplatDeposit::apply_chunk(
    std::span<const SplatRecord *const> chunk, std::span<float> fb, std::int32_t row_lo, std::int32_t row_hi) const {
  // Two passes: build every record's pattern pair, then deposit them all. The
  // separation is load-bearing - the builds are scalar stores and the deposits
  // read them back 8-wide, and Skylake cannot forward a pending scalar store
  // into a vector load (measured as ~12-cycle stalls on every row when a
  // pattern is used right after it is built). A chunk apart, the stores have
  // drained to L1 and the loads are clean.
  alignas(32) std::array<float, kChunkRecords * 2 * kMaxPattern> arena;
  std::array<bool, kChunkRecords> fast{};
  const auto width_floats = width_ * channels_;
  for (std::size_t r = 0; r < chunk.size(); ++r) {
    const auto &s = *chunk[r];
    const auto padded = padded_[s.klass];
    const auto &kc = kernels_.classes[s.klass];
    const std::int32_t base_col = s.x_pixel - kc.radius_x;
    // The fast path writes the pattern's full padded width, so the whole padded
    // span - not just the spot - must sit inside the row. Edge spots (and any
    // over-wide class) keep the scalar splat(), which clips per pixel.
    if (padded == 0 || base_col < 0 ||
        static_cast<std::size_t>(base_col) * channels_ + static_cast<std::size_t>(padded) > width_floats)
      continue;
    fast[r] = true;
    const float *const cw = kc.lut_x + static_cast<std::size_t>(s.x_bin) * static_cast<std::size_t>(kc.stride_x);
    const auto cwe = restrict_ptr<float>{&arena[r * 2 * kMaxPattern]};
    const auto gt = cwe + kMaxPattern;
    std::size_t o = 0;
    for (std::int32_t j = 0; j < kc.stride_x; ++j) {
      for (std::size_t c = 0; c < channels_; ++c) {
        cwe[o] = cw[j];
        gt[o] = s.gun[c];
        ++o;
      }
    }
    for (; o < static_cast<std::size_t>(padded); ++o) {
      cwe[o] = 0.0f;
      gt[o] = 0.0f;
    }
  }
  for (std::size_t r = 0; r < chunk.size(); ++r) {
    const auto &s = *chunk[r];
    if (!fast[r]) {
      splat(s, fb, row_lo, row_hi);
      continue;
    }
    const auto &kc = kernels_.classes[s.klass];
    const std::int32_t base = s.y_pixel - kc.radius_y;
    const std::int32_t k_lo = std::max<std::int32_t>(0, row_lo - base);
    const std::int32_t k_hi = std::min<std::int32_t>(kc.stride_y, row_hi - base);
    if (k_lo >= k_hi)
      continue;
    const float *const rweights = kc.lut_y + static_cast<std::size_t>(s.y_bin) * static_cast<std::size_t>(kc.stride_y);
    const std::int32_t base_col = s.x_pixel - kc.radius_x;
    float *const row0 =
        fb.data() + (static_cast<std::size_t>(base + k_lo) * width_ + static_cast<std::size_t>(base_col)) * channels_;
    const float *const cwe = &arena[r * 2 * kMaxPattern];
    const float *const gt = cwe + kMaxPattern;
    const std::int32_t rows = k_hi - k_lo;
    switch (padded_[s.klass]) {
      case 8: deposit_rows<8>(row0, width_floats, rweights + k_lo, rows, cwe, gt); break;
      case 16: deposit_rows<16>(row0, width_floats, rweights + k_lo, rows, cwe, gt); break;
      case 24: deposit_rows<24>(row0, width_floats, rweights + k_lo, rows, cwe, gt); break;
      case 32: deposit_rows<32>(row0, width_floats, rweights + k_lo, rows, cwe, gt); break;
      default: std::unreachable(); // padded_ is 0 (handled above) or a whole lane count <= kMaxPattern
    }
  }
}

void SplatDeposit::apply_band(std::span<const SplatRecord> recs, std::span<float> fb, unsigned band) const {
  const std::int32_t row_lo = band_edges_[band];
  const std::int32_t row_hi = band_edges_[band + 1];
  std::array<const SplatRecord *, kChunkRecords> chunk;
  std::size_t n = 0;
  for (const std::uint32_t idx: buckets_[band].view()) {
    chunk[n++] = &recs[idx];
    if (n == kChunkRecords) {
      apply_chunk(std::span{chunk.data(), n}, fb, row_lo, row_hi);
      n = 0;
    }
  }
  apply_chunk(std::span{chunk.data(), n}, fb, row_lo, row_hi);
}

void SplatDeposit::apply_serial(std::span<const SplatRecord> recs, std::span<float> fb) const {
  const auto height = static_cast<std::int32_t>(height_);
  std::array<const SplatRecord *, kChunkRecords> chunk;
  std::size_t n = 0;
  for (const auto &s: recs) {
    chunk[n++] = &s;
    if (n == kChunkRecords) {
      apply_chunk(std::span{chunk.data(), n}, fb, 0, height);
      n = 0;
    }
  }
  apply_chunk(std::span{chunk.data(), n}, fb, 0, height);
}

} // namespace palindrome::video
