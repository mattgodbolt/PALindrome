#include "palindrome/fir.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#endif

namespace palindrome::dsp {

namespace {
constexpr double pi = std::numbers::pi;

// One output sample: the dot product of the taps with `window`, accumulated in
// natural order with a single-rounded fused multiply-add. The float accumulator
// trades precision for throughput (error grows ~n*eps), which suits the moderate,
// amplitude-limited filters here. Natural order — not a reassociated tree — so the
// result is bit-identical to convolve_strip's per-output lanes, which is what lets
// a decimating filter equal "filter at full rate, then keep every Nth", and keeps
// the stream chunking-invariant. Used for the decimated grid; the non-decimating
// hot path goes through convolve_strip.
float convolve(const float *taps, const float *window, std::size_t n) {
  float acc = 0.0f;
  for (std::size_t k = 0; k < n; ++k)
    acc = std::fmaf(taps[k], window[k], acc);
  return acc;
}

// y[k] = sum_t taps[t] * window[k+t] for k in [0, outputs) — the non-decimating
// convolution. The classic across-taps dot product (convolve) stalls on a single
// accumulator's FMA-latency chain and pays a horizontal reduction per output; this
// transposes the loops to carry a strip of outputs in named vector accumulators
// across the tap sweep, hiding the latency with no per-output reduction (GCC spills
// any array-based strip, hence the intrinsics). The decimating case (d > 1) keeps
// one output per d inputs: d == 2 (the front-end's vision low-pass) loads the 2x
// span and deinterleaves the even lanes, avoiding gather instructions (throttled
// by the GDS microcode mitigation on this Skylake-X); larger, non-hot strides fall
// to the scalar dot.
//
// Every output — whatever tier or stride lands on it — accumulates the taps in
// natural order with a single-rounded fused multiply-add, so it is bit-identical
// to convolve()'s scalar dot. That is what makes a decimating filter equal "filter
// at full rate, then keep every dth" and the stream independent of how it's chunked
// (the block-invariance guarantee).
void convolve_strip(const float *taps, const float *window, float *y, std::size_t n, std::size_t outputs, std::size_t d,
    std::size_t win_len) {
  std::size_t k = 0;
#if defined(__AVX2__) && defined(__FMA__)
  if (d == 1) {
    for (; k + 32 <= outputs; k += 32) {
      __m256 a0 = _mm256_setzero_ps();
      __m256 a1 = _mm256_setzero_ps();
      __m256 a2 = _mm256_setzero_ps();
      __m256 a3 = _mm256_setzero_ps();
      const float *wb = window + k;
      for (std::size_t t = 0; t < n; ++t) {
        const __m256 tap = _mm256_broadcast_ss(taps + t);
        a0 = _mm256_fmadd_ps(tap, _mm256_loadu_ps(wb + t + 0), a0);
        a1 = _mm256_fmadd_ps(tap, _mm256_loadu_ps(wb + t + 8), a1);
        a2 = _mm256_fmadd_ps(tap, _mm256_loadu_ps(wb + t + 16), a2);
        a3 = _mm256_fmadd_ps(tap, _mm256_loadu_ps(wb + t + 24), a3);
      }
      _mm256_storeu_ps(y + k + 0, a0);
      _mm256_storeu_ps(y + k + 8, a1);
      _mm256_storeu_ps(y + k + 16, a2);
      _mm256_storeu_ps(y + k + 24, a3);
    }
    for (; k + 8 <= outputs; k += 8) {
      __m256 a = _mm256_setzero_ps();
      const float *wb = window + k;
      for (std::size_t t = 0; t < n; ++t)
        a = _mm256_fmadd_ps(_mm256_broadcast_ss(taps + t), _mm256_loadu_ps(wb + t), a);
      _mm256_storeu_ps(y + k, a);
    }
  }
  else if (d == 2) {
    // Lane L wants window[2k + t + 2L]; load the 16-sample span [2k+t .. 2k+t+15]
    // and gather its even elements into one register.
    const __m256i even = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
    // The hi load reads window[2k+t+8 .. +15]; cap k so its last access (t=n-1)
    // stays in the window. The leftover outputs drop to the in-bounds scalar dot.
    for (; k + 8 <= outputs && 2 * k + n + 15 <= win_len; k += 8) {
      __m256 a = _mm256_setzero_ps();
      const float *wb = window + 2 * k;
      for (std::size_t t = 0; t < n; ++t) {
        const __m256 lo = _mm256_loadu_ps(wb + t);
        const __m256 hi = _mm256_loadu_ps(wb + t + 8);
        const __m256 sh = _mm256_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0));
        a = _mm256_fmadd_ps(_mm256_broadcast_ss(taps + t), _mm256_permutevar8x32_ps(sh, even), a);
      }
      _mm256_storeu_ps(y + k, a);
    }
  }
#endif
  for (; k < outputs; ++k)
    y[k] = convolve(taps, window + d * k, n);
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
  const auto m = static_cast<double>(num_taps - 1);
  const double centre = m / 2.0;

  std::vector<float> taps(num_taps);
  double sum = 0.0;
  for (std::size_t i = 0; i < num_taps; ++i) {
    const auto k = static_cast<double>(i) - centre;
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

std::vector<float> bandpass_kernel(
    std::size_t num_taps, double sample_rate_hz, double low_hz, double high_hz, Window window) {
  if (num_taps == 0)
    throw std::invalid_argument("num_taps must be non-zero");
  if (!(low_hz > 0.0 && low_hz < high_hz && high_hz < sample_rate_hz / 2.0))
    throw std::invalid_argument("bandpass needs 0 < low < high < sample_rate / 2");

  const double flo = low_hz / sample_rate_hz; // cycles per sample
  const double fhi = high_hz / sample_rate_hz;
  const auto m = static_cast<double>(num_taps - 1);
  const double centre = m / 2.0;

  // Band-pass = high-cut sinc minus low-cut sinc, windowed. Then scale to unity
  // gain at the band centre frequency so the chroma amplitude is preserved.
  std::vector<float> taps(num_taps);
  const double fc = 0.5 * (flo + fhi); // band centre, cycles per sample
  double gain_re = 0.0;
  double gain_im = 0.0;
  for (std::size_t i = 0; i < num_taps; ++i) {
    const auto k = static_cast<double>(i) - centre;
    const double hi = (k == 0.0) ? 2.0 * fhi : std::sin(2.0 * pi * fhi * k) / (pi * k);
    const double lo = (k == 0.0) ? 2.0 * flo : std::sin(2.0 * pi * flo * k) / (pi * k);
    const double tap = (hi - lo) * window_value(window, static_cast<double>(i), m);
    taps[i] = static_cast<float>(tap);
    gain_re += tap * std::cos(2.0 * pi * fc * k);
    gain_im += tap * std::sin(2.0 * pi * fc * k);
  }
  const double gain = std::hypot(gain_re, gain_im);
  if (gain > 0.0)
    for (float &tap: taps)
      tap = static_cast<float>(tap / gain);
  return taps;
}

std::vector<float> notch_kernel(
    std::size_t num_taps, double sample_rate_hz, double low_hz, double high_hz, Window window) {
  auto k = bandpass_kernel(num_taps, sample_rate_hz, low_hz, high_hz, window);
  for (auto &t: k)
    t = -t;
  k[(num_taps - 1) / 2] += 1.0f; // + unit impulse at the centre tap
  return k;
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
  const auto n = taps_.size();
  const auto m = in.size();
  if (m == 0)
    return {};

  // Lay the carried tail and this block out contiguously, so each output is a
  // modulo-free sliding dot product over `window_`. prepare() must have budgeted
  // for the largest block; write_n throws on anything bigger.
  const auto carry = history_.size();
  const auto window = window_.write_n(carry + m);
  std::ranges::copy(history_, window.begin());
  std::ranges::copy(in, window.begin() + static_cast<std::ptrdiff_t>(carry));

  // Evaluate the convolution only at kept positions: start `phase_` samples into
  // the block, then stride by the decimation factor. `phase_` carries whatever
  // skip is left over into the next block, so chunking can't shift the grid.
  const std::size_t outputs = (m > phase_) ? (m - phase_ + decimation_ - 1) / decimation_ : 0;
  auto *y = out_.write_n(outputs).data();
  const auto *taps = taps_.data();
  const auto *w = window.data();
  // From the first kept position, convolve_strip evaluates output k at window
  // offset phase_ + k*decimation_, vectorising across outputs to hide the FMA
  // latency chain that throttles the across-taps dot product. The skip left over
  // past the block carries into the next call, so chunking can't shift the grid.
  convolve_strip(taps, w + phase_, y, n, outputs, decimation_, carry + m - phase_);
  phase_ = phase_ + outputs * decimation_ - m;

  // Carry the last size()-1 samples into the next call.
  if (carry != 0)
    std::ranges::copy(window.end() - static_cast<std::ptrdiff_t>(carry), window.end(), history_.begin());

  return out_.view();
}

} // namespace palindrome::dsp
