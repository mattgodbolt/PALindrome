#pragma once

#include "palindrome/demod.hpp"
#include "palindrome/sigmf.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
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
  bool carrier_scanned = false; // the carrier came from a signal scan, not metadata
  double metadata_carrier_hz = 0.0; // what the metadata claimed (0 = none), for the scan diagnostic
};

// Load a PAL recording from `recording` (a .sigmf-meta path or a basename) and
// resolve the vision carrier. Precedence: `carrier_override != 0` (an absolute
// IF) wins; else, unless `force_scan`, the metadata (rx888:vision_if_hz /
// airspy:vision_if_hz); else a coarse FFT scan of the signal itself
// (demod::find_vision_carrier) - so a recording with no carrier metadata, the
// live-RF case, still decodes. `force_scan` takes the scan even when metadata is
// present (to validate it). Throws (a std::exception — runtime_error for a
// missing/invalid recording or a complex ci16 input, invalid_argument for a scan
// that finds no carrier) — main catches it and prints "palindrome: <what>".
[[nodiscard]] LoadedRecording load_recording(
    const std::filesystem::path &recording, double carrier_override = 0.0, bool force_scan = false);

// Stream ri16-LE (real int16) from `data_path` as float blocks of up to
// `block_samples` samples, scaling each to [-1, 1). Invokes `on_block` with each
// block (a span owned by an internal buffer, valid only for that call). Never
// accumulates the whole signal — the streaming path the demod stages drive.
// Throws std::runtime_error on a file-open failure.
void stream_ri16le_blocks(const std::filesystem::path &data_path,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples = std::size_t{1} << 16);

// Which front end demodulates the vision carrier. The saw modes run one
// complex-coefficient FIR shaped like a receiver SAW IF (Nyquist flank,
// vestige cutoff, finite sound notch, group-delay ripple - see
// demod::IfTemplate); flat is the ideal symmetric low-pass the saw modes
// replaced, kept verbatim as the legacy/comparison chain.
enum class IfMode { saw80, saw90, flat };

// Demod parameters that aren't carried in the recording itself. The detector
// applies to the saw modes only: the flat chain is the legacy envelope
// demodulator, kept verbatim.
struct EnvelopeOptions {
  double cutoff_hz = 5.0e6; // baseband low-pass corner (flat mode only)
  std::size_t decimation = 1;
  IfMode if_mode = IfMode::flat;
  demod::Detector detector = demod::Detector::quasi_sync; // saw modes' detector
  std::optional<double> sound_notch_db{}; // dB of IF sound rejection, overriding the template
  std::optional<double> gd_ripple_ns{}; // peak group-delay ripple ns, overriding the template
};

// What stream_envelope reports back to the caller.
struct EnvelopeStream {
  double rate_hz = 0.0; // envelope sample rate (input rate / decimation)
  double carrier_hz = 0.0; // the vision carrier actually used (resolved by a live scan)
  // Where the quasi-sync AFC left the carrier, Hz from carrier_hz. Engaged
  // (and so reported) only when that loop ran: the envelope detector and the
  // flat chain have no AFC, and "no loop" must not print as "measured zero".
  std::optional<double> afc_offset_hz{};
  std::vector<std::string> warnings; // demod warnings (e.g. cutoff vs decimated Nyquist)
};

// Demodulate a loaded recording to composite-envelope blocks: form the analytic
// signal (so the real IF's carrier image can't fold onto the chroma), mix the
// vision carrier to DC, low-pass, and take the magnitude. Invokes `on_block`
// with each envelope block (owned by the demodulator, valid only for that call).
// Throws std::runtime_error on a file-open failure.
EnvelopeStream stream_envelope(const LoadedRecording &loaded, const EnvelopeOptions &opts,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples = std::size_t{1} << 16);

// Live variant: the same demod, but the real-IF samples come from stdin (a
// continuous SDR stream, e.g. `airspy_rx -r /dev/stdout | palindrome render
// --live`) instead of a file, and it runs until stdin closes. `sample_rate_hz`
// is the real input rate (the file path had it in the metadata). The carrier is
// `carrier_override` if positive, else scanned from the opening samples of the
// stream (demod::find_vision_carrier) - the resolved value comes back in
// EnvelopeStream::carrier_hz. Invokes `on_block` with each envelope block (owned,
// valid only for that call).
EnvelopeStream stream_envelope_live(double sample_rate_hz, double carrier_override, const EnvelopeOptions &opts,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples = std::size_t{1} << 16);

} // namespace palindrome::cli
