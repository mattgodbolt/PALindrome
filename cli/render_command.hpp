#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>

#include <lyra/lyra.hpp>

namespace palindrome::cli {

// `palindrome render <recording>` — a first, deliberately naive picture out of a
// recording. It AM-demodulates the vision carrier (the same streaming stages as
// `demod`) and then just folds the composite envelope into a width-by-lines
// grid: row = sample / width, col = sample % width. There is no sync detection
// yet, so the image rolls and shears — by exactly the amount the real line rate
// differs from nominal, which is the point: it shows us the error the eventual
// sync-locked deflection model has to track out.
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
  std::size_t width_{0}; // 0 => round(samples per line) from the line rate
  std::size_t lines_{625};
  std::size_t start_line_{0};
  bool no_sound_trap_{false};
  double sound_q_{10.0};
};

} // namespace palindrome::cli
