#pragma once

#include <span>
#include <vector>

// Second-order IIR (biquad) filtering: a streaming, stateful section plus
// factory helpers for the standard responses. State carries across process()
// calls, so output is independent of block size.
namespace palindrome::dsp {

class Biquad {
public:
  // Coefficients in the usual normalised form (a0 already divided out):
  //   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
  Biquad(double b0, double b1, double b2, double a1, double a2);

  // Filter `in`, appending one output sample per input sample to `out`.
  void process(std::span<const float> in, std::vector<float> &out);

  void reset();

private:
  double b0_, b1_, b2_, a1_, a2_;
  double z1_{}, z2_{}; // transposed direct-form II state
};

// A second-order notch (band-reject) at center_hz with quality factor q; higher
// q is a narrower notch. Throws std::invalid_argument on bad parameters
// (center not in (0, sample_rate_hz / 2), or q <= 0).
[[nodiscard]] Biquad notch(double sample_rate_hz, double center_hz, double q);

} // namespace palindrome::dsp
