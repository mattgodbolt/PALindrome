#pragma once

#include "palindrome/buffer.hpp"

#include <cstddef>
#include <span>
#include <vector>

// FIR filtering: a streaming, stateful filter plus kernel-design helpers. Like
// the other DSP stages, Fir::process carries its delay line across calls, so
// the output is independent of how the input is chunked.
namespace palindrome::dsp {

enum class Window { Hamming, Blackman };

// A windowed-sinc low-pass kernel, normalised to unity DC gain. An odd
// num_taps gives a symmetric (linear-phase) kernel with an integer group delay
// of (num_taps - 1) / 2 samples. Throws std::invalid_argument if num_taps is 0
// or cutoff_hz is not in (0, sample_rate_hz / 2).
[[nodiscard]] std::vector<float> lowpass_kernel(
    std::size_t num_taps, double sample_rate_hz, double cutoff_hz, Window window = Window::Hamming);

// A windowed-sinc band-pass kernel (difference of two low-pass sincs), normalised
// to unity gain at the band centre. Passes (low_hz, high_hz); an odd num_taps is
// linear-phase with group delay (num_taps - 1) / 2. Throws std::invalid_argument
// if num_taps is 0 or the band is not 0 < low < high < sample_rate / 2.
[[nodiscard]] std::vector<float> bandpass_kernel(
    std::size_t num_taps, double sample_rate_hz, double low_hz, double high_hz, Window window = Window::Hamming);

// A band-stop (notch) kernel: the all-pass impulse minus a band-pass, so a signal
// keeps its full bandwidth except the notched band (e.g. a chroma trap on luma,
// rather than a soft low-pass). Same length and group delay as the band-pass it
// is built from; num_taps must be odd so the unit impulse lands on the centre tap.
[[nodiscard]] std::vector<float> notch_kernel(
    std::size_t num_taps, double sample_rate_hz, double low_hz, double high_hz, Window window = Window::Hamming);

// A Type III FIR Hilbert transformer (90° phase shift): an antisymmetric kernel
// with zeros at DC and Nyquist. The ideal impulse response is 2/(pi*k) for odd k
// and 0 for even k (k = tap - centre), windowed. Convolving a real signal with it
// yields the signal's quadrature; pairing that with the input delayed by the
// kernel's group delay (num_taps-1)/2 forms the analytic (one-sided, +frequency)
// signal — see demod::Hilbert. Rate-independent (a 90° shift at every frequency),
// so it takes no sample rate. num_taps should be odd for an integer group delay.
// Throws std::invalid_argument if num_taps is 0.
[[nodiscard]] std::vector<float> hilbert_kernel(std::size_t num_taps, Window window = Window::Hamming);

class Fir {
public:
  // A decimating FIR: with `decimation` D it keeps one of every D output
  // samples (output rate = input rate / D), and — being a *decimating* filter —
  // only evaluates the convolution at the kept positions, so it costs ~D times
  // less than filtering at full rate. The caller is responsible for the
  // anti-alias budget: the kernel must suppress everything above the decimated
  // Nyquist (input_rate / (2*D)) or it folds back into band. Throws
  // std::invalid_argument on empty taps or decimation < 1.
  explicit Fir(std::vector<float> taps, std::size_t decimation = 1);

  // Budget internal storage for input blocks of up to `max_in` samples, so the
  // streaming path never allocates. Required before process(): the hot path does
  // not grow, so a block bigger than the budget throws (std::length_error).
  void prepare(std::size_t max_in);

  // Filter `in`, returning a view of one output sample per `decimation()` input
  // samples. The decimation phase carries across calls, so the result is
  // independent of how the input is chunked. The returned span is owned by the
  // filter and valid only until the next process() call.
  [[nodiscard]] std::span<const float> process(std::span<const float> in);

  // Upper bound on the outputs a single process() call can emit for `n_in`
  // inputs, regardless of the carried decimation phase -- i.e. the count when
  // the phase is zero, ceil(n_in / decimation). Use this to size storage; the
  // exact per-call count (phase-dependent) is process()'s own business and is
  // reported by the size of the span it returns.
  [[nodiscard]] std::size_t max_output_for(std::size_t n_in) const noexcept {
    return (n_in + decimation_ - 1) / decimation_;
  }

  // The input block size this filter prefers: a multiple of decimation() keeps
  // every block aligned to the decimation grid (phase stays zero).
  [[nodiscard]] std::size_t input_multiple() const noexcept { return decimation_; }
  [[nodiscard]] std::size_t size() const noexcept { return taps_.size(); }
  [[nodiscard]] std::size_t decimation() const noexcept { return decimation_; }

private:
  std::vector<float> taps_; // reversed, so a window dot product runs forward
  std::vector<float> history_; // last size()-1 input samples, carried across calls
  Buffer<float> window_; // scratch: history followed by the current block
  Buffer<float> out_; // owned output, reused across calls
  std::size_t decimation_; // keep one output per this many inputs
  std::size_t phase_{}; // inputs still to skip before the next kept output
};

} // namespace palindrome::dsp
