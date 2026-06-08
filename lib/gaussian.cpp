#include "palindrome/gaussian.hpp"

#include <cmath>

namespace palindrome::dsp {

std::size_t splat_radius_for(double sigma) {
  return sigma > 0.0 ? static_cast<std::size_t>(std::ceil(2.5 * sigma)) : 0;
}

std::vector<float> gaussian_splat_lut(double sigma, std::size_t radius, std::size_t bins) {
  const std::size_t stride = 2 * radius + 1;
  const double sigma2 = sigma * sigma;
  std::vector<float> lut(bins * stride, 0.0f);
  std::vector<double> kernel(stride); // scratch, normalised before narrowing to float
  for (std::size_t bin = 0; bin < bins; ++bin) {
    const double frac = (static_cast<double>(bin) + 0.5) / static_cast<double>(bins);
    double sum = 0.0;
    for (std::size_t k = 0; k < stride; ++k) {
      const double d = static_cast<double>(k) - static_cast<double>(radius) + 0.5 - frac;
      kernel[k] = sigma2 > 0.0 ? std::exp(-0.5 * d * d / sigma2) : 1.0;
      sum += kernel[k];
    }
    const double inv = sum > 0.0 ? 1.0 / sum : 0.0;
    float *w = &lut[bin * stride];
    for (std::size_t k = 0; k < stride; ++k)
      w[k] = static_cast<float>(kernel[k] * inv);
  }
  return lut;
}

} // namespace palindrome::dsp
