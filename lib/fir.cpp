#include "palindrome/fir.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace palindrome::dsp {

namespace {
constexpr double pi = std::numbers::pi;

double window_value(Window window, double n, double m) {
  if (m == 0.0)
    return 1.0; // single tap: the window is irrelevant
  const double phase = 2.0 * pi * n / m;
  if (window == Window::Hamming)
    return 0.54 - 0.46 * std::cos(phase);
  return 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase); // Blackman
}
} // namespace

std::vector<float> lowpass_kernel(std::size_t num_taps, double sample_rate_hz, double cutoff_hz, Window window) {
  if (num_taps == 0)
    throw std::invalid_argument("num_taps must be non-zero");
  if (cutoff_hz <= 0.0 || cutoff_hz >= sample_rate_hz / 2.0)
    throw std::invalid_argument("cutoff must be in (0, sample_rate / 2)");

  const double fc = cutoff_hz / sample_rate_hz; // cycles per sample
  const double m = static_cast<double>(num_taps - 1);
  const double centre = m / 2.0;

  std::vector<float> taps(num_taps);
  double sum = 0.0;
  for (std::size_t i = 0; i < num_taps; ++i) {
    const double k = static_cast<double>(i) - centre;
    // Ideal low-pass impulse response 2*fc*sinc(2*fc*k), windowed.
    const double sinc = (k == 0.0) ? 2.0 * fc : std::sin(2.0 * pi * fc * k) / (pi * k);
    const double tap = sinc * window_value(window, static_cast<double>(i), m);
    taps[i] = static_cast<float>(tap);
    sum += tap;
  }
  for (float &tap: taps)
    tap = static_cast<float>(tap / sum); // normalise to unity DC gain
  return taps;
}

Fir::Fir(std::vector<float> taps) : taps_{std::move(taps)} {
  if (taps_.empty())
    throw std::invalid_argument("FIR needs at least one tap");
  history_.assign(taps_.size(), 0.0f);
}

void Fir::process(std::span<const float> in, std::vector<float> &out) {
  out.reserve(out.size() + in.size());
  const std::size_t n = taps_.size();
  for (const float sample: in) {
    history_[head_] = sample;
    // Convolve: taps_[0] weights the newest sample, walking back through history.
    double acc = 0.0;
    std::size_t idx = head_;
    for (std::size_t k = 0; k < n; ++k) {
      acc += static_cast<double>(taps_[k]) * static_cast<double>(history_[idx]);
      idx = (idx == 0) ? n - 1 : idx - 1;
    }
    out.push_back(static_cast<float>(acc));
    head_ = (head_ + 1 == n) ? 0 : head_ + 1;
  }
}

} // namespace palindrome::dsp
