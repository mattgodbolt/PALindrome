#include "palindrome/video.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace palindrome::video {

namespace {
// Wrap a phase error into [-0.5, 0.5). Past that range the loop would push the
// wrong way around the cycle; this is the standard PLL phase-detector range.
[[nodiscard]] double wrap_error(double e) noexcept {
  e -= std::floor(e);
  return e < 0.5 ? e : e - 1.0;
}

// Slicer hysteresis (fraction of the floor-to-peak range) and the per-sample
// release time constant of the peak/floor trackers. Analog equivalents of
// fixed circuit values — picked once, not user-tuned.
constexpr double kSyncHysteresis = 0.05;
constexpr double kLevelRelease = 0.999999; // ~60 ms settle at 16 MS/s
} // namespace

// === SyncSeparator ===

SyncSeparator::SyncSeparator(const SyncSeparatorConfig &cfg) : cfg_{cfg} {
  if (!(cfg_.sample_rate_hz > 0.0))
    throw std::invalid_argument{"SyncSeparator: sample_rate_hz must be positive"};
  if (!(cfg_.sync_level + kSyncHysteresis * 0.5 < 1.0 && cfg_.sync_level - kSyncHysteresis * 0.5 > 0.0))
    throw std::invalid_argument{"SyncSeparator: sync_level +/- hysteresis must stay within (0, 1)"};
}

void SyncSeparator::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const SyncSample> SyncSeparator::process(std::span<const float> envelope) {
  const std::size_t n = envelope.size();
  out_.reserve(n);
  const std::span<SyncSample> dst = out_.write_n(n);

  // Seed peak/floor from the first sample we ever see, so the slice level is
  // meaningful from sample 0 rather than starting at a degenerate range of 0.
  if (!seeded_ && n > 0) {
    peak_ = floor_ = static_cast<double>(envelope[0]);
    seeded_ = true;
  }

  const double enter_frac = cfg_.sync_level + kSyncHysteresis * 0.5;
  const double leave_frac = cfg_.sync_level - kSyncHysteresis * 0.5;
  const double release = 1.0 - kLevelRelease;

  for (std::size_t k = 0; k < n; ++k) {
    const double env = static_cast<double>(envelope[k]);

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

    dst[k] = SyncSample{.envelope = envelope[k], .sync = sync_};
  }

  return out_.view();
}

// === HorizontalSweep ===

HorizontalSweep::HorizontalSweep(const HorizontalSweepConfig &cfg) : cfg_{cfg} {
  if (!(cfg_.sample_rate_hz > 0.0))
    throw std::invalid_argument{"HorizontalSweep: sample_rate_hz must be positive"};
  if (!(cfg_.nominal_line_hz > 0.0 && cfg_.nominal_line_hz < cfg_.sample_rate_hz / 2.0))
    throw std::invalid_argument{"HorizontalSweep: nominal_line_hz out of range"};
  if (!(cfg_.min_pulse_fraction > 0.0 && cfg_.min_pulse_fraction < cfg_.max_pulse_fraction))
    throw std::invalid_argument{"HorizontalSweep: min_pulse_fraction must be in (0, max_pulse_fraction)"};
  if (!(cfg_.max_pulse_fraction < cfg_.min_line_fraction))
    throw std::invalid_argument{"HorizontalSweep: max_pulse_fraction must be < min_line_fraction"};
  if (!(cfg_.min_line_fraction < 1.0))
    throw std::invalid_argument{"HorizontalSweep: min_line_fraction must be < 1"};
  if (!(cfg_.pll_kp >= 0.0 && cfg_.pll_kp <= 1.0))
    throw std::invalid_argument{"HorizontalSweep: pll_kp must be in [0, 1]"};
  if (!(cfg_.omega_clamp > 0.0 && cfg_.omega_clamp < 1.0))
    throw std::invalid_argument{"HorizontalSweep: omega_clamp must be in (0, 1)"};
  omega_ = cfg_.nominal_line_hz / cfg_.sample_rate_hz;
}

void HorizontalSweep::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const BeamSample> HorizontalSweep::process(std::span<const SyncSample> in) {
  const std::size_t n = in.size();
  out_.reserve(n);
  const std::span<BeamSample> dst = out_.write_n(n);

  const double nominal_omega = cfg_.nominal_line_hz / cfg_.sample_rate_hz;
  const double omega_lo = nominal_omega * (1.0 - cfg_.omega_clamp);
  const double omega_hi = nominal_omega * (1.0 + cfg_.omega_clamp);

  for (std::size_t k = 0; k < n; ++k) {
    const bool sync = in[k].sync;
    bool line_start = false;

    if (sync && !prev_sync_) {
      // Leading edge of a sync pulse: the timing reference. Stash where the
      // oscillator phase was here; we commit to it only once the trailing edge
      // confirms the pulse was line-width (not a vertical-interval pulse).
      leading_edge_sample_ = sample_index_;
      leading_edge_phase_ = phase_;
    }
    else if (!sync && prev_sync_) {
      // Trailing edge: the pulse width is now known. Accept it as line sync
      // only if it falls in the line-sync width window AND the horizontal-hold
      // lockout has elapsed since the last accepted edge. Width gating rejects
      // the broad pulses (too wide) and equalising pulses (too narrow); the
      // lockout rejects chroma-ripple retriggers on the back porch.
      const double width = static_cast<double>(sample_index_ - leading_edge_sample_);
      const double since = static_cast<double>(sample_index_ - last_accepted_sample_);
      const double min_pulse = cfg_.min_pulse_fraction / omega_;
      const double max_pulse = cfg_.max_pulse_fraction / omega_;
      const double min_gap = cfg_.min_line_fraction / omega_;
      // The hold gate needs a prior edge to measure against; before the first
      // accepted edge there is none, so don't let it veto edge zero.
      const bool gap_ok = !have_accepted_ || since >= min_gap;
      if (width >= min_pulse && width <= max_pulse && gap_ok) {
        // PI correction anchored to the LEADING edge (the sharp, stable sync
        // transition). err is the leading edge's phase, which should have been
        // 0. kp snaps it there; ki nudges omega toward the true line rate,
        // anti-windup clamped so a biased acquisition can't run omega away.
        const double err = wrap_error(leading_edge_phase_);
        phase_ -= cfg_.pll_kp * err;
        omega_ = std::clamp(omega_ - cfg_.pll_ki * err, omega_lo, omega_hi);
        last_accepted_sample_ = sample_index_;
        have_accepted_ = true;
        ++accepted_;
        line_start = true;
      }
      else {
        ++rejected_;
      }
    }

    dst[k] = BeamSample{
        .envelope = in[k].envelope,
        .h_phase = static_cast<float>(phase_ - std::floor(phase_)),
        .line_start = line_start,
    };

    // Advance the oscillator and wrap into [0, 1). floor() covers the rare case
    // where a kp snap pushed phase_ slightly negative.
    phase_ += omega_;
    phase_ -= std::floor(phase_);
    prev_sync_ = sync;
    ++sample_index_;
  }

  return out_.view();
}

} // namespace palindrome::video
