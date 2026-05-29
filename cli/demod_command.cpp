#include "demod_command.hpp"

#include "cli_util.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <print>
#include <ranges>
#include <string>

namespace palindrome::cli {

void DemodCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("demod", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("AM-demodulate a recording's vision carrier to a WAV for inspection")
          .add_argument(lyra::opt(output_, "file")["-o"]["--output"]("Output WAV (default: <recording>.wav)"))
          .add_argument(lyra::opt(carrier_, "hz")["--carrier"]("Carrier Hz (default: rx888:vision_if_hz)"))
          .add_argument(lyra::opt(cutoff_, "hz")["--cutoff"]("Baseband low-pass cutoff Hz"))
          .add_argument(lyra::opt(decimate_, "n")["--decimate"]("Keep 1 output sample per N inputs"))
          .add_argument(lyra::opt(slowdown_, "factor")["--slowdown"]("Stamp WAV rate as output/factor"))
          .add_argument(lyra::opt(no_sound_trap_)["--no-sound-trap"]("Disable the sound-carrier notch"))
          .add_argument(lyra::opt(sound_q_, "q")["--sound-q"]("Sound-trap notch Q"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to demodulate (e.g. corpus/alex_kidd)")));
}

int DemodCommand::run() const {
  const auto loaded = load_real_int16_recording(recording_, carrier_);

  demod::VisionChainConfig cfg{
      .sample_rate_hz = loaded.sample_rate_hz,
      .vision_carrier_hz = loaded.vision_carrier_hz,
      .sound_trap_hz = no_sound_trap_ ? std::optional<double>{} : loaded.meta.field<double>("rx888:sound_if_hz"),
      .sound_q = sound_q_,
      .cutoff_hz = cutoff_,
      .decimation = decimate_,
  };
  auto vision = demod::build_vision_chain(cfg);
  for (const auto &w: vision.warnings)
    std::println(std::cerr, "demod: warning: {}", w);

  auto out = stream_int16le_through_chain(loaded.data_path, vision.chain);
  if (out.empty()) {
    std::println(std::cerr, "demod: no samples read from {}", loaded.data_path.string());
    return 1;
  }

  // Normalise and invert to ~±0.9 full scale for comfortable viewing.
  const float peak = std::ranges::max(out, {}, [](float v) { return std::abs(v); });
  if (peak > 0.0f)
    std::ranges::transform(out, out.begin(), [scale = 0.9f / peak](float v) { return 1.f - v * scale; });

  auto output = output_;
  if (output.empty())
    output = std::format("{}.wav", loaded.meta_path.stem().string());
  // Real output rate is the input rate after decimation; the slowdown then maps
  // that to the stamped WAV rate, so the Audacity timeline is invariant to N.
  const double output_rate = loaded.sample_rate_hz / static_cast<double>(decimate_);
  const auto wav_rate = static_cast<std::uint32_t>(output_rate / slowdown_);
  wav::write_mono_float(output, out, wav_rate);

  std::string traps_desc = vision.trap_notes.empty() ? "no traps" : "traps: ";
  for (std::size_t t = 0; t < vision.trap_notes.size(); ++t)
    traps_desc += (t ? ", " : "") + vision.trap_notes[t];
  std::println(
      "wrote {} ({} samples @ {} Hz = real/{:g} after /{} decimation); carrier {:.4f} MHz, cutoff {:g} MHz; {}",
      output.string(), out.size(), wav_rate, slowdown_, decimate_, loaded.vision_carrier_hz / 1e6, cutoff_ / 1e6,
      traps_desc);
  return 0;
}

} // namespace palindrome::cli
