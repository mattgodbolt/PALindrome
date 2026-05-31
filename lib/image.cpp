#include "palindrome/image.hpp"

#include <cstddef>
#include <format>
#include <fstream>
#include <ios>
#include <vector>

#include <lodepng.h>

namespace palindrome::image {

void write_png_grey(
    const std::filesystem::path &path, std::span<const std::uint8_t> pixels, unsigned width, unsigned height) {
  const auto expected = static_cast<std::size_t>(width) * height;
  if (pixels.size() != expected)
    throw WriteError{std::format("image is {} bytes, expected {}x{} = {}", pixels.size(), width, height, expected)};

  // Encode in memory and write the buffer ourselves: lodepng's filename
  // overload calls fwrite/fclose without checking their return codes (see
  // lodepng_save_file in lodepng.cpp), so a disk-full or short-write after a
  // successful fopen would silently report success and leave a corrupt PNG.
  std::vector<unsigned char> buffer;
  if (const unsigned err = lodepng::encode(buffer, pixels.data(), width, height, LCT_GREY, 8))
    throw WriteError{std::format("lodepng encode of {}: {}", path.string(), lodepng_error_text(err))};

  std::ofstream out;
  out.exceptions(std::ios::badbit | std::ios::failbit);
  try {
    out.open(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    out.close(); // explicit close so a failed flush throws, rather than being swallowed by the destructor
  }
  catch (const std::ios_base::failure &e) {
    throw WriteError{std::format("write of {}: {}", path.string(), e.what())};
  }
}

void write_png_rgb(
    const std::filesystem::path &path, std::span<const std::uint8_t> pixels, unsigned width, unsigned height) {
  const auto expected = static_cast<std::size_t>(width) * height * 3;
  if (pixels.size() != expected)
    throw WriteError{std::format("image is {} bytes, expected 3x{}x{} = {}", pixels.size(), width, height, expected)};

  // Encode in memory then write ourselves, for the same reason write_png_grey
  // does: lodepng's filename overload ignores fwrite/fclose return codes.
  std::vector<unsigned char> buffer;
  if (const unsigned err = lodepng::encode(buffer, pixels.data(), width, height, LCT_RGB, 8))
    throw WriteError{std::format("lodepng encode of {}: {}", path.string(), lodepng_error_text(err))};

  std::ofstream out;
  out.exceptions(std::ios::badbit | std::ios::failbit);
  try {
    out.open(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    out.close();
  }
  catch (const std::ios_base::failure &e) {
    throw WriteError{std::format("write of {}: {}", path.string(), e.what())};
  }
}

GreyImage read_png_grey(const std::filesystem::path &path) {
  GreyImage img;
  if (const unsigned err = lodepng::decode(img.pixels, img.width, img.height, path.string(), LCT_GREY, 8))
    throw ReadError{std::format("lodepng decode of {}: {}", path.string(), lodepng_error_text(err))};
  return img;
}

} // namespace palindrome::image
