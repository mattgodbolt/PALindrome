#pragma once

#include "palindrome/composite.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/sigmf.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace palindrome::cli {

// Resolve a recording argument — either a .sigmf-meta path or a bare basename
// such as "corpus/alex_kidd" — to the metadata file path.
[[nodiscard]] std::filesystem::path resolve_meta(std::filesystem::path path);

// Which kind of signal the recording holds. rf is a modulated real IF that goes
// through the vision front end. composite is baseband CVBS, which skips the IF
// filter and detector entirely: video::CompositeInput maps it onto the same
// envelope rail, so every stage downstream is unchanged and does not know.
enum class InputMode { rf, composite };

// How the raw samples are stored. Real int16 is the SDR recordings' format;
// real unsigned 8-bit is what a CX2388x card hands over through cxadc, whose
// ADC is 8-bit and whose zero sits at mid-scale.
enum class SampleFormat { s16, u8 };

// Parse an --input value, printing `<command>: --input must be one of: ...` to
// stderr and returning nullopt if it is not one. Shared so the commands cannot
// drift on either the accepted names or the wording.
[[nodiscard]] std::optional<InputMode> parse_input_mode(std::string_view command, std::string_view value);

// What load_recording returns: the parsed metadata, the paired meta/data paths,
// and the RF parameters every consumer needs. Recordings are real int16 IF
// (ri16_le) — the RX888, or the AirSpy's raw 20 MS/s real mode; the decoder
// makes them analytic at the front (see demod::Hilbert), so there is no separate
// complex-baseband input.
struct LoadedRecording {
  SampleFormat format = SampleFormat::s16;
  sigmf::Metadata meta;
  std::filesystem::path meta_path;
  std::filesystem::path data_path;
  double sample_rate_hz{};
  double vision_carrier_hz{}; // absolute IF carrier to mix down
  bool carrier_scanned = false; // the carrier came from a signal scan, not metadata
  double metadata_carrier_hz = 0.0; // what the metadata claimed (0 = none), for the scan diagnostic
  // Non-fatal complaints for the command to print. This layer never writes to
  // stderr itself: sync and demod call load_recording too, so a message
  // prefixed by one command's name would be a lie in the others.
  std::vector<std::string> warnings;
};

// How to load one. A struct rather than a tail of defaulted positional
// parameters, matching CompositeInputConfig and AgcConfig elsewhere.
struct LoadOptions {
  double carrier_override = 0.0; // absolute IF; 0 = take it from metadata or scan
  bool force_scan = false; // scan even when metadata has a carrier, to validate it
  InputMode input = InputMode::rf;
};

// Load a PAL recording from `recording` (a .sigmf-meta path or a basename) and
// resolve the vision carrier. Precedence: `carrier_override != 0` (an absolute
// IF) wins; else, unless `force_scan`, the metadata (rx888:vision_if_hz /
// airspy:vision_if_hz); else a coarse FFT scan of the signal itself
// (demod::find_vision_carrier) - so a recording with no carrier metadata, the
// live-RF case, still decodes. `force_scan` takes the scan even when metadata is
// present (to validate it).
//
// None of that applies to `input == composite`: baseband CVBS has no carrier,
// so resolution is skipped entirely and it never scans - scanning it would find
// a spectral peak that is not a carrier. A composite load that finds carrier
// metadata anyway says so through `warnings`, since that is what an RF
// recording opened in the wrong mode looks like.
//
// Throws (a std::exception — runtime_error for a missing/invalid recording, a
// complex ci16 input, an unsupported datatype, or a scan that finds no carrier)
// — main catches it and prints "palindrome: <what>".
[[nodiscard]] LoadedRecording load_recording(const std::filesystem::path &recording, const LoadOptions &opts = {});

// Stream real samples from `data_path` as float blocks of up to `block_samples`
// samples, scaling each to [-1, 1): int16 by 1/32768, unsigned 8-bit about its
// mid-scale zero. Invokes `on_block` with each
// block (a span owned by an internal buffer, valid only for that call). Never
// accumulates the whole signal — the streaming path the demod stages drive.
// Throws std::runtime_error on a file-open failure.
void stream_real_blocks(const std::filesystem::path &data_path, SampleFormat format,
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
  InputMode input = InputMode::rf;
  // Live only: a recording carries its format in the metadata instead.
  SampleFormat sample_format = SampleFormat::s16;
  // The composite stage's own config, so a new knob is one line in one place
  // and no default is written down twice. sample_rate_hz is the exception:
  // make_front_end fills it, since only it knows the post-decimation rate.
  video::CompositeInputConfig composite{};
  double cutoff_hz = 5.0e6; // baseband low-pass corner (flat mode; and composite when decimating)
  std::size_t decimation = 1;
  IfMode if_mode = IfMode::flat;
  demod::Detector detector = demod::Detector::quasi_sync; // saw modes' detector
  std::optional<double> sound_notch_db{}; // dB of IF sound rejection, overriding the template
  std::optional<double> gd_ripple_ns{}; // peak group-delay ripple ns, overriding the template
};

// What stream_envelope reports back to the caller.
struct EnvelopeStream {
  double rate_hz = 0.0; // envelope sample rate (input rate / decimation)
  double carrier_hz = 0.0; // the vision carrier used (metadata, --carrier, or a recording scan)
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

// Stream real samples from stdin as float blocks until the pipe closes — the
// live counterpart of stream_real_blocks (which reads a named file).
void stream_real_stdin(SampleFormat format, const std::function<void(std::span<const float>)> &on_block,
    std::size_t block_samples = std::size_t{1} << 16);

// Live variant: the same demod, but the real-IF samples come from stdin (a
// continuous SDR stream, e.g. `airspy_rx -r /dev/stdout | palindrome render
// --live`) instead of a file, and it runs until stdin closes. `sample_rate_hz`
// is the real input rate (the file path had it in the metadata). `carrier_hz`
// is required (throws std::runtime_error otherwise): the tuner's IF-plan
// target, i.e. the channel preset - the input stage places the carrier there
// and the AFC absorbs the drift, as on a real tuned set. Invokes `on_block`
// with each envelope block (owned, valid only for that call).
EnvelopeStream stream_envelope_live(double sample_rate_hz, double carrier_hz, const EnvelopeOptions &opts,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples = std::size_t{1} << 16);

} // namespace palindrome::cli
