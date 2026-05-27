#include "palindrome/dc_blocker.hpp"

#include "palindrome/restrict.hpp"

#include <stdexcept>

namespace palindrome::dsp {

DcBlocker::DcBlocker(double pole) : pole_{pole} {
  if (pole <= 0.0 || pole >= 1.0)
    throw std::invalid_argument("DC blocker pole must be in (0, 1)");
}

void DcBlocker::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const float> DcBlocker::process(std::span<const float> in) {
  const std::size_t n = in.size();
  out_.reserve(n);
  const float *PAL_RESTRICT x = in.data();
  float *PAL_RESTRICT y = out_.write_n(n).data();
  // A one-pole IIR has a serial dependency that doesn't vectorise; the win over
  // push_back is the flat counted store into pre-reserved storage.
  for (std::size_t k = 0; k < n; ++k) {
    const double xk = x[k];
    const double yk = xk - prev_in_ + pole_ * prev_out_;
    prev_in_ = xk;
    prev_out_ = yk;
    y[k] = static_cast<float>(yk);
  }
  return out_.view();
}

void DcBlocker::reset() { prev_in_ = prev_out_ = 0.0; }

} // namespace palindrome::dsp
