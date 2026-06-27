#include "palindrome/decoder.hpp"

#include "palindrome/fir.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>

namespace palindrome::video {

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

// One switch picks the level scheme everywhere: the separator's slice mode
// must match whether the AGC is in the chain (a fixed slice needs the tip held
// at 1.0), so it's stamped here rather than left as an independent knob.
[[nodiscard]] SyncSeparatorConfig with_mode(SyncSeparatorConfig c, AgcMode mode) {
  c.adaptive = mode == AgcMode::adaptive;
  return c;
}

// The AGC stage exists only in sync-tip mode; adaptive levels bypass it.
[[nodiscard]] std::optional<Agc> make_agc(const DecoderConfig &cfg) {
  if (cfg.agc_mode != AgcMode::sync_tip)
    return std::nullopt;
  return Agc{with_rate(cfg.agc, cfg.sample_rate_hz)};
}
} // namespace

Decoder::Decoder(const DecoderConfig &cfg) :
    colour_{cfg.colour}, agc_{make_agc(cfg)},
    sync_lp_{dsp::lowpass_kernel(kSyncLpTaps, cfg.sample_rate_hz, cfg.sync_lp_cutoff_hz)},
    sep_{with_mode(with_rate(cfg.sep, cfg.sample_rate_hz), cfg.agc_mode)},
    hsweep_{with_rate(cfg.hsweep, cfg.sample_rate_hz)}, vsync_{with_rate(cfg.vsync, cfg.sample_rate_hz)},
    chroma_{with_rate(cfg.chroma, cfg.sample_rate_hz)},
    screen_{ScreenConfig{.width = cfg.width,
        .height = cfg.height,
        .sample_rate_hz = cfg.sample_rate_hz,
        .persistence_fields = cfg.persistence_fields,
        .beam_sigma = cfg.beam_sigma,
        .beam_sigma_cols = cfg.beam_sigma_cols,
        .gamma = cfg.gamma,
        .colour = cfg.colour,
        .saturation = cfg.saturation,
        .contrast = cfg.contrast,
        .tracked_white = cfg.agc_mode == AgcMode::adaptive,
        .pwl_threshold = cfg.pwl_threshold,
        .readout_gamma = cfg.readout_gamma,
        .h_blank = cfg.h_blank,
        // Register the picture: in colour it lags the timing rails by the chroma
        // group delay. Mono is the raw envelope (no lag).
        .picture_lag_samples = cfg.colour ? static_cast<double>(chroma_.group_delay_samples()) : 0.0,
        .h_window_lo = cfg.h_window_lo,
        .h_window_hi = cfg.h_window_hi,
        .v_window_lo = cfg.v_window_lo,
        .v_window_hi = cfg.v_window_hi,
        .eht_sag = cfg.eht_sag,
        .eht_tc_fields = cfg.eht_tc_fields,
        .eht_focus = cfg.eht_focus,
        .line_pull = cfg.line_pull,
        .bcl_threshold = cfg.bcl_threshold,
        .bcl_tc_fields = cfg.bcl_tc_fields,
        .deposit_lanes = cfg.deposit_lanes}} {}

void Decoder::prepare(std::size_t max_in) {
  if (agc_)
    agc_->prepare(max_in);
  sync_lp_.prepare(max_in);
  sep_.prepare(max_in);
  hsweep_.prepare(max_in);
  vsync_.prepare(max_in);
  chroma_.prepare(max_in);
  screen_.prepare(max_in);
}

void Decoder::decode_sync(SyncedBlock &out, std::span<const float> envelope) {
  // The IF AGC normalises the carrier first - every branch downstream (sync
  // slice, picture rail, chroma) sees the same absolute levels, exactly as the
  // gain stage sits ahead of the detector fan-out in a real IF strip. Absent in
  // adaptive mode, where the per-stage trackers do their own levelling.
  if (agc_)
    envelope = agc_->process(envelope);
  // The sync fan-out: the envelope feeds a narrow sync low-pass whose sliced sync
  // bit drives both timebases. Carry the AGC'd envelope forward too - the chroma
  // stage needs it (and the horizontal rail) a block behind, on its own thread.
  const auto sync_env = sync_lp_.process(envelope);
  const auto sync = sep_.process(sync_env);
  const auto hbeam = hsweep_.process(sync);
  const auto vbeam = vsync_.process(sync);
  out.resize(envelope.size());
  std::ranges::copy(envelope, out.envelope.begin());
  std::ranges::copy(hbeam, out.hbeam.begin());
  std::ranges::copy(vbeam, out.vbeam.begin());
}

void Decoder::decode_chroma(DecodedBlock &out, const SyncedBlock &synced) {
  out.resize(synced.size());
  std::ranges::copy(synced.hbeam, out.hbeam.begin());
  std::ranges::copy(synced.vbeam, out.vbeam.begin());
  if (colour_) {
    // The chroma decoder is a branch off the envelope (it needs the horizontal
    // rail for the burst gate). The FIR-heavy work that this split moves off the
    // sync thread onto its own.
    std::ranges::copy(chroma_.process(synced.envelope, synced.hbeam), out.picture.begin());
  }
  else {
    // Monochrome: the luma rail is the envelope straight through, no chroma.
    std::ranges::transform(synced.envelope, out.picture.begin(),
        [](float luma) { return ChromaSample{.luma = luma, .u = 0.0f, .v = 0.0f}; });
  }
}

void Decoder::deposit(const DecodedBlock &block, const Screen::FieldCallback &on_field) {
  // The screen joins the picture rail with the two timing rails; it owns the
  // phosphor framebuffer, so only ever one thread runs this.
  screen_.process(block.picture, block.hbeam, block.vbeam, on_field);
}

} // namespace palindrome::video
