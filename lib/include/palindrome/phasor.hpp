#pragma once

#include "palindrome/cmul.hpp"

#include <complex>

// The NCO / mix-phasor recurrence shared by the DSP stages: a unit-modulus
// complex<double> advanced by cmul per sample (or per lane group), with a
// counted renormalisation shedding the multiply's accumulated magnitude drift.
// The phase is a cross-sample accumulator, so it lives in double; consumers
// snapshot it down to float at the point of use (the precision rule in
// CLAUDE.md) - that's what the _f accessors are for.
namespace palindrome::dsp {

class Phasor {
public:
  // Renormalise every this many advances: often enough that |z| stays within
  // ~5e-6 of 1 in lock, rare enough to cost nothing per sample.
  static constexpr unsigned kRenormInterval = 1024;

  constexpr Phasor() = default;
  explicit constexpr Phasor(std::complex<double> z) : z_{z} {}

  [[nodiscard]] constexpr std::complex<double> value() const noexcept { return z_; }
  [[nodiscard]] float real_f() const noexcept { return static_cast<float>(z_.real()); }
  [[nodiscard]] float imag_f() const noexcept { return static_cast<float>(z_.imag()); }

  void advance(std::complex<double> step) noexcept {
    z_ = cmul(z_, step);
    tick();
  }

  // Advance, then trim by the small angle `a` as the first-order rotation
  // (1 - j*a) - the locked-loop NCO idiom: exact enough for a tiny correction,
  // with the renorm absorbing its second-order magnitude drift. Returns true
  // when this step renormalised: the chunk-invariant instants at which an
  // owner tracking a large standing trim may retune the renorm cadence (see
  // set_renorm_interval).
  bool advance_trimmed(std::complex<double> step, double a) noexcept {
    z_ = cmul(z_, step);
    z_ = {z_.real() + a * z_.imag(), z_.imag() - a * z_.real()};
    return tick();
  }

  // The trim's magnitude error grows with its angle (|1 - j*a| = sqrt(1+a^2)),
  // so a loop holding a large standing correction must renormalise more often
  // than the default or |z| sawtooths visibly between renorms. Counted per
  // sample from state carried across calls, so the cadence is independent of
  // input chunking.
  void set_renorm_interval(unsigned n) noexcept { renorm_interval_ = n; }

private:
  bool tick() noexcept {
    if (++since_renorm_ >= renorm_interval_) [[unlikely]] {
      z_ /= std::abs(z_);
      since_renorm_ = 0;
      return true;
    }
    return false;
  }

  std::complex<double> z_{1.0, 0.0};
  unsigned since_renorm_ = 0;
  unsigned renorm_interval_ = kRenormInterval;
};

} // namespace palindrome::dsp
