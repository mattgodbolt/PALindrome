#pragma once

// The synthetic composite envelope shared by the video-stage test files
// (VideoTest, ScreenTest, ChromaDecoderTest), so the three agree on what a
// test line looks like and the signal can't drift between them. The line's
// geometry is load-bearing beyond this file: ChromaDecoderTest's
// synth_colour_composite layers its burst (line_len/12) and active-video
// (line_len/6) windows onto this sync width, and its killer_test_config
// burst gate is tuned to that layout - change the fractions here in step
// with those.

#include <cstddef>
#include <vector>

namespace videotest {

// A crude synthetic composite envelope: `lines` lines of `line_len` samples
// each, sync tip (1.0) at the start of every line, back porch + active video
// (0.3 black) with one white bar (0.0) so there is content to slice around.
// Negatively modulated, so sync is the peak — exactly what the separator
// expects. Enough lines for the sweep to lock.
inline std::vector<float> synth_composite(std::size_t lines, std::size_t line_len) {
  std::vector<float> e(lines * line_len, 0.3f);
  for (std::size_t l = 0; l < lines; ++l) {
    const std::size_t base = l * line_len;
    for (std::size_t k = 0; k < line_len; ++k) {
      if (k < line_len / 14) // ~4.6 us at 1028 samples/line: line-sync width
        e[base + k] = 1.0f;
      else if (k > line_len / 2 && k < line_len / 2 + 40) // a white bar mid-line
        e[base + k] = 0.0f;
    }
  }
  return e;
}

inline constexpr double kRate = 16.0e6;

} // namespace videotest
