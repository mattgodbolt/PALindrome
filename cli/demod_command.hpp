#pragma once

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
  double slowdown_{1000.0}; // WAV is stamped at real sample_rate / this
  bool no_sound_trap_{false}; // disable the sound-carrier notch
  double sound_q_{10.0}; // sound-trap notch Q
};

} // namespace palindrome::cli
