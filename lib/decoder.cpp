#include "palindrome/decoder.hpp"

#include "palindrome/fir.hpp"

#include <algorithm>
#include <cstddef>
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
} // namespace

Decoder::Decoder(const DecoderConfig &cfg) :
    colour_{cfg.colour}, sync_lp_{dsp::lowpass_kernel(kSyncLpTaps, cfg.sample_rate_hz, cfg.sync_lp_cutoff_hz)},
    sep_{with_rate(cfg.sep, cfg.sample_rate_hz)}, hsweep_{with_rate(cfg.hsweep, cfg.sample_rate_hz)},
    vsync_{with_rate(cfg.vsync, cfg.sample_rate_hz)}, chroma_{with_rate(cfg.chroma, cfg.sample_rate_hz)},
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
        .readout_gamma = cfg.readout_gamma,
        .h_blank = cfg.h_blank,
        // Register the picture: in colour it lags the timing rails by the chroma
        // group delay. Mono is the raw envelope (no lag).
        .picture_lag_samples = cfg.colour ? static_cast<double>(chroma_.group_delay_samples()) : 0.0,
        .h_window_lo = cfg.h_window_lo,
        .h_window_hi = cfg.h_window_hi,
        .v_window_lo = cfg.v_window_lo,
        .v_window_hi = cfg.v_window_hi}} {}

void Decoder::prepare(std::size_t max_in) {
  sync_lp_.prepare(max_in);
  sep_.prepare(max_in);
  hsweep_.prepare(max_in);
  vsync_.prepare(max_in);
  chroma_.prepare(max_in);
  screen_.prepare(max_in);
}

void Decoder::decode_into(DecodedBlock &out, std::span<const float> envelope) {
  // The branch: the envelope fans to a narrow sync low-pass (whose sliced sync
  // bit feeds both timebases) and, untouched, to the picture rail. In colour the
  // chroma decoder is a third branch off the envelope (it needs the horizontal
  // rail for the burst gate). The rails are copied into owned vectors so the
  // screen deposit can run on a different thread a block behind.
  const auto sync_env = sync_lp_.process(envelope);
  const auto sync = sep_.process(sync_env);
  const auto hbeam = hsweep_.process(sync);
  const auto vbeam = vsync_.process(sync);
  const auto n = envelope.size();
  out.resize(n);
  std::ranges::copy(hbeam, out.hbeam.begin());
  std::ranges::copy(vbeam, out.vbeam.begin());
  if (colour_) {
    std::ranges::copy(chroma_.process(envelope, hbeam), out.picture.begin());
  }
  else {
    // Monochrome: the luma rail is the envelope straight through, no chroma.
    std::ranges::transform(
        envelope, out.picture.begin(), [](float luma) { return ChromaSample{.luma = luma, .u = 0.0f, .v = 0.0f}; });
  }
}

void Decoder::deposit(const DecodedBlock &block, const Screen::FieldCallback &on_field) {
  // The screen joins the picture rail with the two timing rails; it owns the
  // phosphor framebuffer, so only ever one thread runs this.
  screen_.process(block.picture, block.hbeam, block.vbeam, on_field);
}

} // namespace palindrome::video
