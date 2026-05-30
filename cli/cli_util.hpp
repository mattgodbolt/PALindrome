#pragma once

#include "palindrome/pipeline.hpp"
#include "palindrome/sigmf.hpp"

#include <complex>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace palindrome::cli {

// Resolve a recording argument — either a .sigmf-meta path or a bare basename
// such as "corpus/alex_kidd" — to the metadata file path.
[[nodiscard]] std::filesystem::path resolve_meta(std::filesystem::path path);

// What load_recording returns: the parsed metadata, the paired meta/data paths,
// and the RF parameters every consumer needs. Two flavours of recording: real
// int16 IF (RX888) and complex int16 baseband (AirSpy); complex_baseband says
// which, and the matching carrier field is the one to use.
struct LoadedRecording {
  sigmf::Metadata meta;
  std::filesystem::path meta_path;
  std::filesystem::path data_path;
  double sample_rate_hz{};
  bool complex_baseband = false; // true => ci16_le (AirSpy), false => ri16_le (RX888)
  double vision_carrier_hz{}; // real path: absolute IF carrier to mix down
  double vision_offset_hz{}; // complex path: vision carrier offset from core:frequency
};

// Load a PAL recording from `recording` (a .sigmf-meta path or a basename),
// accepting real int16 IF (ri16_le) or complex int16 baseband (ci16_le), and
// resolve the vision carrier/offset from the metadata. `carrier_override != 0`
// takes precedence (an absolute IF for real, an offset for complex). Throws
// std::runtime_error on a missing/invalid recording (main catches and prints
// "palindrome: <what>").
[[nodiscard]] LoadedRecording load_recording(const std::filesystem::path &recording, double carrier_override = 0.0);

// Stream int16-LE samples from `data_path` through `chain` in blocks of
// `block_samples`, scaling each input to a float in [-1, 1) (divide by 32768),
// and invoke `on_block` with each produced output block. The span passed to
// `on_block` is owned by the chain and valid only for that call (consume it
// before returning). Never accumulates the whole signal — this is the path the
// live/streaming consumers use. Throws std::runtime_error on a file-open failure.
void stream_blocks_through_chain(const std::filesystem::path &data_path, Chain &chain,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples = std::size_t{1} << 16);

// Convenience wrapper that accumulates the whole streamed output into one
// vector (reserving from the file size up front). For batch consumers — the
// inspection tools — not the streaming path. Throws on a file-open failure.
[[nodiscard]] std::vector<float> stream_int16le_through_chain(
    const std::filesystem::path &data_path, Chain &chain, std::size_t block_samples = std::size_t{1} << 16);

// Stream ci16-LE (interleaved I,Q int16) from `data_path` as complex<float>
// blocks of up to `block_samples` complex samples, scaling each component to
// [-1, 1). Invokes `on_block` with each block (a span owned by an internal
// buffer, valid only for that call). The complex-baseband counterpart of
// stream_blocks_through_chain. Throws std::runtime_error on a file-open failure.
void stream_ci16le_blocks(const std::filesystem::path &data_path,
    const std::function<void(std::span<const std::complex<float>>)> &on_block,
    std::size_t block_samples = std::size_t{1} << 16);

// Demod parameters that aren't carried in the recording itself.
struct EnvelopeOptions {
  double cutoff_hz = 5.0e6;
  std::size_t decimation = 1;
  bool no_sound_trap = false; // real-IF path only
  double sound_q = 10.0; // real-IF path only
};

// What stream_envelope reports back to the caller.
struct EnvelopeStream {
  double rate_hz = 0.0; // envelope sample rate (input rate / decimation)
  std::vector<std::string> warnings; // demod warnings (e.g. cutoff vs decimated Nyquist)
};

// Demodulate a loaded recording to composite-envelope blocks, hiding whether it
// is a real IF strip (RX888) or complex baseband (AirSpy). Invokes `on_block`
// with each envelope block (owned by the demodulator, valid only for that
// call). The real/complex branch is resolved once here, not in every consumer.
// Throws std::runtime_error on a file-open failure.
EnvelopeStream stream_envelope(const LoadedRecording &loaded, const EnvelopeOptions &opts,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples = std::size_t{1} << 16);

} // namespace palindrome::cli
