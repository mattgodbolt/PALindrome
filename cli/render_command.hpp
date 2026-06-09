#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

#include <lyra/lyra.hpp>

namespace palindrome::cli {

// `palindrome render <recording>` — a sync-locked picture out of a recording.
// The signal flows through the analog-style video graph (AM envelope -> sync
// separator -> horizontal sweep + vertical sync -> phosphor screen): the beam
// paints brightness at (x = h_phase*width, y = v_phase*height) onto a decaying
// phosphor. Both timebases lock automatically, so there's no manual framing;
// interlace falls out of the half-line field offset.
class RenderCommand {
public:
  void add_to(lyra::cli &cli, std::function<int()> &action);

private:
  int run() const;
  std::filesystem::path recording_;
  std::filesystem::path output_;
  double carrier_{0.0}; // 0 => take the vision carrier from metadata
  double cutoff_{4.8e6};
  double sync_cutoff_{1.2e6}; // narrow low-pass on the sync-detection branch
  std::size_t decimate_{0}; // 0 => auto from the sample rate (keep chroma below 0.7*Nyquist)
  std::size_t width_{720};
  std::size_t height_{576};
  double persistence_{1.6}; // phosphor persistence in field periods
  double beam_sigma_{0.8}; // beam-spot vertical size in output rows
  double beam_sigma_x_{-1.0}; // beam-spot horizontal size in cols; <0 = match (round)
  double gamma_{1.5}; // electron-gun gamma
  // Colour decode (PAL-D chroma channel). Off => a grey render.
  bool colour_{false};
  double saturation_{0.17}; // chroma gain (fraction of white reference)
  double contrast_{0.85}; // readout white point (AGC-relative)
  double h_blank_{0.16}; // retrace blanking end, h_phase
  double subcarrier_{0.0}; // subcarrier crystal Hz; 0 => textbook 4.43361875 MHz
  double uv_bandwidth_{0.0}; // post-demod U/V low-pass corner Hz; 0 => decoder default
  double band_lo_{0.0}; // chroma band-pass edges Hz; 0 => decoder default
  double band_hi_{0.0};
  double burst_gate_lo_{0.11}; // burst gate, h_phase window
  double burst_gate_hi_{0.14};
  bool no_delay_line_{false}; // deprecated alias for --comb-mode off
  std::string comb_mode_; // "off" | "post" | "delay-line"; empty => decoder default (post)
  double ref_tc_{10.0}; // APC reference time constant in lines (EMA rate = 1/ref_tc)
  std::size_t frame_stride_{0}; // 0 => one image; N => a PNG every Nth field boundary
  bool no_sync_{false}; // debug: naive-fold the envelope, bypassing the sync graph
  bool no_threads_{false}; // decode serially instead of the default stage pipeline
  // Sync/sweep hold knobs — the rest of the decoder, surfaced for the tuner.
  double sync_level_{0.85}; // separator slice level
  double h_kp_{1.0}; // horizontal hold (AFC PI + omega clamp)
  double h_ki_{1.0e-5};
  double h_clamp_{0.2};
  double v_level_{0.4}; // vertical hold (vsync slice + PI)
  double v_kp_{1.0};
  double v_ki_{2.0e-8};
  double v_tc_{0.5}; // vertical integrator tc
  double v_minfield_{0.7}; // min field fraction
};

} // namespace palindrome::cli
