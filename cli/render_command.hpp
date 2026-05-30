#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>

#include <lyra/lyra.hpp>

namespace palindrome::cli {

// `palindrome render <recording>` — a sync-locked picture out of a recording.
// The signal flows through the analog-style stages: AM envelope (same as
// `demod`) -> sync separator -> horizontal sweep (AFC flywheel) -> pixel splat,
// with x driven by the locked horizontal phase and y advanced on each locked
// line. Vertical sync is still manual via --start-line / --lines until the
// vertical timebase lands, so expect the half-line interlace offset between
// the two fields and pre-lock garbage at the top.
class RenderCommand {
public:
  void add_to(lyra::cli &cli, std::function<int()> &action);

private:
  int run() const;
  std::filesystem::path recording_;
  std::filesystem::path output_;
  double carrier_{0.0}; // 0 => take the vision carrier from metadata
  double cutoff_{5.0e6};
  std::size_t decimate_{2}; // 32 MS/s / 2 = 16 MS/s => ~1024 samples/line
  std::size_t width_{720};
  std::size_t lines_{625};
  std::size_t start_line_{0};
  bool no_sound_trap_{false};
  double sound_q_{10.0};
};

} // namespace palindrome::cli
