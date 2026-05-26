#include "palindrome/wav.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace wav = palindrome::wav;

namespace {

std::vector<unsigned char> read_all(const std::filesystem::path &path) {
  std::ifstream in{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::uint32_t le32(const unsigned char *p) {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::uint16_t le16(const unsigned char *p) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1]) << 8);
}

} // namespace

TEST_CASE("write_mono_float emits a well-formed WAV that round-trips") {
  const auto path = std::filesystem::temp_directory_path() / "palindrome_wav_test.wav";
  std::filesystem::remove(path);

  const std::array<float, 4> samples{0.0f, 0.5f, -0.5f, 1.0f};
  wav::write_mono_float(path, samples, 48000);

  const auto bytes = read_all(path);
  REQUIRE(bytes.size() == 44 + samples.size() * sizeof(float)); // 44-byte canonical header

  CHECK(std::memcmp(bytes.data(), "RIFF", 4) == 0);
  CHECK(le32(&bytes[4]) == 36 + samples.size() * sizeof(float));
  CHECK(std::memcmp(&bytes[8], "WAVE", 4) == 0);
  CHECK(std::memcmp(&bytes[12], "fmt ", 4) == 0);
  CHECK(le32(&bytes[16]) == 16); // fmt chunk size
  CHECK(le16(&bytes[20]) == 3); // IEEE float
  CHECK(le16(&bytes[22]) == 1); // mono
  CHECK(le32(&bytes[24]) == 48000); // sample rate
  CHECK(le32(&bytes[28]) == 48000 * 4); // byte rate
  CHECK(le16(&bytes[32]) == 4); // block align
  CHECK(le16(&bytes[34]) == 32); // bits per sample
  CHECK(std::memcmp(&bytes[36], "data", 4) == 0);
  CHECK(le32(&bytes[40]) == samples.size() * sizeof(float));

  // The payload should be the samples verbatim.
  std::array<float, 4> round_trip{};
  std::memcpy(round_trip.data(), &bytes[44], sizeof(round_trip));
  CHECK(round_trip == samples);

  std::filesystem::remove(path);
}

TEST_CASE("write_mono_float reports an unwritable path") {
  CHECK_THROWS_AS(
      wav::write_mono_float("/no/such/directory/out.wav", std::array<float, 1>{0.0f}, 8000), wav::WriteError);
}
