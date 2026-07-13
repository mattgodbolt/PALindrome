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

private:
  void tick() noexcept {
    if (++since_renorm_ >= kRenormInterval) [[unlikely]] {
      z_ /= std::abs(z_);
      since_renorm_ = 0;
    }
  }

  std::complex<double> z_{1.0, 0.0};
  unsigned since_renorm_ = 0;
};

} // namespace palindrome::dsp
