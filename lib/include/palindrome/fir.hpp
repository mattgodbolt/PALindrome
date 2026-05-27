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
  explicit Fir(std::vector<float> taps);

  // Filter `in`, appending one output sample per input sample to `out`.
  void process(std::span<const float> in, std::vector<float> &out);

  [[nodiscard]] std::size_t size() const { return taps_.size(); }

private:
  std::vector<float> taps_; // reversed, so a window dot product runs forward
  std::vector<float> history_; // last size()-1 input samples, carried across calls
  std::vector<float> window_; // scratch: history followed by the current block
};

} // namespace palindrome::dsp
