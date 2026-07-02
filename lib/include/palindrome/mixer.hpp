#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/phasor.hpp"
#include "palindrome/restrict_ptr.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <span>

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
  // A matched pair of baseband planes. The spans are owned by the mixer and
  // valid only until the next process() call.
  struct Iq {
    std::span<const float> i;
    std::span<const float> q;
  };

  // Mix carrier_hz down to DC for input sampled at sample_rate_hz.
  Mixer(double carrier_hz, double sample_rate_hz);

  // Budget internal storage for blocks of up to `max_in` samples. Required
  // before process(): the hot path does not grow, so a block bigger than the
  // budget throws (std::length_error) rather than reallocating.
  void prepare(std::size_t max_in);

  // Down-convert `in`, returning one (I, Q) pair per input sample.
  [[nodiscard]] Iq process(std::span<const float> in);

  [[nodiscard]] std::size_t max_output_for(std::size_t n_in) const noexcept { return n_in; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

private:
  // One phase group's width. 8 matches an AVX2 float vector; on other targets it
  // is just the unroll factor and stays correct.
  static constexpr std::size_t kLanes = 8;

  // Mix one group of kLanes samples from a fixed base phasor (br, bi). The
  // restrict_ptr params declare the planes don't alias (no `this`, no aliasing),
  // which is what lets it lower to a single wide vector step; nesting the same
  // loop inside process() does not.
  static void mix_group(restrict_ptr<const float> x, restrict_ptr<float> i, restrict_ptr<float> q,
      restrict_ptr<const float> rot_re, restrict_ptr<const float> rot_im, float br, float bi);

  std::array<float, kLanes> rot_re_{}; //  cos(omega*l) = Re(step^l), l in [0, kLanes)
  std::array<float, kLanes> rot_im_{}; // -sin(omega*l) = Im(step^l)
  std::complex<double> step_; // e^{-i*omega}; advances `base` one sample at a time
  std::complex<double> step_block_; // step^kLanes; advances `base` a whole group
  Phasor base_; // phase at the current group boundary (counted renorm inside)
  Buffer<float> i_out_; // owned I plane, reused across calls
  Buffer<float> q_out_; // owned Q plane, reused across calls
};

} // namespace palindrome::dsp
