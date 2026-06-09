#include "palindrome/horizontal_sweep.hpp"

#include "palindrome/phase.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace palindrome::video {

namespace {
// A non-coincident edge drains the coincidence detector this many times faster
// than a coincident edge fills it: brief noise (a VCR head-switch, an impulse)
// doesn't drop a locked loop into fast acquisition, but a sustained phase step
// unlocks within a handful of lines — the RC integrator of a real coincidence
// detector, in saturating-counter form.
constexpr std::size_t kCoincidencePenalty = 4;
} // namespace

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
  if (!(cfg_.pll_ki >= 0.0))
    throw std::invalid_argument{"HorizontalSweep: pll_ki must be >= 0"};
  if (!(cfg_.acq_kp >= 0.0 && cfg_.acq_kp <= 1.0))
    throw std::invalid_argument{"HorizontalSweep: acq_kp must be in [0, 1]"};
  if (!(cfg_.acq_ki >= 0.0))
    throw std::invalid_argument{"HorizontalSweep: acq_ki must be >= 0"};
  if (!(cfg_.coincidence_window > 0.0 && cfg_.coincidence_window < 0.5))
    throw std::invalid_argument{"HorizontalSweep: coincidence_window must be in (0, 0.5)"};
  if (cfg_.coincidence_lines == 0)
    throw std::invalid_argument{"HorizontalSweep: coincidence_lines must be >= 1"};
  if (!(cfg_.omega_clamp > 0.0 && cfg_.omega_clamp < 1.0))
    throw std::invalid_argument{"HorizontalSweep: omega_clamp must be in (0, 1)"};
  omega_ = cfg_.nominal_line_hz / cfg_.sample_rate_hz;
}

void HorizontalSweep::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const BeamSample> HorizontalSweep::process(std::span<const SyncSample> in) {
  const std::size_t n = in.size();
  const auto dst = out_.write_n(n);

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
      const auto width = static_cast<double>(sample_index_ - leading_edge_sample_);
      const auto since = static_cast<double>(sample_index_ - last_accepted_sample_);
      const double min_pulse = cfg_.min_pulse_fraction / omega_;
      const double max_pulse = cfg_.max_pulse_fraction / omega_;
      const double min_gap = cfg_.min_line_fraction / omega_;
      // The hold gate needs a prior edge to measure against; before the first
      // accepted edge there is none, so don't let it veto edge zero.
      const bool gap_ok = !have_accepted_ || since >= min_gap;
      if (width >= min_pulse && width <= max_pulse && gap_ok) {
        // PI correction anchored to the LEADING edge (the sharp, stable sync
        // transition). err is the leading edge's phase, which should have been
        // 0. kp pulls it there; ki nudges omega toward the true line rate,
        // anti-windup clamped so a biased acquisition can't run omega away.
        // The gains come from the coincidence detector's state BEFORE this
        // edge updates it: a phase step hitting a locked loop is corrected
        // slowly first (the flagging/recentring of a real flywheel), and only
        // once the detector drains does the loop re-acquire fast.
        const double err = dsp::wrap_error(leading_edge_phase_);
        const double kp = locked_ ? cfg_.pll_kp : cfg_.acq_kp;
        const double ki = locked_ ? cfg_.pll_ki : cfg_.acq_ki;
        phase_ -= kp * err;
        omega_ = std::clamp(omega_ - ki * err, omega_lo, omega_hi);
        if (std::abs(err) <= cfg_.coincidence_window)
          coincidence_ = std::min(coincidence_ + 1, 2 * cfg_.coincidence_lines);
        else
          coincidence_ -= std::min(coincidence_, kCoincidencePenalty);
        locked_ = coincidence_ >= cfg_.coincidence_lines;
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
