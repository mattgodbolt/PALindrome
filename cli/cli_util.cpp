#include "cli_util.hpp"

#include "palindrome/composite.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/fir.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <ios>
#include <iostream>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace palindrome::cli {

namespace {
// Which template a saw mode synthesises. No default: a new IfMode then fails to
// compile (-Wswitch -Werror) until it picks one.
[[nodiscard]] demod::IfTemplate template_for(IfMode mode) {
  switch (mode) {
    case IfMode::saw80: return demod::saw80_template();
    case IfMode::saw90: return demod::saw90_template();
    case IfMode::flat: break; // the legacy chain: never synthesised
  }
  std::unreachable();
}
} // namespace

std::filesystem::path resolve_meta(std::filesystem::path path) {
  if (path.extension() != ".sigmf-meta")
    path.replace_extension(".sigmf-meta");
  return path;
}

namespace {
// Carrier-scan geometry for the recording scan.
constexpr std::size_t kScanSamples = std::size_t{1} << 20; // ~50 ms at 20 MS/s
constexpr double kScanLoHz = 1.0e6; // a vision IF never sits below ~1 MHz on either SDR's plan
constexpr double kNyquistGuard = 0.95; // stay off the anti-alias roll-off at the top of the band

// No default: a new CarrierScanError fails to compile until it's worded here.
[[nodiscard]] std::string_view describe(demod::CarrierScanError err) {
  switch (err) {
    case demod::CarrierScanError::too_few_samples: return "not enough samples to scan";
    case demod::CarrierScanError::no_signal: return "no spectral energy in the band";
  }
  std::unreachable();
}

// Read up to `n_samples` real int16 from the head of `data_path`, scaled to
// [-1, 1) - enough signal for a coarse carrier scan without streaming the file.
std::vector<float> read_head(const std::filesystem::path &data_path, std::size_t n_samples) {
  std::ifstream data{data_path, std::ios::binary};
  if (!data)
    throw std::runtime_error{std::format("cannot open data file: {}", data_path.string())};
  std::vector<std::int16_t> raw(n_samples);
  data.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(std::int16_t)));
  const auto got = static_cast<std::size_t>(data.gcount()) / sizeof(std::int16_t);
  std::vector<float> out(got);
  for (std::size_t k = 0; k < got; ++k)
    out[k] = static_cast<float>(raw[k]) * (1.0f / 32768.0f);
  return out;
}

// Find the vision carrier in the recording's opening samples: a power-of-two
// block (~50 ms at 20 MS/s) over the band [1 MHz, 0.95*Nyquist] - low enough to
// skip DC/LO junk, high enough to stay off the anti-alias roll-off, and the
// vision carrier is the dominant line within it. The block is long on purpose:
// finer bins separate the pure carrier line from its close-in video-modulation
// sidebands, which a short block blurs together and the peak then sits between.
// A failed scan is terminal (the recording is what it is), so the expected
// converts to a throw here.
double scan_vision_carrier(const std::filesystem::path &data_path, double sample_rate_hz) {
  const auto head = read_head(data_path, kScanSamples);
  const auto carrier =
      demod::find_vision_carrier(head, sample_rate_hz, kScanLoHz, kNyquistGuard * sample_rate_hz / 2.0);
  if (!carrier)
    throw std::runtime_error{
        std::format("no vision carrier found in {}: {}", data_path.string(), describe(carrier.error()))};
  return *carrier;
}
} // namespace

std::optional<InputMode> parse_input_mode(std::string_view command, std::string_view value) {
  if (value == "rf")
    return InputMode::rf;
  if (value == "composite")
    return InputMode::composite;
  std::println(std::cerr, "{}: --input must be one of: rf, composite", command);
  return std::nullopt;
}

LoadedRecording load_recording(const std::filesystem::path &recording, const LoadOptions &opts) {
  LoadedRecording loaded;
  loaded.meta_path = resolve_meta(recording);
  loaded.meta = sigmf::load(loaded.meta_path); // ParseError derives from runtime_error
  loaded.data_path = sigmf::data_path_for(loaded.meta_path);

  if (!loaded.meta.global.sample_rate)
    throw std::runtime_error{"recording has no core:sample_rate"};
  loaded.sample_rate_hz = *loaded.meta.global.sample_rate;

  const auto &dt = loaded.meta.global.parsed_datatype;
  if (dt.format == sigmf::DataType::Format::SignedInt && dt.bits == 16)
    loaded.format = SampleFormat::s16;
  else if (dt.format == sigmf::DataType::Format::UnsignedInt && dt.bits == 8)
    loaded.format = SampleFormat::u8; // a CX2388x card through cxadc
  else
    throw std::runtime_error{std::format("only real int16 or uint8 input is supported (got {})", dt)};
  if (dt.complex)
    throw std::runtime_error{"complex (ci16) input is no longer supported — recapture as real ri16 "
                             "(the AirSpy now uses raw 20 MS/s real mode; see tools/capture_airspy.py)"};

  // Real IF, vision carrier an absolute IF frequency: the RX888 (rx888:*), or an
  // AirSpy raw 20 MS/s capture (airspy:*). Both decode through the same analytic
  // front end (see stream_envelope).
  if (const auto rx = loaded.meta.field<double>("rx888:vision_if_hz"))
    loaded.metadata_carrier_hz = *rx;
  else if (const auto air = loaded.meta.field<double>("airspy:vision_if_hz"))
    loaded.metadata_carrier_hz = *air;

  // Baseband composite has no carrier to resolve, and scanning it would find a
  // spectral peak that is not one. Warn if the file looks like the other kind.
  if (opts.input == InputMode::composite) {
    if (loaded.metadata_carrier_hz > 0.0)
      loaded.warnings.push_back(
          std::format("--input composite, but {} declares a vision carrier ({:.4f} MHz) - is this an RF recording?",
              loaded.meta_path.string(), loaded.metadata_carrier_hz / 1e6));
    return loaded;
  }

  if (opts.carrier_override > 0.0)
    loaded.vision_carrier_hz = opts.carrier_override;
  else if (loaded.metadata_carrier_hz > 0.0 && !opts.force_scan)
    loaded.vision_carrier_hz = loaded.metadata_carrier_hz;
  else {
    // No carrier to trust (or --scan asked us not to): find it in the signal.
    loaded.vision_carrier_hz = scan_vision_carrier(loaded.data_path, loaded.sample_rate_hz);
    loaded.carrier_scanned = true;
  }

  return loaded;
}

namespace {
// Scale a raw block to [-1, 1). int16 is signed about zero; unsigned 8-bit
// counts up from zero, so its silence sits at mid-scale and comes off first.
// The exact zero hardly matters for composite - the sync-tip clamp restores DC
// downstream - but getting it right keeps the declared volts meaningful.
constexpr float kU8Mid = 128.0f;
void scale_s16(std::span<const std::int16_t> raw, std::span<float> dst) {
  for (std::size_t k = 0; k < raw.size(); ++k)
    dst[k] = static_cast<float>(raw[k]) * (1.0f / 32768.0f);
}
void scale_u8(std::span<const std::uint8_t> raw, std::span<float> dst) {
  for (std::size_t k = 0; k < raw.size(); ++k)
    dst[k] = (static_cast<float>(raw[k]) - kU8Mid) * (1.0f / kU8Mid);
}
[[nodiscard]] std::size_t bytes_per(SampleFormat format) {
  switch (format) {
    case SampleFormat::s16: return sizeof(std::int16_t);
    case SampleFormat::u8: return sizeof(std::uint8_t);
  }
  std::unreachable();
}
void scale_block(SampleFormat format, std::span<const std::byte> raw, std::span<float> dst) {
  switch (format) {
    case SampleFormat::s16: scale_s16({reinterpret_cast<const std::int16_t *>(raw.data()), dst.size()}, dst); return;
    case SampleFormat::u8: scale_u8({reinterpret_cast<const std::uint8_t *>(raw.data()), dst.size()}, dst); return;
  }
  std::unreachable();
}
} // namespace

void stream_real_blocks(const std::filesystem::path &data_path, SampleFormat format,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples) {
  std::ifstream data{data_path, std::ios::binary};
  if (!data)
    throw std::runtime_error{std::format("cannot open data file: {}", data_path.string())};

  const auto width = bytes_per(format);
  std::vector<std::byte> raw(block_samples * width);
  std::vector<float> block(block_samples);
  while (data) {
    data.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
    const auto got = static_cast<std::size_t>(data.gcount()) / width;
    if (got == 0)
      break;
    const std::span<float> dst{block.data(), got};
    scale_block(format, std::span{raw}.first(got * width), dst);
    on_block(dst);
  }
}

namespace {
// Stream real int16 from stdin as float blocks until the pipe closes - the
// live counterpart of stream_real_blocks (which reads a named file).
void stream_real_stdin(
    SampleFormat format, const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples) {
  const auto width = bytes_per(format);
  std::vector<std::byte> raw(block_samples * width);
  std::vector<float> block(block_samples);
  while (true) {
    const auto got = std::fread(raw.data(), width, block_samples, stdin);
    if (got == 0)
      break;
    const std::span<float> dst{block.data(), got};
    scale_block(format, std::span{raw}.first(got * width), dst);
    on_block(dst);
  }
}

// The vision front end as one object: either the SAW-era IF (one complex FIR +
// detector) or the legacy flat chain (Hilbert + ComplexAmEnvelope). Built once
// for a carrier and driven block by block, so the file and live sources share
// exactly one construction path.
struct VisionFrontEnd {
  std::optional<demod::VisionIf> saw;
  std::optional<demod::Hilbert> hilbert;
  std::optional<demod::ComplexAmEnvelope> flat;
  // Composite: an optional decimating anti-alias FIR, then the affine map and
  // sync-tip clamp. No IF filter and no detector - the samples are the video.
  std::optional<dsp::Fir> composite_lp;
  std::optional<video::CompositeInput> composite;

  [[nodiscard]] std::span<const float> process(std::span<const float> x) {
    if (composite)
      return composite->process(composite_lp ? composite_lp->process(x) : x);
    return saw ? saw->process(x) : flat->process(hilbert->process(x));
  }
};

VisionFrontEnd make_front_end(double sample_rate_hz, double carrier_hz, const EnvelopeOptions &opts,
    std::size_t block_samples, std::vector<std::string> &warnings) {
  VisionFrontEnd fe;
  if (opts.input == InputMode::composite) {
    // Filter only when decimating: at the capture rate the composite is already
    // band-limited, and a cutoff near 4.43 MHz would bite into the chroma for
    // no reason. Decimating first also means the clamp sees the low-passed
    // signal, so a noise spike cannot drag it as deep.
    const auto rate_after = sample_rate_hz / static_cast<double>(opts.decimation);
    if (opts.decimation > 1) {
      if (opts.cutoff_hz >= rate_after / 2.0)
        warnings.push_back(std::format("cutoff {:g} MHz exceeds the decimated Nyquist {:g} MHz; expect aliasing",
            opts.cutoff_hz / 1e6, rate_after / 2.0 / 1e6));
      fe.composite_lp.emplace(
          dsp::lowpass_kernel(demod::kDefaultVisionTaps, sample_rate_hz, opts.cutoff_hz), opts.decimation);
      fe.composite_lp->prepare(block_samples);
    }
    auto cfg = opts.composite;
    cfg.sample_rate_hz = rate_after; // only we know it: it depends on the decimation
    fe.composite.emplace(cfg);
    fe.composite->prepare(fe.composite_lp ? fe.composite_lp->max_output_for(block_samples) : block_samples);
    return fe;
  }
  if (opts.if_mode != IfMode::flat) {
    // The SAW-era IF: one complex-coefficient FIR realising the set's IF curve
    // around the carrier, applied straight to the real IF, then the detector.
    // One-sided taps subsume the analytic (Hilbert) step, and the template -
    // not a --cutoff - decides what survives, including a deliberately finite
    // sound notch, so the sound carrier leaves a faint period-true 6 MHz beat.
    auto shape = template_for(opts.if_mode);
    if (opts.sound_notch_db)
      shape.sound_notch_db = -*opts.sound_notch_db; // the option is dB of rejection, positive
    if (opts.gd_ripple_ns)
      shape.gd_ripple_ns = *opts.gd_ripple_ns;
    fe.saw.emplace(
        sample_rate_hz, carrier_hz, shape, opts.detector, demod::kDefaultIfTaps, dsp::Window::Hamming, opts.decimation);
    fe.saw->prepare(block_samples);
    return fe;
  }

  // Flat mode - the pre-SAW chain, kept verbatim: form the analytic (one-sided)
  // signal so the carrier's negative-frequency image can't fold onto the
  // chroma, then mix the vision carrier to DC, low-pass and take the magnitude.
  // The sound carrier (vision + 6 MHz) lands above the chroma after the mix and
  // is removed entirely by the cutoff low-pass - the ideal no real set was.
  fe.hilbert.emplace(demod::kDefaultVisionTaps);
  fe.flat.emplace(
      sample_rate_hz, carrier_hz, opts.cutoff_hz, demod::kDefaultVisionTaps, dsp::Window::Hamming, opts.decimation);
  fe.hilbert->prepare(block_samples);
  fe.flat->prepare(block_samples);
  if (const auto decimated_nyquist = sample_rate_hz / (2.0 * static_cast<double>(opts.decimation));
      opts.cutoff_hz >= decimated_nyquist)
    warnings.push_back(std::format("cutoff {:g} MHz exceeds the decimated Nyquist {:g} MHz; expect aliasing",
        opts.cutoff_hz / 1e6, decimated_nyquist / 1e6));
  return fe;
}
} // namespace

// The demodulators validate decimation themselves, but both stream functions
// divide by it first - guard here so --decimate 0 throws before any division.
std::size_t checked_decimation(std::size_t decimation) {
  if (decimation < 1)
    throw std::invalid_argument{"decimation must be >= 1"};
  return decimation;
}

EnvelopeStream stream_envelope(const LoadedRecording &loaded, const EnvelopeOptions &opts,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples) {
  EnvelopeStream result;
  result.rate_hz = loaded.sample_rate_hz / static_cast<double>(checked_decimation(opts.decimation));
  result.carrier_hz = loaded.vision_carrier_hz;

  auto fe = make_front_end(loaded.sample_rate_hz, loaded.vision_carrier_hz, opts, block_samples, result.warnings);
  stream_real_blocks(
      loaded.data_path, loaded.format, [&](std::span<const float> x) { on_block(fe.process(x)); }, block_samples);
  if (fe.saw && opts.detector == demod::Detector::quasi_sync)
    result.afc_offset_hz = fe.saw->afc_offset_hz();
  return result;
}

EnvelopeStream stream_envelope_live(double sample_rate_hz, double carrier_hz, const EnvelopeOptions &opts,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples) {
  EnvelopeStream result;
  result.rate_hz = sample_rate_hz / static_cast<double>(checked_decimation(opts.decimation));

  // The carrier is the input stage's job: the tuner places the vision
  // carrier at its IF plan's target (live_view.py's tune arithmetic), exactly
  // as a real set's channel preset does, and the AFC absorbs the drift from
  // there. No set scans the aether at switch-on - a source that has drifted
  // past the catch range is retuned at the tuner, not hunted for here.
  if (opts.input != InputMode::composite && !(carrier_hz > 0.0))
    throw std::runtime_error{
        "live decode needs --carrier: the tuner's IF-plan target (the channel preset). The AFC pulls in the drift."};
  result.carrier_hz = carrier_hz;

  auto fe = make_front_end(sample_rate_hz, result.carrier_hz, opts, block_samples, result.warnings);
  const auto feed = [&](std::span<const float> x) { on_block(fe.process(x)); };
  stream_real_stdin(opts.sample_format, feed, block_samples);
  if (fe.saw && opts.detector == demod::Detector::quasi_sync)
    result.afc_offset_hz = fe.saw->afc_offset_hz();
  return result;
}

} // namespace palindrome::cli
