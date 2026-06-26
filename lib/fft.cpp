#include "palindrome/fft.hpp"

#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <stdexcept>
#include <utility>

namespace palindrome::dsp {

void fft(std::span<std::complex<double>> x) {
  const std::size_t n = x.size();
  if (n == 0)
    return;
  if (!std::has_single_bit(n))
    throw std::invalid_argument{"fft: size must be a power of two"};

  // Decimation-in-time: reorder into bit-reversed index order, then combine.
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; (j & bit) != 0; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
      std::swap(x[i], x[j]);
  }

  // Butterflies over doubling stage lengths; the twiddle for each stage is
  // recurred (w *= wlen) rather than recomputed - a cold one-shot, so the small
  // accumulated phase drift across a stage is below anything the peak-pick sees.
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * std::numbers::pi / static_cast<double>(len);
    const std::complex<double> wlen{std::cos(ang), std::sin(ang)};
    for (std::size_t base = 0; base < n; base += len) {
      std::complex<double> w{1.0, 0.0};
      for (std::size_t k = 0; k < len / 2; ++k) {
        const auto u = x[base + k];
        const auto v = x[base + k + len / 2] * w;
        x[base + k] = u + v;
        x[base + k + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
}

} // namespace palindrome::dsp
