#include "demod_command.hpp"

#include "cli_util.hpp"
#include "palindrome/biquad.hpp"
#include "palindrome/dc_blocker.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/sigmf.hpp"
#include "palindrome/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace palindrome::cli {

namespace {
namespace sigmf = palindrome::sigmf;
namespace demod = palindrome::demod;
namespace wav = palindrome::wav;

// A streaming filter plus the scratch buffer it fills.
template<typename Filter>
struct Stage {
  Filter filter;
  std::vector<float> buf;

  std::span<const float> process(std::span<const float> in) {
    buf.clear();
    filter.process(in, buf);
    return buf;
  }
};
} // namespace

void DemodCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("demod", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("AM-demodulate a recording's vision carrier to a WAV for inspection")
          .add_argument(lyra::opt(output_, "file")["-o"]["--output"]("Output WAV (default: <recording>.wav)"))
          .add_argument(lyra::opt(carrier_, "hz")["--carrier"]("Carrier Hz (default: rx888:vision_if_hz)"))
          .add_argument(lyra::opt(cutoff_, "hz")["--cutoff"]("Baseband low-pass cutoff Hz (default 5e6)"))
          .add_argument(lyra::opt(slowdown_, "factor")["--slowdown"]("Stamp WAV rate as real/factor (default 1000)"))
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

  // IF traps applied (in order) before detection, mirroring a TV's IF strip.
  std::vector<Stage<dsp::Biquad>> traps;
  std::vector<std::string> trap_notes;

  // The FM sound carrier.
  if (!no_sound_trap_) {
    if (const auto s = meta.field<double>("rx888:sound_if_hz")) {
      traps.push_back({dsp::notch(fs, *s, sound_q_), {}});
      trap_notes.push_back(std::format("sound {:.3f} MHz (Q {:g})", *s / 1e6, sound_q_));
    }
  }

  // Remove any constant ADC offset before mixing, so it can't beat into the
  // envelope at the carrier frequency.
  Stage<dsp::DcBlocker> dc_blocker;

  const auto data_path = sigmf::data_path_for(meta_path);
  std::ifstream data{data_path, std::ios::binary};
  if (!data) {
    std::println(std::cerr, "demod: cannot open data file: {}", data_path.string());
    return 1;
  }

  demod::AmEnvelope envelope{fs, carrier, cutoff_};
  std::vector<float> out;
  std::vector<std::int16_t> raw(std::size_t{1} << 16);
  std::vector<float> block;
  while (data) {
    data.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(std::int16_t)));
    const auto got = static_cast<std::size_t>(data.gcount()) / sizeof(std::int16_t);
    if (got == 0)
      break;
    block.resize(got);
    for (std::size_t k = 0; k < got; ++k)
      block[k] = static_cast<float>(raw[k]) / 32768.0f;

    std::span<const float> stage{block};
    for (auto &trap: traps)
      stage = trap.process(stage);
    stage = dc_blocker.process(stage);
    envelope.process(stage, out);
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
  const auto wav_rate = static_cast<std::uint32_t>(fs / slowdown_);
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
  std::println("wrote {} ({} samples @ {} Hz = real/{:g}); carrier {:.4f} MHz, cutoff {:g} MHz; {}", output.string(),
      out.size(), wav_rate, slowdown_, carrier / 1e6, cutoff_ / 1e6, traps_desc);
  return 0;
}

} // namespace palindrome::cli
