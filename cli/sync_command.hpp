#pragma once

#include "cli_util.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>

#include <lyra/lyra.hpp>

namespace palindrome::cli {

// `palindrome sync <recording>` — a diagnostic for the sync chain. It runs the
// AM envelope and the sync separator, then reports what the separator sliced
// out: how many sync pulses, their width distribution (line sync vs the
// vertical-interval broad / equalising pulses), and the spacing between
// line-sync pulses. It then runs the horizontal sweep and reports the lock.
// This is the microscope for debugging the timebase — no picture, just numbers.
class SyncCommand {
public:
  void add_to(lyra::cli &cli, std::function<int()> &action);

private:
  int run() const;
  static constexpr EnvelopeOptions kDefaults{}; // flag defaults come from the library (see RenderCommand)
  std::filesystem::path recording_;
  std::string input_mode_{"rf"}; // "rf" (modulated real IF) | "composite" (baseband CVBS)
  double carrier_{0.0};
  double cutoff_{kDefaults.cutoff_hz};
  // Composite needs no decimation: the pulse shapes are already at baseband and
  // there is no chroma to keep under Nyquist, so nothing is bought by halving.
  std::size_t composite_decimate_{1};
  std::size_t decimate_{2}; // deliberately /2 (not the library's 1): sync only needs the slow pulse shapes
};

} // namespace palindrome::cli
