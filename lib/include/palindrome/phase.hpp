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

// Wrap a phase in radians into [-π, π).
[[nodiscard]] inline double wrap_angle(double a) noexcept { return std::remainder(a, 2.0 * std::numbers::pi); }

} // namespace palindrome::dsp
