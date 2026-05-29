#include "palindrome/image.hpp"

#include <array>
#include <cstdint>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

namespace image = palindrome::image;

TEST_CASE("write_png_grey round-trips through read_png_grey") {
  const auto path = std::filesystem::temp_directory_path() / "palindrome_image_test.png";
  std::filesystem::remove(path);

  // A 3x2 ramp; row-major, 0 = black .. 255 = white.
  const std::array<std::uint8_t, 6> pixels{0, 51, 102, 153, 204, 255};
  image::write_png_grey(path, pixels, 3, 2);

  const auto got = image::read_png_grey(path);
  CHECK(got.width == 3);
  CHECK(got.height == 2);
  REQUIRE(got.pixels.size() == pixels.size());
  CHECK(std::equal(pixels.begin(), pixels.end(), got.pixels.begin()));

  std::filesystem::remove(path);
}

TEST_CASE("write_png_grey rejects a buffer that doesn't match the dimensions") {
  const std::array<std::uint8_t, 3> pixels{1, 2, 3};
  CHECK_THROWS_AS(image::write_png_grey("/tmp/unused.png", pixels, 2, 2), image::WriteError);
}

TEST_CASE("write_png_grey reports an unwritable path") {
  const std::array<std::uint8_t, 1> pixels{0};
  CHECK_THROWS_AS(image::write_png_grey("/no/such/directory/out.png", pixels, 1, 1), image::WriteError);
}

TEST_CASE("read_png_grey reports a missing file") {
  CHECK_THROWS_AS(image::read_png_grey("/no/such/directory/in.png"), image::ReadError);
}
