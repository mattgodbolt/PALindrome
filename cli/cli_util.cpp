#include "cli_util.hpp"

#include <cstdint>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <stdexcept>
#include <system_error>

namespace palindrome::cli {

std::filesystem::path resolve_meta(std::filesystem::path path) {
  if (path.extension() != ".sigmf-meta")
    path.replace_extension(".sigmf-meta");
  return path;
}

LoadedRecording load_real_int16_recording(const std::filesystem::path &recording, double carrier_override) {
  LoadedRecording loaded;
  loaded.meta_path = resolve_meta(recording);
  loaded.meta = sigmf::load(loaded.meta_path); // ParseError derives from runtime_error
  loaded.data_path = sigmf::data_path_for(loaded.meta_path);

  if (!loaded.meta.global.sample_rate)
    throw std::runtime_error{"recording has no core:sample_rate"};
  loaded.sample_rate_hz = *loaded.meta.global.sample_rate;

  const auto &dt = loaded.meta.global.parsed_datatype;
  if (dt.complex || dt.format != sigmf::DataType::Format::SignedInt || dt.bits != 16)
    throw std::runtime_error{std::format("only real int16 input is supported (got {})", dt)};

  if (carrier_override > 0.0) {
    loaded.vision_carrier_hz = carrier_override;
  }
  else if (const auto v = loaded.meta.field<double>("rx888:vision_if_hz")) {
    loaded.vision_carrier_hz = *v;
  }
  else {
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

} // namespace palindrome::cli
