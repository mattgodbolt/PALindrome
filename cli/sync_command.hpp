#pragma once

#include "cli_util.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

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
  double composite_scale_{0.0}; // volts at input full scale; 0 => nominal
  double composite_sync_v_{0.0}; // the source's sync amplitude in volts; 0 => nominal
  // 0 => the mode's default: EnvelopeOptions' corner for RF, the composite
  // vision band for composite - matching render, so this tool measures the
  // same signal the decoder locks to. Measured on pattern.bin, the undecimated
  // composite filter leaves the statistics alone (jitter 0.029 -> 0.026 us,
  // equalising 30 -> 43 of ~31k pulses, line rate within 0.001%).
  double cutoff_{0.0};
  // "flat" by default, unlike render: the microscope measures the signal, not
  // a receiver's SAW curve. The saw modes opt into the receiver's-eye view.
  std::string if_mode_{"flat"};
  // 0 = take the mode's default: /2 for flat RF, where sync only needs the
  // slow pulse shapes, and /1 for composite and the saw modes, where there is
  // chroma (or baseband detail) to keep under Nyquist. Any explicit
  // --decimate wins in every mode.
  std::size_t decimate_{0};
};

} // namespace palindrome::cli
