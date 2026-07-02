#include "palindrome/sync_separator.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

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
std::span<const SyncSample> SyncSeparator::process_fixed(std::span<const float> envelope) {
  const std::size_t n = envelope.size();
  const auto dst = out_.write_n(n);

  const double enter = 1.0 - cfg_.slice_depth + kFixedHysteresis * 0.5;
  const double leave = 1.0 - cfg_.slice_depth - kFixedHysteresis * 0.5;

  for (std::size_t k = 0; k < n; ++k) {
    const auto env = static_cast<double>(envelope[k]);
    if (!sync_ && env >= enter)
      sync_ = true;
    else if (sync_ && env < leave)
      sync_ = false;
    dst[k] = SyncSample{.sync = sync_};
  }

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
