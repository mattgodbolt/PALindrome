#pragma once

#include "palindrome/pipeline.hpp"
#include "palindrome/sigmf.hpp"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace palindrome::cli {

// Resolve a recording argument — either a .sigmf-meta path or a bare basename
// such as "corpus/alex_kidd" — to the metadata file path.
[[nodiscard]] std::filesystem::path resolve_meta(std::filesystem::path path);

// What load_real_int16_recording returns: the parsed metadata, the paired
// meta/data paths, and the two RF parameters every consumer needs from them.
struct LoadedRecording {
  sigmf::Metadata meta;
  std::filesystem::path meta_path;
  std::filesystem::path data_path;
  double sample_rate_hz{};
  double vision_carrier_hz{};
};

// Load a real-int16 PAL recording from `recording` (a .sigmf-meta path or a
// basename), validate the datatype, and resolve the vision carrier.
// `carrier_override > 0` takes precedence over rx888:vision_if_hz in the
// metadata. Throws std::runtime_error on a missing/invalid recording (main
// catches and prints "palindrome: <what>").
[[nodiscard]] LoadedRecording load_real_int16_recording(
    const std::filesystem::path &recording, double carrier_override = 0.0);

// Stream int16-LE samples from `data_path` through `chain`, scaling each input
// sample to a float in [-1, 1) (divide by 32768). Reserves the output vector
// from the file size up front (via Chain::max_output_for) so the streaming loop
// never reallocates. Throws std::runtime_error on a file-open failure.
[[nodiscard]] std::vector<float> stream_int16le_through_chain(
    const std::filesystem::path &data_path, Chain &chain, std::size_t block_samples = std::size_t{1} << 16);

} // namespace palindrome::cli
