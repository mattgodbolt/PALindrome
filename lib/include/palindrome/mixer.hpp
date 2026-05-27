#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <span>
#include <vector>

// Quadrature down-conversion. Like the other DSP stages, process() carries phase
// across calls, so the result composes into the streaming pipeline.
namespace palindrome::dsp {

// Multiplies a real input by e^{-i*omega*n} to shift a carrier down to DC,
// emitting in-phase (I) and quadrature (Q) baseband planes.
//
// The phase is generated a whole vector at a time rather than by a per-sample
// recurrence. Since phasor[n] = base * step^n, we hold a constant table of the
// first kLanes rotations and advance `base` by step^kLanes once per group. That
// breaks the serial dependency, so the elementwise mix vectorises with no
// fast-math relaxation; it also keeps the accumulating phase error on `base`,
// which now drifts kLanes times more slowly than a per-sample phasor would.
//
// Because a full group advances `base` by the precomputed step^kLanes while a
// sub-group tail advances it one step at a time, the two paths round slightly
// differently: process() is independent of block size only to within float
// rounding (~1e-7), not bit-for-bit like the FIR or DC blocker.
class Mixer {
public:
  // Mix carrier_hz down to DC for input sampled at sample_rate_hz.
  Mixer(double carrier_hz, double sample_rate_hz);

  // Down-convert `in`, appending one (I, Q) pair per input sample to `i`/`q`.
  void process(std::span<const float> in, std::vector<float> &i, std::vector<float> &q);

private:
  // One phase group's width. 8 matches an AVX2 float vector; on other targets it
  // is just the unroll factor and stays correct.
  static constexpr std::size_t kLanes = 8;

  // Mix one group of kLanes samples from a fixed base phasor (br, bi). Taking
  // raw restrict pointers (no `this`, no aliasing) is what lets it lower to a
  // single wide vector step; nesting the same loop inside process() does not.
  static void mix_group(
      const float *x, float *i, float *q, const float *rot_re, const float *rot_im, float br, float bi);

  std::array<float, kLanes> rot_re_{}; //  cos(omega*l) = Re(step^l), l in [0, kLanes)
  std::array<float, kLanes> rot_im_{}; // -sin(omega*l) = Im(step^l)
  std::complex<double> step_; // e^{-i*omega}; advances `base` one sample at a time
  std::complex<double> step_block_; // step^kLanes; advances `base` a whole group
  std::complex<double> base_{1.0, 0.0}; // phase at the current group boundary
  unsigned groups_since_renorm_{}; // groups since `base` was last renormalised
};

} // namespace palindrome::dsp
