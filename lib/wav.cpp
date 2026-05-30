#include "palindrome/wav.hpp"

#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <ostream>

namespace palindrome::wav {

namespace {

void put_u16(std::ostream &os, std::uint16_t v) {
  const unsigned char bytes[]{static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
  os.write(reinterpret_cast<const char *>(bytes), sizeof(bytes));
}

void put_u32(std::ostream &os, std::uint32_t v) {
  const unsigned char bytes[]{
      static_cast<unsigned char>(v),
      static_cast<unsigned char>(v >> 8),
      static_cast<unsigned char>(v >> 16),
      static_cast<unsigned char>(v >> 24),
  };
  os.write(reinterpret_cast<const char *>(bytes), sizeof(bytes));
}

constexpr std::uint16_t format_ieee_float = 3;
constexpr std::uint16_t channels = 1;
constexpr std::uint16_t bits_per_sample = 32;

} // namespace

void write_mono_float(const std::filesystem::path &path, std::span<const float> samples, std::uint32_t sample_rate_hz) {
  std::ofstream os{path, std::ios::binary};
  if (!os)
    throw WriteError(std::format("could not open WAV for writing: {}", path.string()));

  constexpr std::uint32_t block_align = channels * (bits_per_sample / 8);
  const std::uint32_t byte_rate = sample_rate_hz * block_align;

  // RIFF is a 32-bit format: the size fields can't describe more than 4 GiB.
  const std::size_t total = samples.size() * sizeof(float);
  if (total > std::numeric_limits<std::uint32_t>::max() - 36u)
    throw WriteError(std::format("WAV too large for 32-bit RIFF header: {}", path.string()));
  const auto data_bytes = static_cast<std::uint32_t>(total);

  os.write("RIFF", 4);
  put_u32(os, 36 + data_bytes); // RIFF chunk size = 4 ("WAVE") + 24 (fmt) + 8 (data header) + data
  os.write("WAVE", 4);

  os.write("fmt ", 4);
  put_u32(os, 16); // PCM/float fmt chunk size
  put_u16(os, format_ieee_float);
  put_u16(os, channels);
  put_u32(os, sample_rate_hz);
  put_u32(os, byte_rate);
  put_u16(os, block_align);
  put_u16(os, bits_per_sample);

  os.write("data", 4);
  put_u32(os, data_bytes);
  os.write(reinterpret_cast<const char *>(samples.data()), static_cast<std::streamsize>(data_bytes));

  if (!os)
    throw WriteError(std::format("failed while writing WAV: {}", path.string()));
}

} // namespace palindrome::wav
