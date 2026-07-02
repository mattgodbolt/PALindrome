#pragma once

#include "cli_util.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>

#include <lyra/lyra.hpp>

namespace palindrome::cli {

// `palindrome demod <recording>` — AM-demodulate the vision carrier to a WAV.
class DemodCommand {
public:
  void add_to(lyra::cli &cli, std::function<int()> &action);

private:
  int run() const;
  static constexpr EnvelopeOptions kDefaults{}; // flag defaults come from the library (see RenderCommand)
  std::filesystem::path recording_;
  std::filesystem::path output_;
  double carrier_{0.0}; // 0 => take the vision carrier from metadata
  double cutoff_{kDefaults.cutoff_hz}; // baseband low-pass corner
  std::size_t decimate_{kDefaults.decimation}; // keep one output sample per this many inputs
  double slowdown_{1000.0}; // WAV is stamped at real output rate / this
};

} // namespace palindrome::cli
