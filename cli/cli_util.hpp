#pragma once

#include "palindrome/sigmf.hpp"

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
// and the RF parameters every consumer needs. Recordings are real int16 IF
// (ri16_le) — the RX888, or the AirSpy's raw 20 MS/s real mode; the decoder
// makes them analytic at the front (see demod::Hilbert), so there is no separate
// complex-baseband input.
struct LoadedRecording {
  sigmf::Metadata meta;
  std::filesystem::path meta_path;
  std::filesystem::path data_path;
  double sample_rate_hz{};
  double vision_carrier_hz{}; // absolute IF carrier to mix down
};

// Load a PAL recording from `recording` (a .sigmf-meta path or a basename) and
// resolve the vision carrier from the metadata (rx888:vision_if_hz or
// airspy:vision_if_hz). `carrier_override != 0` takes precedence (an absolute
// IF). Throws std::runtime_error on a missing/invalid recording, or on a
// complex (ci16) input (recapture as real) — main catches and prints
// "palindrome: <what>".
[[nodiscard]] LoadedRecording load_recording(const std::filesystem::path &recording, double carrier_override = 0.0);

// Stream ri16-LE (real int16) from `data_path` as float blocks of up to
// `block_samples` samples, scaling each to [-1, 1). Invokes `on_block` with each
// block (a span owned by an internal buffer, valid only for that call). Never
// accumulates the whole signal — the streaming path the demod stages drive.
// Throws std::runtime_error on a file-open failure.
void stream_ri16le_blocks(const std::filesystem::path &data_path,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples = std::size_t{1} << 16);

// Demod parameters that aren't carried in the recording itself.
struct EnvelopeOptions {
  double cutoff_hz = 5.0e6;
  std::size_t decimation = 1;
};

// What stream_envelope reports back to the caller.
struct EnvelopeStream {
  double rate_hz = 0.0; // envelope sample rate (input rate / decimation)
  std::vector<std::string> warnings; // demod warnings (e.g. cutoff vs decimated Nyquist)
};

// Demodulate a loaded recording to composite-envelope blocks: form the analytic
// signal (so the real IF's carrier image can't fold onto the chroma), mix the
// vision carrier to DC, low-pass, and take the magnitude. Invokes `on_block`
// with each envelope block (owned by the demodulator, valid only for that call).
// Throws std::runtime_error on a file-open failure.
EnvelopeStream stream_envelope(const LoadedRecording &loaded, const EnvelopeOptions &opts,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples = std::size_t{1} << 16);

} // namespace palindrome::cli
