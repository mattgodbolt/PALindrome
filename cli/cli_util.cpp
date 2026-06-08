#include "cli_util.hpp"

#include "palindrome/demod.hpp"
#include "palindrome/fir.hpp"

#include <cstdint>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace palindrome::cli {

std::filesystem::path resolve_meta(std::filesystem::path path) {
  if (path.extension() != ".sigmf-meta")
    path.replace_extension(".sigmf-meta");
  return path;
}

LoadedRecording load_recording(const std::filesystem::path &recording, double carrier_override) {
  LoadedRecording loaded;
  loaded.meta_path = resolve_meta(recording);
  loaded.meta = sigmf::load(loaded.meta_path); // ParseError derives from runtime_error
  loaded.data_path = sigmf::data_path_for(loaded.meta_path);

  if (!loaded.meta.global.sample_rate)
    throw std::runtime_error{"recording has no core:sample_rate"};
  loaded.sample_rate_hz = *loaded.meta.global.sample_rate;

  const auto &dt = loaded.meta.global.parsed_datatype;
  if (dt.format != sigmf::DataType::Format::SignedInt || dt.bits != 16)
    throw std::runtime_error{std::format("only int16 input is supported (got {})", dt)};
  if (dt.complex)
    throw std::runtime_error{"complex (ci16) input is no longer supported — recapture as real ri16 "
                             "(the AirSpy now uses raw 20 MS/s real mode; see tools/capture_airspy.py)"};

  // Real IF, vision carrier an absolute IF frequency: the RX888 (rx888:*), or an
  // AirSpy raw 20 MS/s capture (airspy:*). Both decode through the same analytic
  // front end (see stream_envelope).
  if (carrier_override > 0.0)
    loaded.vision_carrier_hz = carrier_override;
  else if (const auto rx = loaded.meta.field<double>("rx888:vision_if_hz"))
    loaded.vision_carrier_hz = *rx;
  else if (const auto air = loaded.meta.field<double>("airspy:vision_if_hz"))
    loaded.vision_carrier_hz = *air;
  else
    throw std::runtime_error{"no rx888:vision_if_hz or airspy:vision_if_hz in metadata; pass --carrier"};

  return loaded;
}

void stream_ri16le_blocks(const std::filesystem::path &data_path,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples) {
  std::ifstream data{data_path, std::ios::binary};
  if (!data)
    throw std::runtime_error{std::format("cannot open data file: {}", data_path.string())};

  std::vector<std::int16_t> raw(block_samples);
  std::vector<float> block(block_samples);
  while (data) {
    data.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(std::int16_t)));
    const auto got = static_cast<std::size_t>(data.gcount()) / sizeof(std::int16_t);
    if (got == 0)
      break;
    const std::span<float> dst{block.data(), got};
    for (std::size_t k = 0; k < got; ++k)
      dst[k] = static_cast<float>(raw[k]) * (1.0f / 32768.0f);
    on_block(dst);
  }
}

EnvelopeStream stream_envelope(const LoadedRecording &loaded, const EnvelopeOptions &opts,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples) {
  EnvelopeStream result;
  result.rate_hz = loaded.sample_rate_hz / static_cast<double>(opts.decimation);

  // Real IF (RX888, or an AirSpy raw 20 MS/s capture): form the analytic
  // (one-sided) signal so the carrier's negative-frequency image can't fold onto
  // the chroma, then mix the vision carrier to DC, low-pass and take the
  // magnitude. The sound carrier (vision + 6 MHz) lands above the chroma after
  // the mix and is removed by the cutoff low-pass, so no separate sound trap is
  // needed (confirmed on the RX888 corpus, whose old strip had one).
  demod::Hilbert hilbert{demod::kDefaultVisionTaps};
  demod::ComplexAmEnvelope env_demod{loaded.sample_rate_hz, loaded.vision_carrier_hz, opts.cutoff_hz,
      demod::kDefaultVisionTaps, dsp::Window::Hamming, opts.decimation};
  hilbert.prepare(block_samples);
  env_demod.prepare(block_samples);
  if (const auto decimated_nyquist = loaded.sample_rate_hz / (2.0 * static_cast<double>(opts.decimation));
      opts.cutoff_hz >= decimated_nyquist)
    result.warnings.push_back(std::format("cutoff {:g} MHz exceeds the decimated Nyquist {:g} MHz; expect aliasing",
        opts.cutoff_hz / 1e6, decimated_nyquist / 1e6));

  stream_ri16le_blocks(
      loaded.data_path, [&](std::span<const float> x) { on_block(env_demod.process(hilbert.process(x))); },
      block_samples);
  return result;
}

} // namespace palindrome::cli
