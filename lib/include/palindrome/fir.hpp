#pragma once

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

  // Filter `in`, appending one output sample per `decimation()` input samples to
  // `out`. The decimation phase carries across calls, so the result is
  // independent of how the input is chunked.
  void process(std::span<const float> in, std::vector<float> &out);

  [[nodiscard]] std::size_t size() const { return taps_.size(); }
  [[nodiscard]] std::size_t decimation() const { return decimation_; }

private:
  std::vector<float> taps_; // reversed, so a window dot product runs forward
  std::vector<float> history_; // last size()-1 input samples, carried across calls
  std::vector<float> window_; // scratch: history followed by the current block
  std::size_t decimation_; // keep one output per this many inputs
  std::size_t phase_{}; // inputs still to skip before the next kept output
};

} // namespace palindrome::dsp
