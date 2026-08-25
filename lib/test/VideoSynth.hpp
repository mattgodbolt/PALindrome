#pragma once

// The synthetic composite envelope shared by the video-stage test files
// (VideoTest, ScreenTest, ChromaDecoderTest) and ChromaBench, so they agree
// on what a test line looks like and the signal can't drift between them. The
// line's geometry is load-bearing: synth_colour_composite layers its burst
// (line_len/12) and active-video (line_len/6) windows onto this sync width,
// and ChromaDecoderTest's killer_test_config burst gate is tuned to that
// layout - change the fractions here in step with those.

#include <cmath>
#include <cstddef>
#include <numbers>
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

// A synthetic composite carrying chroma: synth_composite plus ~10 cycles of
// colour burst on the back porch and a constant-phase subcarrier across the
// active video, so the chroma decoder has a real burst to gate and a steady
// colour to recover. The burst swings ±45° about its mean axis on alternate
// lines — the PAL swinging burst, which is what the ident (and so the colour
// killer) recognises as PAL; without the swing the killer rightly treats the
// signal as not-PAL and mutes it.
inline std::vector<float> synth_colour_composite(std::size_t lines, std::size_t line_len, double fsc_offset_hz = 0.0) {
  auto e = synth_composite(lines, line_len);
  const double fsc = 4.43361875e6 + fsc_offset_hz;
  const double w = 2.0 * std::numbers::pi * fsc / kRate;
  for (std::size_t l = 0; l < lines; ++l) {
    const std::size_t base = l * line_len;
    const double swing = (l % 2 == 0 ? 1.0 : -1.0) * std::numbers::pi / 4.0;
    for (std::size_t k = 0; k < line_len; ++k) {
      const double phase = w * static_cast<double>(base + k);
      const bool in_burst = k >= line_len / 12 && k < line_len / 12 + 36;
      const bool in_active = k > line_len / 6;
      if (in_burst)
        e[base + k] += 0.15f * static_cast<float>(std::sin(phase + swing)); // ±45° swinging burst
      else if (in_active)
        e[base + k] += 0.08f * static_cast<float>(std::cos(phase)); // steady chroma
    }
  }
  return e;
}

} // namespace videotest
