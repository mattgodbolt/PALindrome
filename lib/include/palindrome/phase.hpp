#pragma once

#include <cmath>
#include <numbers>

// Phase-detector wrapping helpers, shared by the timing loops (horizontal /
// vertical sweep PLLs) and the chroma APC. Pure functions — the wrap is the one
// fiddly, boundary-sensitive bit of a phase loop, so it lives here and is tested.
namespace palindrome::dsp {

// Wrap a phase error in cycles into [-0.5, 0.5). Past that range a PLL loop would
// push the wrong way around the cycle; this is the standard phase-detector range.
[[nodiscard]] inline double wrap_error(double e) noexcept {
  e -= std::floor(e);
  return e < 0.5 ? e : e - 1.0;
}

// Wrap a phase in cycles into [0, 1], given it is already within one cycle of
// that range - (-1, 2). Over that range it is bit-identical to x - std::floor(x)
// (the floor only ever picks which of -1/0/+1 to subtract, and the subtraction
// is the same IEEE operation either way) but keeps the round-to-integer off the
// oscillators' loop-carried chain: the compares predict perfectly, a wrap firing
// once per line or once per field. The closed top end is real: x in (-2^-54, 0)
// rounds to exactly 1.0 through x + 1.0, as it did through the floor form, and
// the consumers key on the phase dropping, not on it being zero. The one
// difference from the floor form is the sign of zero (-0.0 passes through where
// floor gave +0.0); no accumulator here can produce a -0.0.
[[nodiscard]] inline double wrap_phase(double x) noexcept {
  if (x >= 1.0)
    return x - 1.0;
  if (x < 0.0)
    return x + 1.0;
  return x;
}

// Wrap a phase in radians into [-π, π).
[[nodiscard]] inline double wrap_angle(double a) noexcept { return std::remainder(a, 2.0 * std::numbers::pi); }

} // namespace palindrome::dsp
