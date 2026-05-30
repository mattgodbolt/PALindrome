#include "palindrome/video.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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
constexpr double kVsyncHysteresis = 0.05; // integrator slice hysteresis for vertical sync
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
  const auto dst = out_.write_n(n);

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

// === VerticalSync ===

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
  out_.reserve(n);
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
        const double err = wrap_error(v_phase_);
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

// === Screen ===

Screen::Screen(const ScreenConfig &cfg) :
    cfg_{cfg}, bright_(cfg.width * cfg.height, 0.0f), last_(cfg.width * cfg.height, 0) {
  if (cfg_.width == 0 || cfg_.height == 0)
    throw std::invalid_argument{"Screen: width and height must be positive"};
  if (!(cfg_.sample_rate_hz > 0.0 && cfg_.field_hz > 0.0))
    throw std::invalid_argument{"Screen: sample_rate_hz and field_hz must be positive"};
  if (!(cfg_.persistence_fields > 0.0))
    throw std::invalid_argument{"Screen: persistence_fields must be positive"};
  if (!(cfg_.beam_sigma_rows >= 0.0))
    throw std::invalid_argument{"Screen: beam_sigma_rows must be non-negative"};
  if (!(cfg_.nominal_line_hz > cfg_.field_hz))
    throw std::invalid_argument{"Screen: nominal_line_hz must exceed field_hz"};
  // Decay per sample so that brightness falls by 1/e over persistence_fields
  // field periods: tau_samples = persistence * (sample_rate / field_hz).
  const double tau_samples = cfg_.persistence_fields * cfg_.sample_rate_hz / cfg_.field_hz;
  log_decay_ = -1.0 / tau_samples;
  // Yoke shear: the beam steps (field_hz / line_hz) of a field per line, i.e.
  // this many output rows — the amount to un-creep within each line.
  yoke_tilt_rows_ = static_cast<double>(cfg_.height) * cfg_.field_hz / cfg_.nominal_line_hz;
  // Splat the beam spot out to 2.5 sigma; weights themselves are recomputed per
  // line, since the spot centre sits at a sub-pixel row that shifts line to line.
  splat_radius_ = cfg_.beam_sigma_rows > 0.0 ? static_cast<std::size_t>(std::ceil(2.5 * cfg_.beam_sigma_rows)) : 0;
  row_weights_.assign(2 * splat_radius_ + 1, 0.0);
}

void Screen::prepare(std::size_t) {}

namespace {
// The back porch — the blanking shelf just after the line-sync pulse (h_phase=0
// is the sync leading edge) — sits at black every line, a clean reference for
// DC restoration. As an h_phase window: sync is ~0..0.07, the back porch ~0.09.
constexpr float kBackPorchLo = 0.09f;
constexpr float kBackPorchHi = 0.14f;
constexpr double kBlackTrack = 0.02; // black-reference tracking per back-porch sample
} // namespace

float Screen::intensity_of(float env_f, float h_phase) {
  // The electron gun (see Screen::black_): DC-restore black from the back-porch
  // window, then drive = black - env, cut off below black. No upper bound.
  const auto env = static_cast<double>(env_f);
  if (!seeded_) {
    black_ = env;
    seeded_ = true;
  }
  if (h_phase >= kBackPorchLo && h_phase < kBackPorchHi)
    black_ += kBlackTrack * (env - black_);
  const double drive = black_ - env;
  if (drive <= 0.0)
    return 0.0f;
  // Gun gamma: beam current ~ drive^gamma. Peak-normalised at readout, so the
  // absolute drive scale washes out and the curve shape is all that matters.
  return static_cast<float>(cfg_.gamma == 1.0 ? drive : std::pow(drive, cfg_.gamma));
}

void Screen::process(std::span<const float> envelope, std::span<const BeamSample> hbeam, std::span<const VSample> vbeam,
    const FieldCallback &on_field) {
  const std::size_t n = std::min({envelope.size(), hbeam.size(), vbeam.size()});
  for (std::size_t i = 0; i < n; ++i) {
    // A field boundary closes off a completed field: snapshot the phosphor as
    // it stands now (before this sample's deposit), then carry on. Block-size
    // independent — field_start is per-sample, so the snapshots land identically
    // however the input is chunked.
    if (on_field && vbeam[i].field_start)
      on_field(snapshot());

    const float drive = intensity_of(envelope[i], hbeam[i].h_phase);

    auto x = static_cast<std::size_t>(static_cast<double>(hbeam[i].h_phase) * static_cast<double>(cfg_.width));
    if (x >= cfg_.width)
      x = cfg_.width - 1;

    // Yoke shear: un-creep the vertical so the scanline is flat (see header).
    const auto yc = static_cast<double>(vbeam[i].v_phase) * static_cast<double>(cfg_.height) -
                    yoke_tilt_rows_ * static_cast<double>(hbeam[i].h_phase);

    // Gaussian beam spot, centred on the sheared sub-pixel row and normalised so
    // the splat conserves the deposited charge.
    const auto base = static_cast<std::ptrdiff_t>(std::floor(yc)) - static_cast<std::ptrdiff_t>(splat_radius_);
    double sum = 0.0;
    for (std::size_t k = 0; k < row_weights_.size(); ++k) {
      const auto d = static_cast<double>(base + static_cast<std::ptrdiff_t>(k)) + 0.5 - yc;
      const double w =
          cfg_.beam_sigma_rows > 0.0 ? std::exp(-0.5 * d * d / (cfg_.beam_sigma_rows * cfg_.beam_sigma_rows)) : 1.0;
      row_weights_[k] = w;
      sum += w;
    }
    const double inv = sum > 0.0 ? 1.0 / sum : 0.0;

    // Splat into those rows; each pixel keeps its own lazy phosphor decay (charge
    // brought forward to now, then the fresh deposit).
    for (std::size_t k = 0; k < row_weights_.size(); ++k) {
      const std::ptrdiff_t row = base + static_cast<std::ptrdiff_t>(k);
      if (row < 0 || row >= static_cast<std::ptrdiff_t>(cfg_.height))
        continue;
      const auto idx = static_cast<std::size_t>(row) * cfg_.width + x;
      const auto dt = static_cast<double>(sample_index_ - last_[idx]);
      bright_[idx] = bright_[idx] * static_cast<float>(std::exp(log_decay_ * dt)) +
                     static_cast<float>(drive * row_weights_[k] * inv);
      last_[idx] = sample_index_;
    }
    ++sample_index_;
  }
}

Screen::Frame Screen::snapshot() const {
  // Decay every pixel to the current instant, find the peak, normalise to grey.
  std::vector<float> now(bright_.size());
  float peak = 0.0f;
  for (std::size_t i = 0; i < bright_.size(); ++i) {
    const auto dt = static_cast<double>(sample_index_ - last_[i]);
    now[i] = bright_[i] * static_cast<float>(std::exp(log_decay_ * dt));
    peak = std::max(peak, now[i]);
  }
  const float scale = peak > 0.0f ? 255.0f / peak : 0.0f;
  std::vector<std::uint8_t> grey(bright_.size());
  for (std::size_t i = 0; i < grey.size(); ++i)
    grey[i] = static_cast<std::uint8_t>(std::clamp(now[i] * scale, 0.0f, 255.0f) + 0.5f);
  return Frame{.grey = std::move(grey), .width = cfg_.width, .height = cfg_.height};
}

// === Decoder ===

namespace {
// Sync-branch low-pass length. Modest, so the group delay (~half this) stays a
// small fraction of a line — a constant horizontal offset, not a smear.
constexpr std::size_t kSyncLpTaps = 63;

// Copy a sub-stage config and stamp the shared sample rate into it; lets
// DecoderConfig carry the sub-configs with their sample_rate_hz left at 0.
template<class C>
[[nodiscard]] C with_rate(C c, double rate) {
  c.sample_rate_hz = rate;
  return c;
}
} // namespace

Decoder::Decoder(const DecoderConfig &cfg) :
    sync_lp_{dsp::lowpass_kernel(kSyncLpTaps, cfg.sample_rate_hz, cfg.sync_lp_cutoff_hz)},
    sep_{with_rate(cfg.sep, cfg.sample_rate_hz)}, hsweep_{with_rate(cfg.hsweep, cfg.sample_rate_hz)},
    vsync_{with_rate(cfg.vsync, cfg.sample_rate_hz)}, screen_{ScreenConfig{.width = cfg.width,
                                                          .height = cfg.height,
                                                          .sample_rate_hz = cfg.sample_rate_hz,
                                                          .persistence_fields = cfg.persistence_fields,
                                                          .beam_sigma_rows = cfg.beam_sigma_rows,
                                                          .gamma = cfg.gamma}} {}

void Decoder::prepare(std::size_t max_in) {
  sync_lp_.prepare(max_in);
  sep_.prepare(max_in);
  hsweep_.prepare(max_in);
  vsync_.prepare(max_in);
  screen_.prepare(max_in);
}

void Decoder::process(std::span<const float> envelope, const Screen::FieldCallback &on_field) {
  // The branch: the envelope fans to a narrow sync low-pass (whose sliced sync
  // bit feeds both timebases) and, untouched, to the picture rail. The screen
  // joins the picture rail with the two timing rails. Spans stay valid because
  // each producer is read before it runs again next block.
  const auto sync_env = sync_lp_.process(envelope);
  const auto sync = sep_.process(sync_env);
  const auto hbeam = hsweep_.process(sync);
  const auto vbeam = vsync_.process(sync);
  screen_.process(envelope, hbeam, vbeam, on_field);
}

} // namespace palindrome::video
