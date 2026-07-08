#include "palindrome/fir.hpp"

#include <algorithm>
#include <cmath>
#include <format>
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
    [[maybe_unused]] std::size_t win_len) {
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
    // Like the d == 1 tier, carry four accumulators (32 outputs, a 64-sample
    // input span) per strip so the tap loop isn't bound by one chain's 4-cycle
    // FMA latency. Each lane still sums its taps in natural order with one fma,
    // so the result is bit-identical to the 8-wide strip and the scalar dot.
    for (; k + 32 <= outputs && 2 * k + 48 + n + 15 <= win_len; k += 32) {
      __m256 a0 = _mm256_setzero_ps();
      __m256 a1 = _mm256_setzero_ps();
      __m256 a2 = _mm256_setzero_ps();
      __m256 a3 = _mm256_setzero_ps();
      const float *wb = window + 2 * k;
      for (std::size_t t = 0; t < n; ++t) {
        const __m256 tap = _mm256_broadcast_ss(taps + t);
        const __m256 l0 = _mm256_loadu_ps(wb + t);
        const __m256 l1 = _mm256_loadu_ps(wb + t + 8);
        const __m256 l2 = _mm256_loadu_ps(wb + t + 16);
        const __m256 l3 = _mm256_loadu_ps(wb + t + 24);
        const __m256 l4 = _mm256_loadu_ps(wb + t + 32);
        const __m256 l5 = _mm256_loadu_ps(wb + t + 40);
        const __m256 l6 = _mm256_loadu_ps(wb + t + 48);
        const __m256 l7 = _mm256_loadu_ps(wb + t + 56);
        a0 = _mm256_fmadd_ps(
            tap, _mm256_permutevar8x32_ps(_mm256_shuffle_ps(l0, l1, _MM_SHUFFLE(2, 0, 2, 0)), even), a0);
        a1 = _mm256_fmadd_ps(
            tap, _mm256_permutevar8x32_ps(_mm256_shuffle_ps(l2, l3, _MM_SHUFFLE(2, 0, 2, 0)), even), a1);
        a2 = _mm256_fmadd_ps(
            tap, _mm256_permutevar8x32_ps(_mm256_shuffle_ps(l4, l5, _MM_SHUFFLE(2, 0, 2, 0)), even), a2);
        a3 = _mm256_fmadd_ps(
            tap, _mm256_permutevar8x32_ps(_mm256_shuffle_ps(l6, l7, _MM_SHUFFLE(2, 0, 2, 0)), even), a3);
      }
      _mm256_storeu_ps(y + k, a0);
      _mm256_storeu_ps(y + k + 8, a1);
      _mm256_storeu_ps(y + k + 16, a2);
      _mm256_storeu_ps(y + k + 24, a3);
    }
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

// Deinterleave a window into its even/odd sample planes (E[i] = w[2i],
// O[i] = w[2i+1]) for the polyphase d == 2 pair tier below. One pass over the
// block (microseconds) instead of the shuffle work the tier used to repeat per
// tap. Plain C++ on purpose: this is off the binding port either way, so it
// earns no intrinsics.
void split_even_odd(const float *w, float *even_plane, float *odd_plane, std::size_t win_len) {
  const std::size_t pairs = win_len / 2;
  for (std::size_t i = 0; i < pairs; ++i) {
    even_plane[i] = w[2 * i];
    odd_plane[i] = w[2 * i + 1];
  }
  if (win_len % 2 != 0)
    even_plane[pairs] = w[win_len - 1]; // an odd window's last sample is an even-plane entry
}

// The fused-pair counterpart of convolve_strip: two tap sets, one window sweep,
// with every window load feeding both accumulator sets - the load ports the
// d == 1 tier is bound on, which is where the pair's ~40% comes from there; the
// FMA count is unchanged. The d == 2 tier goes further: instead of
// re-deinterleaving the window's even lanes per tap (8 port-5 shuffle uops per
// tap per strip - the binding resource, paid n times over the same data), the
// caller splits the window into even/odd planes ONCE per block and the tap
// loop alternates planes - even taps read even_plane, odd taps read odd_plane,
// at the same plane offset - so the strip runs at the d == 1 FMA bound with no
// shuffles at all (~2x the shuffling pair, measured). Every output still
// accumulates its own taps in natural t order with the same single-rounded
// FMAs on the same operand values, so the pair is bit-identical to two
// independent Firs whichever tier runs. The polyphase loads are exactly the
// samples each output needs (no over-read), so unlike convolve_strip's d == 2
// tier there is no window-length guard to carry.
// TODO(std::simd): re-express these tiers (and convolve_strip's) in std::simd
// when GCC 16's <simd> is production-ready - the fused structure carries over
// unchanged, 512-bit lanes would halve the strip count on this box, and the
// polyphase shape (not the shuffle one) is the right basis for the d == 2 port.
void convolve_strip_pair(const float *rtaps, const float *itaps, const float *window,
    [[maybe_unused]] const float *even_plane, [[maybe_unused]] const float *odd_plane, float *yr, float *yi,
    std::size_t n, std::size_t outputs, std::size_t d) {
  std::size_t k = 0;
#if defined(__AVX2__) && defined(__FMA__)
  if (d == 1) {
    for (; k + 32 <= outputs; k += 32) {
      __m256 a0 = _mm256_setzero_ps();
      __m256 a1 = _mm256_setzero_ps();
      __m256 a2 = _mm256_setzero_ps();
      __m256 a3 = _mm256_setzero_ps();
      __m256 b0 = _mm256_setzero_ps();
      __m256 b1 = _mm256_setzero_ps();
      __m256 b2 = _mm256_setzero_ps();
      __m256 b3 = _mm256_setzero_ps();
      const float *wb = window + k;
      for (std::size_t t = 0; t < n; ++t) {
        const __m256 tr = _mm256_broadcast_ss(rtaps + t);
        const __m256 ti = _mm256_broadcast_ss(itaps + t);
        const __m256 l0 = _mm256_loadu_ps(wb + t + 0);
        const __m256 l1 = _mm256_loadu_ps(wb + t + 8);
        const __m256 l2 = _mm256_loadu_ps(wb + t + 16);
        const __m256 l3 = _mm256_loadu_ps(wb + t + 24);
        a0 = _mm256_fmadd_ps(tr, l0, a0);
        a1 = _mm256_fmadd_ps(tr, l1, a1);
        a2 = _mm256_fmadd_ps(tr, l2, a2);
        a3 = _mm256_fmadd_ps(tr, l3, a3);
        b0 = _mm256_fmadd_ps(ti, l0, b0);
        b1 = _mm256_fmadd_ps(ti, l1, b1);
        b2 = _mm256_fmadd_ps(ti, l2, b2);
        b3 = _mm256_fmadd_ps(ti, l3, b3);
      }
      _mm256_storeu_ps(yr + k + 0, a0);
      _mm256_storeu_ps(yr + k + 8, a1);
      _mm256_storeu_ps(yr + k + 16, a2);
      _mm256_storeu_ps(yr + k + 24, a3);
      _mm256_storeu_ps(yi + k + 0, b0);
      _mm256_storeu_ps(yi + k + 8, b1);
      _mm256_storeu_ps(yi + k + 16, b2);
      _mm256_storeu_ps(yi + k + 24, b3);
    }
    for (; k + 8 <= outputs; k += 8) {
      __m256 a = _mm256_setzero_ps();
      __m256 b = _mm256_setzero_ps();
      const float *wb = window + k;
      for (std::size_t t = 0; t < n; ++t) {
        const __m256 l = _mm256_loadu_ps(wb + t);
        a = _mm256_fmadd_ps(_mm256_broadcast_ss(rtaps + t), l, a);
        b = _mm256_fmadd_ps(_mm256_broadcast_ss(itaps + t), l, b);
      }
      _mm256_storeu_ps(yr + k, a);
      _mm256_storeu_ps(yi + k, b);
    }
  }
  else if (d == 2) {
    // Polyphase: output k's tap t is window[2k + t] = (t even ? even_plane
    // : odd_plane)[k + t/2], so walking taps in pairs (2u, 2u+1) shares one
    // plane offset and keeps the natural t order every output requires.
    for (; k + 32 <= outputs; k += 32) {
      __m256 a0 = _mm256_setzero_ps();
      __m256 a1 = _mm256_setzero_ps();
      __m256 a2 = _mm256_setzero_ps();
      __m256 a3 = _mm256_setzero_ps();
      __m256 b0 = _mm256_setzero_ps();
      __m256 b1 = _mm256_setzero_ps();
      __m256 b2 = _mm256_setzero_ps();
      __m256 b3 = _mm256_setzero_ps();
      const float *eb = even_plane + k;
      const float *ob = odd_plane + k;
      std::size_t t = 0;
      for (; t + 2 <= n; t += 2) {
        const std::size_t u = t / 2;
        const __m256 tre = _mm256_broadcast_ss(rtaps + t);
        const __m256 tie = _mm256_broadcast_ss(itaps + t);
        const __m256 e0 = _mm256_loadu_ps(eb + u);
        const __m256 e1 = _mm256_loadu_ps(eb + u + 8);
        const __m256 e2 = _mm256_loadu_ps(eb + u + 16);
        const __m256 e3 = _mm256_loadu_ps(eb + u + 24);
        a0 = _mm256_fmadd_ps(tre, e0, a0);
        a1 = _mm256_fmadd_ps(tre, e1, a1);
        a2 = _mm256_fmadd_ps(tre, e2, a2);
        a3 = _mm256_fmadd_ps(tre, e3, a3);
        b0 = _mm256_fmadd_ps(tie, e0, b0);
        b1 = _mm256_fmadd_ps(tie, e1, b1);
        b2 = _mm256_fmadd_ps(tie, e2, b2);
        b3 = _mm256_fmadd_ps(tie, e3, b3);
        const __m256 tro = _mm256_broadcast_ss(rtaps + t + 1);
        const __m256 tio = _mm256_broadcast_ss(itaps + t + 1);
        const __m256 o0 = _mm256_loadu_ps(ob + u);
        const __m256 o1 = _mm256_loadu_ps(ob + u + 8);
        const __m256 o2 = _mm256_loadu_ps(ob + u + 16);
        const __m256 o3 = _mm256_loadu_ps(ob + u + 24);
        a0 = _mm256_fmadd_ps(tro, o0, a0);
        a1 = _mm256_fmadd_ps(tro, o1, a1);
        a2 = _mm256_fmadd_ps(tro, o2, a2);
        a3 = _mm256_fmadd_ps(tro, o3, a3);
        b0 = _mm256_fmadd_ps(tio, o0, b0);
        b1 = _mm256_fmadd_ps(tio, o1, b1);
        b2 = _mm256_fmadd_ps(tio, o2, b2);
        b3 = _mm256_fmadd_ps(tio, o3, b3);
      }
      for (; t < n; ++t) { // odd tap count: one trailing even-plane tap
        const std::size_t u = t / 2;
        const __m256 tre = _mm256_broadcast_ss(rtaps + t);
        const __m256 tie = _mm256_broadcast_ss(itaps + t);
        const __m256 e0 = _mm256_loadu_ps(eb + u);
        const __m256 e1 = _mm256_loadu_ps(eb + u + 8);
        const __m256 e2 = _mm256_loadu_ps(eb + u + 16);
        const __m256 e3 = _mm256_loadu_ps(eb + u + 24);
        a0 = _mm256_fmadd_ps(tre, e0, a0);
        a1 = _mm256_fmadd_ps(tre, e1, a1);
        a2 = _mm256_fmadd_ps(tre, e2, a2);
        a3 = _mm256_fmadd_ps(tre, e3, a3);
        b0 = _mm256_fmadd_ps(tie, e0, b0);
        b1 = _mm256_fmadd_ps(tie, e1, b1);
        b2 = _mm256_fmadd_ps(tie, e2, b2);
        b3 = _mm256_fmadd_ps(tie, e3, b3);
      }
      _mm256_storeu_ps(yr + k, a0);
      _mm256_storeu_ps(yr + k + 8, a1);
      _mm256_storeu_ps(yr + k + 16, a2);
      _mm256_storeu_ps(yr + k + 24, a3);
      _mm256_storeu_ps(yi + k, b0);
      _mm256_storeu_ps(yi + k + 8, b1);
      _mm256_storeu_ps(yi + k + 16, b2);
      _mm256_storeu_ps(yi + k + 24, b3);
    }
    for (; k + 8 <= outputs; k += 8) {
      __m256 a = _mm256_setzero_ps();
      __m256 b = _mm256_setzero_ps();
      const float *eb = even_plane + k;
      const float *ob = odd_plane + k;
      for (std::size_t t = 0; t < n; ++t) {
        const __m256 l = _mm256_loadu_ps((t % 2 == 0 ? eb : ob) + t / 2);
        a = _mm256_fmadd_ps(_mm256_broadcast_ss(rtaps + t), l, a);
        b = _mm256_fmadd_ps(_mm256_broadcast_ss(itaps + t), l, b);
      }
      _mm256_storeu_ps(yr + k, a);
      _mm256_storeu_ps(yi + k, b);
    }
  }
#endif
  for (; k < outputs; ++k) {
    yr[k] = convolve(rtaps, window + d * k, n);
    yi[k] = convolve(itaps, window + d * k, n);
  }
}

} // namespace

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
    tap = static_cast<float>(static_cast<double>(tap) / sum); // normalise to unity DC gain
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
      tap = static_cast<float>(static_cast<double>(tap) / gain);
  return taps;
}

std::vector<float> notch_kernel(
    std::size_t num_taps, double sample_rate_hz, double low_hz, double high_hz, Window window) {
  if (num_taps % 2 == 0)
    throw std::invalid_argument("notch needs an odd tap count (the unit impulse must land on the centre tap)");
  auto k = bandpass_kernel(num_taps, sample_rate_hz, low_hz, high_hz, window);
  for (auto &t: k)
    t = -t;
  k[(num_taps - 1) / 2] += 1.0f; // + unit impulse at the centre tap
  return k;
}

std::vector<float> hilbert_kernel(std::size_t num_taps, Window window) {
  if (num_taps == 0)
    throw std::invalid_argument("num_taps must be non-zero");

  const auto m = static_cast<double>(num_taps - 1);
  const double centre = m / 2.0;

  // Type III ideal Hilbert impulse response, windowed: 2/(pi*k) for odd k, 0 for
  // even k (k = i - centre). sin^2(pi*k/2) is the odd-k selector (0 on even k, 1
  // on odd), and makes the centre tap (k == 0) exactly zero. Antisymmetric about
  // the centre, so there is no DC-gain to normalise — a Hilbert kernel has none.
  std::vector<float> taps(num_taps);
  for (std::size_t i = 0; i < num_taps; ++i) {
    const auto k = static_cast<double>(i) - centre;
    double h = 0.0;
    if (k != 0.0) {
      const double s = std::sin(pi * k / 2.0);
      h = (2.0 / (pi * k)) * s * s;
    }
    taps[i] = static_cast<float>(h * window_value(window, static_cast<double>(i), m));
  }
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

FirPair::FirPair(std::vector<float> re_taps, std::vector<float> im_taps, std::size_t decimation) :
    re_taps_{std::move(re_taps)}, im_taps_{std::move(im_taps)}, decimation_{decimation} {
  if (re_taps_.empty())
    throw std::invalid_argument("FIR pair needs at least one tap");
  if (re_taps_.size() != im_taps_.size())
    throw std::invalid_argument(
        std::format("FIR pair needs equal-length tap sets ({} vs {})", re_taps_.size(), im_taps_.size()));
  if (decimation_ == 0)
    throw std::invalid_argument("FIR pair decimation must be >= 1");
  std::ranges::reverse(re_taps_);
  std::ranges::reverse(im_taps_);
  history_.assign(re_taps_.size() - 1, 0.0f);
}

void FirPair::prepare(std::size_t max_in) {
  window_.reserve(history_.size() + max_in);
  out_re_.reserve(max_output_for(max_in));
  out_im_.reserve(max_output_for(max_in));
  if (decimation_ == 2) {
    even_.reserve((history_.size() + max_in) / 2 + 1);
    odd_.reserve((history_.size() + max_in) / 2 + 1);
  }
}

// Fir::process with one window layout serving both halves; see that function
// for the window/phase mechanics.
FirPair::Outputs FirPair::process(std::span<const float> in) {
  const auto n = re_taps_.size();
  const auto m = in.size();
  if (m == 0)
    return {};

  const auto carry = history_.size();
  const auto window = window_.write_n(carry + m);
  std::ranges::copy(history_, window.begin());
  std::ranges::copy(in, window.begin() + static_cast<std::ptrdiff_t>(carry));

  const std::size_t outputs = (m > phase_) ? (m - phase_ + decimation_ - 1) / decimation_ : 0;
  auto *yr = out_re_.write_n(outputs).data();
  auto *yi = out_im_.write_n(outputs).data();
  const auto *w = window.data();
  // The polyphase d == 2 tier reads the window through its even/odd planes;
  // phase_ shifts which input sample is "even", so split from w + phase_.
  const float *even_plane = nullptr;
  const float *odd_plane = nullptr;
  if (decimation_ == 2 && outputs != 0) {
    const auto win_len = carry + m - phase_;
    const auto ev = even_.write_n(win_len / 2 + 1);
    const auto od = odd_.write_n(win_len / 2 + 1);
    split_even_odd(w + phase_, ev.data(), od.data(), win_len);
    even_plane = ev.data();
    odd_plane = od.data();
  }
  convolve_strip_pair(
      re_taps_.data(), im_taps_.data(), w + phase_, even_plane, odd_plane, yr, yi, n, outputs, decimation_);
  phase_ = phase_ + outputs * decimation_ - m;

  if (carry != 0)
    std::ranges::copy(window.end() - static_cast<std::ptrdiff_t>(carry), window.end(), history_.begin());

  return {.re = out_re_.view(), .im = out_im_.view()};
}

} // namespace palindrome::dsp
