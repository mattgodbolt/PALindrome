#include "palindrome/sync_separator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace palindrome::video {

namespace {
// Slicer hysteresis and (adaptive mode) the per-sample release time constant
// of the peak/floor trackers. Analog equivalents of fixed circuit values -
// picked once, not user-tuned. Fixed-mode hysteresis is in tip units (the AGC
// holds the tip at 1.0); adaptive-mode hysteresis is a fraction of the tracked
// floor-to-peak range.
constexpr double kFixedHysteresis = 0.01;
constexpr double kSyncHysteresis = 0.05;
// Per-sample RETENTION factor of the trackers (the release rate is 1 - this):
// ~60 ms settle at 16 MS/s. Named for what it multiplies, so it can't be
// misread as the rate it is inverted into below.
constexpr double kLevelRetain = 0.999999;

// The smallest float that is >= x. A float sample compared against a double
// threshold t (s >= t, s < t) gets exactly the same verdict against this float,
// because no float lies strictly between t and its round-up; the
// nearest-rounded float can sit just below t and flip the verdict for a sample
// landing exactly on it.
[[nodiscard]] float ceil_to_float(double x) {
  const auto f = static_cast<float>(x);
  return static_cast<double>(f) < x ? std::nextafter(f, std::numeric_limits<float>::infinity()) : f;
}

#if defined(__AVX2__)
// One 64-sample word of the hysteresis slicer, resolved as a carry chain. Each
// sample either sets the state (at or above enter), clears it (below leave -
// the thresholds are disjoint, so never both) or holds it, and the state after
// any sample is whichever of set/clear happened most recently. That is a binary
// adder's carry: a set bit generates, a clear bit kills, a hold bit propagates.
// With A = ~clear and B = set, A & B is the generate mask and A ^ B the
// propagate mask, so the carry into bit k of A + B (+ the incoming state) is the
// state after sample k - 1. Bit k of the result is the state after sample k;
// the carry out of the word is the state after sample 63, handed on as the next
// word's carry in. Precondition: set & clear == 0 (the adder propagates where
// the per-sample form would toggle).
[[nodiscard]] std::uint64_t resolve_word(std::uint64_t set, std::uint64_t clear, bool &state) {
  const auto not_clear = ~clear;
  std::uint64_t sum = 0;
  const auto overflow_a = __builtin_add_overflow(not_clear, set, &sum);
  const auto overflow_b = __builtin_add_overflow(sum, static_cast<std::uint64_t>(state), &sum);
  const auto carry_in = sum ^ not_clear ^ set;
  state = overflow_a || overflow_b;
  return (carry_in >> 1) | (static_cast<std::uint64_t>(state) << 63);
}

// TODO(std::simd): the compare-to-mask and mask-to-bytes helpers are the SIMD
// idioms here; the carry-chain resolve between them is scalar by nature.
template<int predicate>
[[nodiscard]] std::uint64_t compare_mask(const float *src, __m256 threshold) {
  std::uint64_t mask = 0;
  for (int c = 0; c < 8; ++c) {
    const auto v = _mm256_loadu_ps(src + 8 * c);
    const auto bits = static_cast<unsigned>(_mm256_movemask_ps(_mm256_cmp_ps(v, threshold, predicate)));
    mask |= static_cast<std::uint64_t>(bits) << (8 * c);
  }
  return mask;
}

// Spread 64 state bits to 64 one-byte SyncSamples: each 32-bit half of the mask
// is broadcast, byte j of the vector picks mask byte j / 8 and tests bit j % 8,
// and min-with-1 turns the surviving bit into the 0/1 a bool must hold.
void expand_mask(std::uint64_t mask, SyncSample *dst) {
  static_assert(sizeof(SyncSample) == 1);
  const auto byte_select =
      _mm256_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3);
  const auto bit_select = _mm256_setr_epi8(1, 2, 4, 8, 16, 32, 64, -128, 1, 2, 4, 8, 16, 32, 64, -128, 1, 2, 4, 8, 16,
      32, 64, -128, 1, 2, 4, 8, 16, 32, 64, -128);
  const auto one = _mm256_set1_epi8(1);
  for (int half = 0; half < 2; ++half) {
    const auto broadcast = _mm256_set1_epi32(static_cast<int>(static_cast<std::uint32_t>(mask >> (32 * half))));
    const auto bits = _mm256_and_si256(_mm256_shuffle_epi8(broadcast, byte_select), bit_select);
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + 32 * half), _mm256_min_epu8(bits, one));
  }
}
#endif
} // namespace

SyncSeparator::SyncSeparator(const SyncSeparatorConfig &cfg) : cfg_{cfg} {
  if (!(cfg_.sample_rate_hz > 0.0))
    throw std::invalid_argument{"SyncSeparator: sample_rate_hz must be positive"};
  if (cfg_.adaptive) {
    if (!(cfg_.sync_level + kSyncHysteresis * 0.5 < 1.0 && cfg_.sync_level - kSyncHysteresis * 0.5 > 0.0))
      throw std::invalid_argument{"SyncSeparator: sync_level +/- hysteresis must stay within (0, 1)"};
  }
  else {
    if (!(cfg_.slice_depth - kFixedHysteresis * 0.5 > 0.0 && cfg_.slice_depth + kFixedHysteresis * 0.5 < 1.0))
      throw std::invalid_argument{"SyncSeparator: slice_depth +/- hysteresis must stay within (0, 1)"};
  }
}

void SyncSeparator::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const SyncSample> SyncSeparator::process(std::span<const float> envelope) {
  return cfg_.adaptive ? process_adaptive(envelope) : process_fixed(envelope);
}

// The period slicer: the AGC upstream holds the sync tip at 1.0, so the slice
// level is a constant - sync is anything above it. Picture content can never
// move the slice point; an under-modulated source with shallow sync still
// slices cleanly as long as its sync reaches below slice_depth.
//
// Sliced 64 samples at a time - two compare masks and a carry-chain resolve
// (resolve_word), the same boolean function as the per-sample form, which
// takes the tail.
std::span<const SyncSample> SyncSeparator::process_fixed(std::span<const float> envelope) {
  const std::size_t n = envelope.size();
  const auto dst = out_.write_n(n);

  const float enter = ceil_to_float(1.0 - cfg_.slice_depth + kFixedHysteresis * 0.5);
  const float leave = ceil_to_float(1.0 - cfg_.slice_depth - kFixedHysteresis * 0.5);

  auto sync = sync_;
  std::size_t k = 0;
#if defined(__AVX2__)
  const auto enter_v = _mm256_set1_ps(enter);
  const auto leave_v = _mm256_set1_ps(leave);
  for (; k + 64 <= n; k += 64) {
    const auto set = compare_mask<_CMP_GE_OQ>(envelope.data() + k, enter_v);
    const auto clear = compare_mask<_CMP_LT_OQ>(envelope.data() + k, leave_v);
    expand_mask(resolve_word(set, clear, sync), dst.data() + k);
  }
#endif
  for (; k < n; ++k) {
    const auto env = envelope[k];
    if (!sync && env >= enter)
      sync = true;
    else if (sync && env < leave)
      sync = false;
    dst[k] = SyncSample{.sync = sync};
  }
  sync_ = sync;

  return out_.view();
}

std::span<const SyncSample> SyncSeparator::process_adaptive(std::span<const float> envelope) {
  const std::size_t n = envelope.size();
  const auto dst = out_.write_n(n);

  // Seed peak/floor from the first sample we ever see, so the slice level is
  // meaningful from sample 0 rather than starting at a degenerate range of 0.
  if (!seeded_ && n > 0) {
    peak_ = floor_ = static_cast<double>(envelope[0]);
    seeded_ = true;
  }

  const double enter_frac = cfg_.sync_level + kSyncHysteresis * 0.5;
  const double leave_frac = cfg_.sync_level - kSyncHysteresis * 0.5;
  const double release = 1.0 - kLevelRetain;

  for (std::size_t k = 0; k < n; ++k) {
    const auto env = static_cast<double>(envelope[k]);

    // Track peak (sync tip) and floor (white) with fast attack to a new
    // extreme and slow release back toward the other end. Both release terms
    // read the pre-update pair, so there's no within-sample order dependency.
    const double range_pre = peak_ - floor_;
    peak_ = std::max(env, peak_ - range_pre * release);
    floor_ = std::min(env, floor_ + range_pre * release);
    const double range = peak_ - floor_;

    // Slice with hysteresis: enter the sync region high, leave it lower, so
    // chroma ripple on a transition can't chatter the bit. The range > 0 guard
    // stops a degenerate flat input (range == 0, where the enter test would be
    // trivially true) from latching permanently into sync.
    if (!sync_ && range > 0.0 && env >= floor_ + range * enter_frac)
      sync_ = true;
    else if (sync_ && env < floor_ + range * leave_frac)
      sync_ = false;

    dst[k] = SyncSample{.sync = sync_};
  }

  return out_.view();
}

} // namespace palindrome::video
