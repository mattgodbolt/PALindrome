#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

// Minimal image I/O, for dumping rendered frames to disk for inspection and
// reading reference frames back in tests.
namespace palindrome::image {

class WriteError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class ReadError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Write `pixels` to `path` as an 8-bit greyscale PNG. The buffer is row-major,
// exactly width*height bytes, with 0 = black and 255 = white. Throws WriteError
// on a size mismatch or any encode/I-O failure.
void write_png_grey(
    const std::filesystem::path &path, std::span<const std::uint8_t> pixels, unsigned width, unsigned height);

// An 8-bit greyscale image read from disk: row-major width*height bytes.
struct GreyImage {
  unsigned width{};
  unsigned height{};
  std::vector<std::uint8_t> pixels;
};

// Read `path` as an 8-bit greyscale PNG (any source colour type is reduced to
// grey). Throws ReadError on any decode/I-O failure.
[[nodiscard]] GreyImage read_png_grey(const std::filesystem::path &path);

} // namespace palindrome::image
