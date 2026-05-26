#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>

// Minimal WAV writing, just enough to dump demodulated signals for inspection
// in an external viewer (Audacity etc.). Assumes a little-endian host.
namespace palindrome::wav {

class WriteError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Write `samples` to `path` as a mono 32-bit IEEE-float WAV, stamping the
// header with `sample_rate_hz`. Samples are written verbatim; players treat
// [-1, 1] as nominal full scale. Throws WriteError on any I/O failure.
void write_mono_float(const std::filesystem::path &path, std::span<const float> samples, std::uint32_t sample_rate_hz);

} // namespace palindrome::wav
