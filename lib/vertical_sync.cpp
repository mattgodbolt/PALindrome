#include "palindrome/vertical_sync.hpp"

#include "palindrome/phase.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace palindrome::video {

namespace {
constexpr double kVsyncHysteresis = 0.05; // integrator slice hysteresis for vertical sync
} // namespace

VerticalSync::VerticalSync(const VerticalSyncConfig &cfg) : cfg_{cfg} {
  if (!(cfg_.sample_rate_hz > 0.0))
    throw std::invalid_argument{"VerticalSync: sample_rate_hz must be positive"};
  if (!(cfg_.nominal_field_hz > 0.0 && cfg_.nominal_field_hz < cfg_.sample_rate_hz / 2.0))
    throw std::invalid_argument{"VerticalSync: nominal_field_hz out of range"};
  if (!(cfg_.nominal_line_hz > cfg_.nominal_field_hz))
    throw std::invalid_argument{"VerticalSync: nominal_line_hz must exceed nominal_field_hz"};
  // Lower bound 1/line_samples keeps alpha <= 1, so the integrator stays a
  // convex combination (integ_ can't overshoot [0, 1] and ring).
  if (!(cfg_.integrator_tc_lines * cfg_.sample_rate_hz / cfg_.nominal_line_hz >= 1.0))
    throw std::invalid_argument{"VerticalSync: integrator_tc_lines too small (alpha would exceed 1)"};
  if (!(cfg_.vsync_level - kVsyncHysteresis * 0.5 > 0.0 && cfg_.vsync_level + kVsyncHysteresis * 0.5 < 1.0))
    throw std::invalid_argument{"VerticalSync: vsync_level +/- hysteresis must stay within (0, 1)"};
  if (!(cfg_.min_field_fraction >= 0.0 && cfg_.min_field_fraction < 1.0))
    throw std::invalid_argument{"VerticalSync: min_field_fraction must be in [0, 1)"};
  if (!(cfg_.pll_kp >= 0.0 && cfg_.pll_kp <= 1.0))
    throw std::invalid_argument{"VerticalSync: pll_kp must be in [0, 1]"};
  if (!(cfg_.pll_ki >= 0.0))
    throw std::invalid_argument{"VerticalSync: pll_ki must be >= 0"};
  if (!(cfg_.omega_clamp > 0.0 && cfg_.omega_clamp < 1.0))
    throw std::invalid_argument{"VerticalSync: omega_clamp must be in (0, 1)"};

  // Integrator time constant expressed in lines: alpha = 1 / (tc * samples/line).
  const double line_samples = cfg_.sample_rate_hz / cfg_.nominal_line_hz;
  alpha_ = 1.0 / (cfg_.integrator_tc_lines * line_samples);
  omega_ = cfg_.nominal_field_hz / cfg_.sample_rate_hz;
}

void VerticalSync::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const VSample> VerticalSync::process(std::span<const SyncSample> in) {
  const std::size_t n = in.size();
  const auto dst = out_.write_n(n);

  const double nominal_omega = cfg_.nominal_field_hz / cfg_.sample_rate_hz;
  const double omega_lo = nominal_omega * (1.0 - cfg_.omega_clamp);
  const double omega_hi = nominal_omega * (1.0 + cfg_.omega_clamp);
  const double enter = cfg_.vsync_level + kVsyncHysteresis * 0.5;
  const double leave = cfg_.vsync_level - kVsyncHysteresis * 0.5;

  for (std::size_t k = 0; k < n; ++k) {
    // Low-pass the sync bit toward its duty cycle. Line sync settles it near
    // ~0.07; the broad-pulse train drives it toward ~0.84.
    integ_ += alpha_ * ((in[k].sync ? 1.0 : 0.0) - integ_);

    bool field_start = false;
    // Rising crossing of the slice marks the vertical interval. The hold gate
    // rejects a second crossing within the same field (the integrator can dip
    // and re-cross between broad pulses); the flywheel coasts otherwise.
    if (!in_vsync_ && integ_ >= enter) {
      in_vsync_ = true;
      const auto since = static_cast<double>(sample_index_ - last_field_sample_);
      const double min_gap = cfg_.min_field_fraction / omega_;
      if (!have_field_ || since >= min_gap) {
        // PI correction: v_phase should be 0 at the field-sync anchor.
        const double err = dsp::wrap_error(v_phase_);
        v_phase_ -= cfg_.pll_kp * err;
        omega_ = std::clamp(omega_ - cfg_.pll_ki * err, omega_lo, omega_hi);
        last_field_sample_ = sample_index_;
        have_field_ = true;
        ++fields_;
        field_start = true;
      }
    }
    else if (in_vsync_ && integ_ < leave) {
      in_vsync_ = false;
    }

    dst[k] = VSample{
        .v_phase = static_cast<float>(v_phase_ - std::floor(v_phase_)),
        .field_start = field_start,
    };

    v_phase_ += omega_;
    v_phase_ -= std::floor(v_phase_);
    ++sample_index_;
  }

  return out_.view();
}

} // namespace palindrome::video
