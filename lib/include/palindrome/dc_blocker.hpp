#pragma once

#include "palindrome/buffer.hpp"

#include <cstddef>
#include <span>

// A one-pole DC blocker (high-pass). Like the other DSP stages, process()
// carries its state across calls, so the output is independent of how the input
// is chunked.
namespace palindrome::dsp {

// y[n] = x[n] - x[n-1] + R*y[n-1]: removes a constant offset while leaving
// everything well above the corner (~ (1-R)*fs/(2*pi)) essentially untouched.
// A DC bias matters here because, once mixed down, it lands at the carrier
// frequency inside the baseband passband and beats into the envelope.
class DcBlocker {
public:
  // pole R is the feedback coefficient, in (0, 1); closer to 1 lowers the
  // corner. Throws std::invalid_argument if R is outside (0, 1).
  explicit DcBlocker(double pole = 0.9999);

  // Budget internal storage for blocks of up to `max_in` samples. Required before
  // process(): the hot path does not grow, so a bigger block throws.
  void prepare(std::size_t max_in);

  // Filter `in`, returning a view of one output sample per input sample. The
  // returned span is owned by the filter and valid only until the next call.
  [[nodiscard]] std::span<const float> process(std::span<const float> in);

  [[nodiscard]] std::size_t max_output_for(std::size_t n_in) const noexcept { return n_in; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

  void reset();

private:
  double pole_;
  double prev_in_{};
  double prev_out_{};
  Buffer<float> out_; // owned output, reused across calls
};

} // namespace palindrome::dsp
