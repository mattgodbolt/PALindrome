#include "demod_command.hpp"

#include "cli_util.hpp"
#include "palindrome/biquad.hpp"
#include "palindrome/dc_blocker.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/pipeline.hpp"
#include "palindrome/sigmf.hpp"
#include "palindrome/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace palindrome::cli {

namespace {
namespace sigmf = palindrome::sigmf;
namespace demod = palindrome::demod;
namespace wav = palindrome::wav;
} // namespace

void DemodCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("demod", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("AM-demodulate a recording's vision carrier to a WAV for inspection")
          .add_argument(lyra::opt(output_, "file")["-o"]["--output"]("Output WAV (default: <recording>.wav)"))
          .add_argument(lyra::opt(carrier_, "hz")["--carrier"]("Carrier Hz (default: rx888:vision_if_hz)"))
          .add_argument(lyra::opt(cutoff_, "hz")["--cutoff"]("Baseband low-pass cutoff Hz (default 5e6)"))
          .add_argument(lyra::opt(decimate_, "n")["--decimate"]("Keep 1 output sample per N inputs (default 1)"))
          .add_argument(lyra::opt(slowdown_, "factor")["--slowdown"]("Stamp WAV rate as output/factor (default 1000)"))
          .add_argument(lyra::opt(no_sound_trap_)["--no-sound-trap"]("Disable the sound-carrier notch"))
          .add_argument(lyra::opt(sound_q_, "q")["--sound-q"]("Sound-trap notch Q (default 10)"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to demodulate (e.g. corpus/alex_kidd)")));
}

int DemodCommand::run() const {
  const auto meta_path = resolve_meta(recording_);
  sigmf::Metadata meta;
  try {
    meta = sigmf::load(meta_path);
  }
  catch (const sigmf::ParseError &e) {
    std::println(std::cerr, "demod: {}", e.what());
    return 1;
  }

  if (!meta.global.sample_rate) {
    std::println(std::cerr, "demod: recording has no core:sample_rate");
    return 1;
  }
  const double fs = *meta.global.sample_rate;

  const auto &dt = meta.global.parsed_datatype;
  if (dt.complex || dt.format != sigmf::DataType::Format::SignedInt || dt.bits != 16) {
    std::println(std::cerr, "demod: only real int16 input is supported (got {})", dt);
    return 1;
  }

  double carrier = carrier_;
  if (carrier <= 0.0) {
    if (const auto v = meta.field<double>("rx888:vision_if_hz"))
      carrier = *v;
    else {
      std::println(std::cerr, "demod: no rx888:vision_if_hz in metadata; pass --carrier");
      return 1;
    }
  }

  if (decimate_ < 1) {
    std::println(std::cerr, "demod: --decimate must be >= 1");
    return 1;
  }
  // Decimation folds anything above the decimated Nyquist back into band; the
  // low-pass must clear it. Warn rather than fail — it's a quality call.
  if (const double decimated_nyquist = fs / (2.0 * static_cast<double>(decimate_)); cutoff_ >= decimated_nyquist)
    std::println(std::cerr, "demod: warning: cutoff {:g} MHz exceeds the decimated Nyquist {:g} MHz; expect aliasing",
        cutoff_ / 1e6, decimated_nyquist / 1e6);

  // The decode pipeline, mirroring a TV's IF strip: optional IF traps, then a
  // DC blocker, then the AM envelope detector. Each stage owns its buffers; the
  // Chain sizes them once (prepare, below) and pipes each block straight through.
  palindrome::Chain chain;
  std::vector<std::string> trap_notes;

  // The FM sound carrier trap.
  if (!no_sound_trap_) {
    if (const auto s = meta.field<double>("rx888:sound_if_hz")) {
      chain.add(dsp::notch(fs, *s, sound_q_));
      trap_notes.push_back(std::format("sound {:.3f} MHz (Q {:g})", *s / 1e6, sound_q_));
    }
  }

  // Remove any constant ADC offset before mixing, so it can't beat into the
  // envelope at the carrier frequency.
  chain.add(dsp::DcBlocker{});
  chain.add(demod::AmEnvelope{fs, carrier, cutoff_, 127, dsp::Window::Hamming, decimate_});

  const auto data_path = sigmf::data_path_for(meta_path);
  std::ifstream data{data_path, std::ios::binary};
  if (!data) {
    std::println(std::cerr, "demod: cannot open data file: {}", data_path.string());
    return 1;
  }

  // Read in fixed-size blocks; sizing the whole chain's storage once up front
  // means the streaming loop never allocates inside a stage.
  constexpr std::size_t read_samples = std::size_t{1} << 16;
  chain.prepare(read_samples);

  std::vector<float> out;
  // One output sample per `decimate_` input int16s. Reserve the whole lot up
  // front: `out` accumulates across every block, so without this each append
  // would eventually reallocate and recopy all prior samples.
  std::error_code size_ec;
  if (const auto bytes = std::filesystem::file_size(data_path, size_ec); !size_ec)
    out.reserve(bytes / sizeof(std::int16_t) / decimate_ + 1);
  std::vector<std::int16_t> raw(read_samples);
  std::vector<float> block;
  while (data) {
    data.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(std::int16_t)));
    const auto got = static_cast<std::size_t>(data.gcount()) / sizeof(std::int16_t);
    if (got == 0)
      break;
    block.resize(got);
    for (std::size_t k = 0; k < got; ++k)
      block[k] = static_cast<float>(raw[k]) / 32768.0f;

    const std::span<const float> demodulated = chain.process(block);
    out.insert(out.end(), demodulated.begin(), demodulated.end());
  }
  if (out.empty()) {
    std::println(std::cerr, "demod: no samples read from {}", data_path.string());
    return 1;
  }

  // Normalise and invert to ~±0.9 full scale for comfortable viewing.
  const float peak = std::ranges::max(out, {}, [](float v) { return std::abs(v); });
  if (peak > 0.0f)
    std::ranges::transform(out, out.begin(), [scale = 0.9f / peak](float v) { return 1.f - v * scale; });

  auto output = output_;
  if (output.empty())
    output = std::format("{}.wav", meta_path.stem().string());
  // Real output rate is the input rate after decimation; the slowdown then maps
  // that to the stamped WAV rate, so the Audacity timeline is invariant to N.
  const double output_rate = fs / static_cast<double>(decimate_);
  const auto wav_rate = static_cast<std::uint32_t>(output_rate / slowdown_);
  try {
    wav::write_mono_float(output, out, wav_rate);
  }
  catch (const wav::WriteError &e) {
    std::println(std::cerr, "demod: {}", e.what());
    return 1;
  }

  std::string traps_desc = trap_notes.empty() ? "no traps" : "traps: ";
  for (std::size_t t = 0; t < trap_notes.size(); ++t)
    traps_desc += (t ? ", " : "") + trap_notes[t];
  std::println(
      "wrote {} ({} samples @ {} Hz = real/{:g} after /{} decimation); carrier {:.4f} MHz, cutoff {:g} MHz; {}",
      output.string(), out.size(), wav_rate, slowdown_, decimate_, carrier / 1e6, cutoff_ / 1e6, traps_desc);
  return 0;
}

} // namespace palindrome::cli
