#include "palindrome/dc_blocker.hpp"

#include <stdexcept>

namespace palindrome::dsp {

DcBlocker::DcBlocker(double pole) : pole_{pole} {
  if (pole <= 0.0 || pole >= 1.0)
    throw std::invalid_argument("DC blocker pole must be in (0, 1)");
}

void DcBlocker::process(std::span<const float> in, std::vector<float> &out) {
  out.reserve(out.size() + in.size());
  for (const float sample: in) {
    const double x = sample;
    const double y = x - prev_in_ + pole_ * prev_out_;
    prev_in_ = x;
    prev_out_ = y;
    out.push_back(static_cast<float>(y));
  }
}

void DcBlocker::reset() { prev_in_ = prev_out_ = 0.0; }

} // namespace palindrome::dsp
