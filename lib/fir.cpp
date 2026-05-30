#include "palindrome/fir.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace palindrome::dsp {

namespace {
constexpr double pi = std::numbers::pi;

// One output sample: the dot product of the taps with `window`. Reassociation
// is enabled for just this function so the sum vectorises into 8-wide FMA lanes;
// no other floating-point relaxations apply. The accumulator is float, trading
// precision for throughput: error grows with tap count (~n*eps worst case), so
// this suits moderate-length, amplitude-limited filters rather than long or
// ill-conditioned ones that would want a double accumulator.
//
// TODO(std::simd): replace this [[gnu::optimize]] attribute (a GCC debug-only
// feature) with explicit std::simd lanes, which makes the reduction order
// explicit and needs no FP-relaxation flags. Validated working on GCC 16.1's
// <simd>; blocked only on getting that toolchain into the build (no gcc-16
// package for Ubuntu 25.10 yet, so a Compiler Explorer tarball or similar).
[[gnu::optimize("-fassociative-math", "-fno-signed-zeros", "-fno-trapping-math")]]
float convolve(const float *taps, const float *window, std::size_t n) {
  float acc = 0.0f;
  for (std::size_t k = 0; k < n; ++k)
    acc += taps[k] * window[k];
  return acc;
}

double window_value(Window window, double n, double m) {
  if (m == 0.0)
    return 1.0; // single tap: the window is irrelevant
  const double phase = 2.0 * pi * n / m;
  // No default: a new Window value then fails to compile (-Wswitch -Werror)
  // until it's handled here, rather than silently taking a wrong branch.
  switch (window) {
    case Window::Hamming: return 0.54 - 0.46 * std::cos(phase);
    case Window::Blackman: return 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
  }
  std::unreachable();
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

Fir::Fir(std::vector<float> taps, std::size_t decimation) : taps_{std::move(taps)}, decimation_{decimation} {
  if (taps_.empty())
    throw std::invalid_argument("FIR needs at least one tap");
  if (decimation_ == 0)
    throw std::invalid_argument("FIR decimation must be >= 1");
  // Reverse once so the per-output convolution is a forward walk over a
  // contiguous window (oldest sample first), rather than a modulo ring buffer.
  std::ranges::reverse(taps_);
  history_.assign(taps_.size() - 1, 0.0f);
}

void Fir::prepare(std::size_t max_in) {
  window_.reserve(history_.size() + max_in);
  out_.reserve(max_output_for(max_in));
}

std::span<const float> Fir::process(std::span<const float> in) {
  const std::size_t n = taps_.size();
  const std::size_t m = in.size();
  if (m == 0)
    return {};

  // Lay the carried tail and this block out contiguously, so each output is a
  // modulo-free sliding dot product over `window_`. (reserve is a no-op once
  // prepare() has run; only an unbudgeted block grows it.)
  const std::size_t carry = history_.size();
  window_.reserve(carry + m);
  const std::span<float> window = window_.write_n(carry + m);
  std::ranges::copy(history_, window.begin());
  std::ranges::copy(in, window.begin() + static_cast<std::ptrdiff_t>(carry));

  // Evaluate the convolution only at kept positions: start `phase_` samples into
  // the block, then stride by the decimation factor. `phase_` carries whatever
  // skip is left over into the next block, so chunking can't shift the grid.
  const std::size_t outputs = (m > phase_) ? (m - phase_ + decimation_ - 1) / decimation_ : 0;
  out_.reserve(outputs);
  float *y = out_.write_n(outputs).data();
  const float *taps = taps_.data();
  const float *w = window.data();
  std::size_t j = phase_;
  for (std::size_t k = 0; k < outputs; ++k, j += decimation_)
    y[k] = convolve(taps, w + j, n);
  phase_ = j - m;

  // Carry the last size()-1 samples into the next call.
  if (carry != 0)
    std::ranges::copy(window.end() - static_cast<std::ptrdiff_t>(carry), window.end(), history_.begin());

  return out_.view();
}

} // namespace palindrome::dsp
