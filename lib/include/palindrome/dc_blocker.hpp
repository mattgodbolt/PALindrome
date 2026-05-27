#pragma once

#include <span>
#include <vector>

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

  // Filter `in`, appending one output sample per input sample to `out`.
  void process(std::span<const float> in, std::vector<float> &out);

  void reset();

private:
  double pole_;
  double prev_in_{};
  double prev_out_{};
};

} // namespace palindrome::dsp
