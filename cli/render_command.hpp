#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>

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
  double cutoff_{5.0e6};
  double sync_cutoff_{1.2e6}; // narrow low-pass on the sync-detection branch
  std::size_t decimate_{2}; // 32 MS/s / 2 = 16 MS/s => ~1024 samples/line
  std::size_t width_{720};
  std::size_t height_{576};
  double persistence_{1.2}; // phosphor persistence in field periods
  double beam_sigma_{0.8}; // beam-spot vertical size in output rows
  double gamma_{1.0}; // electron-gun gamma
  std::size_t frame_stride_{0}; // 0 => one image; N => a PNG every Nth field boundary
  bool no_sound_trap_{false};
  double sound_q_{10.0};
  bool no_sync_{false}; // debug: naive-fold the envelope, bypassing the sync graph
  // Sync/sweep hold knobs — the rest of the decoder, surfaced for the tuner.
  double sync_level_{0.85}; // separator slice level
  double h_kp_{1.0}, h_ki_{1.0e-5}, h_clamp_{0.2}; // horizontal hold (AFC PI + omega clamp)
  double v_level_{0.4}, v_kp_{1.0}, v_ki_{2.0e-8}; // vertical hold (vsync slice + PI)
  double v_tc_{0.5}, v_minfield_{0.7}; // vertical integrator tc, min field fraction
};

} // namespace palindrome::cli
