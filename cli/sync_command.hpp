#pragma once

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
  std::filesystem::path recording_;
  double carrier_{0.0};
  double cutoff_{5.0e6};
  std::size_t decimate_{2};
  bool no_sound_trap_{false};
  double sound_q_{10.0};
};

} // namespace palindrome::cli
