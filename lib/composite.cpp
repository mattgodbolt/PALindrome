#include "palindrome/composite.hpp"

#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>

namespace palindrome::video {

CompositeInput::CompositeInput(const CompositeInputConfig &cfg) {
  if (!(cfg.sample_rate_hz > 0.0))
    throw std::invalid_argument{"CompositeInput: sample_rate_hz must be positive"};
  if (!(cfg.nominal_line_hz > 0.0))
    throw std::invalid_argument{"CompositeInput: nominal_line_hz must be positive"};
  if (!(cfg.full_scale_volts > 0.0))
    throw std::invalid_argument{"CompositeInput: full_scale_volts must be positive"};
  if (!(cfg.sync_amplitude_v > 0.0))
    throw std::invalid_argument{"CompositeInput: sync_amplitude_v must be positive"};
  if (!(cfg.clamp_lines > 0.0))
    throw std::invalid_argument{"CompositeInput: clamp_lines must be positive"};

  // The source's sync amplitude spans tip to blanking, so it maps onto exactly
  // that much of the envelope. Everything else follows: the same gain carries
  // blanking to peak white across the standard's remaining span, because AM
  // under negative modulation is linear in video volts.
  const double envelope_per_volt = (kSyncTipLevel - kBlankingLevel) / cfg.sync_amplitude_v;
  scale_ = envelope_per_volt * cfg.full_scale_volts;

  const double samples_per_line = cfg.sample_rate_hz / cfg.nominal_line_hz;
  rise_ = 1.0 - std::exp(-1.0 / (cfg.clamp_lines * samples_per_line));
}

void CompositeInput::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const float> CompositeInput::process(std::span<const float> composite) {
  const std::size_t n = composite.size();
  const auto dst = out_.write_n(n);

  // Seed from the first sample so the output is meaningful immediately; the
  // instant attack re-anchors on the first real sync tip, within a line.
  if (!seeded_ && n > 0) {
    tip_ = static_cast<double>(composite[0]);
    seeded_ = true;
  }

  const auto tip_level = static_cast<float>(kSyncTipLevel);
  const auto scale = static_cast<float>(scale_);

  for (std::size_t k = 0; k < n; ++k) {
    const auto x = composite[k];
    const auto v = static_cast<double>(x);
    // Sync tip is the most negative excursion of baseband composite, so the
    // clamp tracks a minimum: instant down, one-pole release up.
    if (v < tip_)
      tip_ = v;
    else
      tip_ += rise_ * (v - tip_);
    // Snapshot the clamp down to float at the point of use; it keeps
    // accumulating in double.
    dst[k] = tip_level - scale * (x - static_cast<float>(tip_));
  }

  return out_.view();
}

} // namespace palindrome::video
