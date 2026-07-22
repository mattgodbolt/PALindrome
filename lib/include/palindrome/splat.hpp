#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/work_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace palindrome::video {

// One beam-spot deposit: where the beam landed, how bright, and which spot size.
// Just (x, y, colour) - the position to sub-pixel precision (an integer pixel
// plus a sub-pixel bin per axis, the bin selecting a pre-shifted copy of the
// Gaussian kernel) and the three gun intensities - plus a one-byte focus-class
// index (the spot defocuses as the EHT sags, so its size drifts; the index names
// which pre-baked kernel applies). Everything else the apply needs (the spot's
// radius, stride, and weight tables) is a property of the class, looked up from
// the SplatKernels, not stored per record. 24 bytes, trivially copyable so a
// field of them lives in a Buffer and could ship to a GPU unchanged.
struct SplatRecord {
  std::int16_t x_pixel; // floor(beam x); the left column is x_pixel - radius_x(class)
  std::int16_t y_pixel; // floor(beam y); the top row is y_pixel - radius_y(class)
  std::uint16_t x_bin; // sub-pixel x fraction -> column-kernel row
  std::uint16_t y_bin; // sub-pixel y fraction -> row-kernel row
  float gun[3]; // the three gun deposits (gun[1..2] unused when channels == 1)
  std::uint8_t klass; // focus-class index into SplatKernels
};
static_assert(sizeof(SplatRecord) == 24);

// The separable Gaussian kernels, one set per focus class. Each class's row
// (lut_y) and column (lut_x) tables are bins x stride floats, normalised, indexed
// [bin * stride + cell]: the same beam-spot tables the Screen builds. The
// pointers reference storage that outlives the SplatDeposit (the Screen's LUTs).
struct SplatKernels {
  std::size_t bins = 1; // sub-pixel bins per axis (the LUT row count)
  struct Class {
    std::int32_t radius_x;
    std::int32_t radius_y;
    std::int32_t stride_x; // 2 * radius_x + 1 (the kernel width)
    std::int32_t stride_y;
    const float *lut_x; // bins * stride_x column weights
    const float *lut_y; // bins * stride_y row weights
  };
  std::vector<Class> classes;
};

// Applies a batch of SplatRecords additively into a phosphor framebuffer. The
// work splits by output band: lane L owns a contiguous row slab, so each pixel
// has exactly one writer and a band keeps its records in list (sample) order -
// the result is byte-identical whatever the lane count, a wall-clock knob only.
// Built for the frame-buffered deposit (the Screen accumulates a field of
// records, then apply()s once at the field boundary, fanning a whole field across
// the lanes at once) and as the natural staging for a future GPU scatter.
//
// Standalone and stateless between calls (the framebuffer is the caller's), so it
// is benchmarked and tested directly with synthetic records and lane counts.
class SplatDeposit {
public:
  // lanes <= 1 keeps everything on the calling thread (no pool). channels is 1
  // (grey) or 3 (RGB), matching the framebuffer's interleaving. kernels resolve a
  // record's (class, bin) to its weight rows and spot geometry.
  SplatDeposit(std::size_t width, std::size_t height, std::size_t channels, std::size_t lanes, SplatKernels kernels);

  // Size the per-band buckets once for batches of up to max_records, so the
  // per-field bin never allocates. Must be called (with a bound >= the largest
  // batch apply() will see) before apply() when threaded; Screen does so from its
  // own prepare().
  void prepare(std::size_t max_records);

  [[nodiscard]] unsigned lanes() const noexcept { return pool_ ? pool_->threads() : 1u; }

  // Add every record's spot into framebuffer (width*height*channels floats),
  // additively. Records whose rows fall off the frame are clipped. NOT reentrant:
  // it reuses internal scratch (tasks_/buckets_) and fans it to the pool, so only
  // one apply() may be in flight at a time. The caller (one in-order Screen
  // thread) guarantees that.
  void apply(std::span<const SplatRecord> recs, std::span<float> framebuffer);

private:
  // The on-screen row range a record's spot covers, [lo, hi); lo >= hi if fully
  // off-frame. Shared by bin() and the serial apply.
  void row_span(const SplatRecord &s, std::int32_t &lo, std::int32_t &hi) const;
  void bin(std::span<const SplatRecord> recs);
  void apply_band(std::span<const SplatRecord> recs, std::span<float> fb, unsigned band) const;
  void apply_serial(std::span<const SplatRecord> recs, std::span<float> fb) const;
  // Deposit one chunk of records: pattern-build them all, then splat them all
  // (see the .cpp for why the two passes must be separated), interior spots via
  // the padded vector row loop and the rest via the scalar splat(). Chunks are
  // how both the serial and the per-band paths consume their record streams.
  void apply_chunk(
      std::span<const SplatRecord *const> chunk, std::span<float> fb, std::int32_t row_lo, std::int32_t row_hi) const;
  // Deposit one record's spot, but only its rows within [row_lo, row_hi).
  // Force-inlined: it's called once per record (hundreds of thousands per field)
  // and is small once column-clipped, so the out-of-line call's prologue dominates.
  [[gnu::always_inline]] void splat(
      const SplatRecord &s, std::span<float> fb, std::int32_t row_lo, std::int32_t row_hi) const;

  std::size_t width_;
  std::size_t height_;
  std::size_t channels_;
  SplatKernels kernels_;
  // Per focus class: the pattern width in floats for the vector row loop -
  // stride_x * channels rounded up to whole 8-float lanes - or 0 for a spot too
  // wide for the fast path (which then always takes the scalar splat()).
  std::vector<std::int32_t> padded_;
  // More bands than threads, so the pull-based queue balances an uneven row
  // distribution (e.g. the blanked top/bottom bands carry fewer splats) instead
  // of pinning one band per thread. Bit-exactness is independent of the count:
  // each output row lives in exactly one band whatever the split.
  std::size_t bands_;
  std::unique_ptr<WorkQueue> pool_; // null when serial (lanes <= 1)
  std::vector<std::function<void()>> tasks_; // reused per-apply band-task closures
  // bin() sorts the record indices into one bucket per band (record order within a
  // band). Each bucket is reserved once in prepare() to the worst case (a band
  // could in the limit own every record), so the per-field bin never allocates.
  std::vector<Buffer<std::uint32_t>> buckets_;
  std::vector<std::int32_t> band_edges_; // band b owns rows [band_edges_[b], band_edges_[b+1])
  std::vector<std::uint16_t> row_band_; // row -> owning band, the inverse of band_edges_
};

} // namespace palindrome::video
