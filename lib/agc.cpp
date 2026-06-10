#include "palindrome/agc.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>

namespace palindrome::video {

Agc::Agc(const AgcConfig &cfg) {
  if (!(cfg.sample_rate_hz > 0.0))
    throw std::invalid_argument{"Agc: sample_rate_hz must be positive"};
  if (!(cfg.nominal_field_hz > 0.0))
    throw std::invalid_argument{"Agc: nominal_field_hz must be positive"};
  if (!(cfg.decay_fields > 0.0))
    throw std::invalid_argument{"Agc: decay_fields must be positive"};
  const double samples_per_field = cfg.sample_rate_hz / cfg.nominal_field_hz;
  release_ = std::exp(-1.0 / (cfg.decay_fields * samples_per_field));
}

void Agc::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const float> Agc::process(std::span<const float> envelope) {
  const std::size_t n = envelope.size();
  const auto dst = out_.write_n(n);

  // Seed from the first sample so the gain is meaningful immediately; the
  // instant attack corrects it the moment the first real sync tip arrives
  // (within a line), so cold start settles in well under a field.
  if (!seeded_ && n > 0) {
    tip_ = static_cast<double>(envelope[0]);
    seeded_ = true;
  }

  for (std::size_t k = 0; k < n; ++k) {
    const auto env = static_cast<double>(envelope[k]);
    tip_ = std::max(env, tip_ * release_);
    // Snapshot the control down to float at the point of use; the tip itself
    // keeps accumulating in double.
    const auto scale = static_cast<float>(tip_ > 0.0 ? 1.0 / tip_ : 0.0);
    dst[k] = envelope[k] * scale;
  }

  return out_.view();
}

} // namespace palindrome::video
