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

// The screen carries the caller's config plus the genuinely derived fields:
// the shared rate, the colour/level switches (stamped so they can't be
// half-mixed with the decode side's), and the picture registration - in colour
// the picture rail lags the timing rails by the chroma group delay; mono is
// the raw envelope (no lag).
[[nodiscard]] ScreenConfig screen_config(const DecoderConfig &cfg, const ChromaDecoder &chroma) {
  auto sc = cfg.screen;
  sc.sample_rate_hz = cfg.sample_rate_hz;
  sc.colour = cfg.colour;
  sc.tracked_white = cfg.agc_mode == AgcMode::adaptive;
  sc.picture_lag_samples = cfg.colour ? static_cast<double>(chroma.group_delay_samples()) : 0.0;
  return sc;
}
} // namespace

Decoder::Decoder(const DecoderConfig &cfg) :
    colour_{cfg.colour}, agc_{make_agc(cfg)},
    sync_lp_{dsp::lowpass_kernel(kSyncLpTaps, cfg.sample_rate_hz, cfg.sync_lp_cutoff_hz)},
    sep_{with_mode(with_rate(cfg.sep, cfg.sample_rate_hz), cfg.agc_mode)},
    hsweep_{with_rate(cfg.hsweep, cfg.sample_rate_hz)}, vsync_{with_rate(cfg.vsync, cfg.sample_rate_hz)},
    chroma_{with_rate(cfg.chroma, cfg.sample_rate_hz)}, screen_{screen_config(cfg, chroma_)} {}

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

void Decoder::decode_into(DecodedBlock &out, std::span<const float> envelope) {
  // The IF AGC normalises the carrier first - every branch below (sync slice,
  // picture rail, chroma) sees the same absolute levels, exactly as the gain
  // stage sits ahead of the detector fan-out in a real IF strip. Absent in
  // adaptive mode, where the per-stage trackers do their own levelling.
  if (agc_)
    envelope = agc_->process(envelope);
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
