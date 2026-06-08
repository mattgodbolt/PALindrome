#pragma once

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
  std::filesystem::path recording_;
  std::filesystem::path output_;
  double carrier_{0.0}; // 0 => take the vision carrier from metadata
  double cutoff_{5.0e6}; // baseband low-pass corner
  std::size_t decimate_{1}; // keep one output sample per this many inputs
  double slowdown_{1000.0}; // WAV is stamped at real output rate / this
};

} // namespace palindrome::cli
