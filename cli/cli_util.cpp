#include "cli_util.hpp"

#include "palindrome/demod.hpp"
#include "palindrome/fir.hpp"

#include <complex>
#include <cstdint>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>

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
  loaded.complex_baseband = dt.complex;

  if (loaded.complex_baseband) {
    // AirSpy: complex baseband, vision carrier a small offset from DC.
    if (carrier_override != 0.0)
      loaded.vision_offset_hz = carrier_override;
    else if (const auto v = loaded.meta.field<double>("airspy:vision_offset_hz"))
      loaded.vision_offset_hz = *v;
    else
      throw std::runtime_error{"no airspy:vision_offset_hz in metadata; pass --carrier (offset from DC)"};
  }
  else {
    // RX888: real IF, vision carrier an absolute IF frequency.
    if (carrier_override > 0.0)
      loaded.vision_carrier_hz = carrier_override;
    else if (const auto v = loaded.meta.field<double>("rx888:vision_if_hz"))
      loaded.vision_carrier_hz = *v;
    else
      throw std::runtime_error{"no rx888:vision_if_hz in metadata; pass --carrier"};
  }

  return loaded;
}

void stream_blocks_through_chain(const std::filesystem::path &data_path, Chain &chain,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples) {
  std::ifstream data{data_path, std::ios::binary};
  if (!data)
    throw std::runtime_error{std::format("cannot open data file: {}", data_path.string())};

  chain.prepare(block_samples);

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
    on_block(chain.process(dst));
  }
}

std::vector<float> stream_int16le_through_chain(
    const std::filesystem::path &data_path, Chain &chain, std::size_t block_samples) {
  // Reserve from the file's input-sample count run through the chain's bound,
  // so the accumulating loop never reallocates the output vector.
  std::vector<float> out;
  std::error_code size_ec;
  if (const auto bytes = std::filesystem::file_size(data_path, size_ec); !size_ec)
    out.reserve(chain.max_output_for(bytes / sizeof(std::int16_t)) + 1);

  stream_blocks_through_chain(
      data_path, chain, [&out](std::span<const float> blk) { out.insert(out.end(), blk.begin(), blk.end()); },
      block_samples);
  return out;
}

void stream_ci16le_blocks(const std::filesystem::path &data_path,
    const std::function<void(std::span<const std::complex<float>>)> &on_block, std::size_t block_samples) {
  std::ifstream data{data_path, std::ios::binary};
  if (!data)
    throw std::runtime_error{std::format("cannot open data file: {}", data_path.string())};

  std::vector<std::int16_t> raw(block_samples * 2); // interleaved I, Q
  std::vector<std::complex<float>> block(block_samples);
  while (data) {
    data.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(std::int16_t)));
    const auto ints = static_cast<std::size_t>(data.gcount()) / sizeof(std::int16_t);
    const std::size_t got = ints / 2; // whole complex samples (a trailing lone int16 is dropped)
    if (got == 0)
      break;
    const std::span<std::complex<float>> dst{block.data(), got};
    for (std::size_t k = 0; k < got; ++k)
      dst[k] = {
          static_cast<float>(raw[2 * k]) * (1.0f / 32768.0f), static_cast<float>(raw[2 * k + 1]) * (1.0f / 32768.0f)};
    on_block(dst);
  }
}

EnvelopeStream stream_envelope(const LoadedRecording &loaded, const EnvelopeOptions &opts,
    const std::function<void(std::span<const float>)> &on_block, std::size_t block_samples) {
  EnvelopeStream result;
  result.rate_hz = loaded.sample_rate_hz / static_cast<double>(opts.decimation);

  if (loaded.complex_baseband) {
    // AirSpy: complex baseband. Mix the vision offset to DC, low-pass, magnitude.
    demod::ComplexAmEnvelope env_demod{loaded.sample_rate_hz, loaded.vision_offset_hz, opts.cutoff_hz,
        demod::kDefaultVisionTaps, dsp::Window::Hamming, opts.decimation};
    env_demod.prepare(block_samples);
    stream_ci16le_blocks(
        loaded.data_path, [&](std::span<const std::complex<float>> iq) { on_block(env_demod.process(iq)); },
        block_samples);
  }
  else {
    // RX888: real IF. The full vision strip (sound trap, DC block, AM envelope).
    demod::VisionChainConfig cfg{
        .sample_rate_hz = loaded.sample_rate_hz,
        .vision_carrier_hz = loaded.vision_carrier_hz,
        .sound_trap_hz = opts.no_sound_trap ? std::optional<double>{} : loaded.meta.field<double>("rx888:sound_if_hz"),
        .sound_q = opts.sound_q,
        .cutoff_hz = opts.cutoff_hz,
        .decimation = opts.decimation,
    };
    auto vision = demod::build_vision_chain(cfg);
    result.warnings = std::move(vision.warnings);
    stream_blocks_through_chain(loaded.data_path, vision.chain, on_block, block_samples);
  }
  return result;
}

} // namespace palindrome::cli
