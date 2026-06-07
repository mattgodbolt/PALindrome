#include "palindrome/dc_blocker.hpp"

#include <stdexcept>

namespace palindrome::dsp {

DcBlocker::DcBlocker(double pole) : pole_{pole} {
  if (pole <= 0.0 || pole >= 1.0)
    throw std::invalid_argument("DC blocker pole must be in (0, 1)");
}

void DcBlocker::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const float> DcBlocker::process(std::span<const float> in) {
  const std::size_t n = in.size();
  const auto *x = in.data();
  auto *y = out_.write_n(n).data();
  // This stays a plain scalar recurrence on purpose. The loop carries prev_out_
  // through a single fused multiply-add (pole_*prev_out_ + ...), so it is bound
  // by that FMA's ~4-cycle latency, and the loop-carried dependency stops the
  // auto-vectoriser cold (the codegen is all *sd/*ss scalar). A manual blocked
  // SIMD form is possible -- evaluate the closed form y[n] = pole^k*y[n-k] +
  // Sum pole^j*d[n-j] over a width-k group to break the chain -- and it does
  // run ~1.4x faster, but it regroups the arithmetic and so rounds differently
  // from the per-sample FMA. That makes the float result depend on where the
  // group boundaries fall, which depends on the block size: it breaks the
  // bit-exact block-invariance the pipeline relies on (DcBlockerTest's
  // "streams identically regardless of block size"). So we keep the scalar
  // recurrence, which is identical no matter how the input is chunked.
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
