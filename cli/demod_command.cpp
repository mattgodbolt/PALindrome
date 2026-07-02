#include "demod_command.hpp"

#include "cli_util.hpp"
#include "palindrome/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <print>
#include <ranges>
#include <span>
#include <vector>

namespace palindrome::cli {

void DemodCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("demod", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("AM-demodulate a recording's vision carrier to a WAV for inspection")
          .add_argument(lyra::opt(output_, "file")["-o"]["--output"]("Output WAV (default: <recording>.wav)"))
          .add_argument(lyra::opt(carrier_, "hz")["--carrier"](
              "Carrier Hz (default: the metadata's vision_if_hz, or a signal scan if it has none)"))
          .add_argument(lyra::opt(cutoff_, "hz")["--cutoff"]("Baseband low-pass cutoff Hz"))
          .add_argument(lyra::opt(decimate_, "n")["--decimate"]("Keep 1 output sample per N inputs"))
          .add_argument(lyra::opt(slowdown_, "factor")["--slowdown"]("Stamp WAV rate as output/factor"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to demodulate (e.g. corpus/alex_kidd)")));
}

int DemodCommand::run() const {
  if (slowdown_ <= 0.0) {
    std::println(std::cerr, "demod: --slowdown must be positive (got {:g})", slowdown_);
    return 1;
  }
  const auto loaded = load_recording(recording_, carrier_);

  // The same composite envelope render/sync see, accumulated for the WAV dump.
  const EnvelopeOptions opts{.cutoff_hz = cutoff_, .decimation = decimate_};
  std::vector<float> out;
  const auto es =
      stream_envelope(loaded, opts, [&](std::span<const float> e) { out.insert(out.end(), e.begin(), e.end()); });
  for (const auto &w: es.warnings)
    std::println(std::cerr, "demod: warning: {}", w);
  if (out.empty()) {
    std::println(std::cerr, "demod: no samples read from {}", loaded.data_path.string());
    return 1;
  }

  // Normalise and invert to ~±0.9 full scale for comfortable viewing.
  const auto peak = std::ranges::max(out, {}, [](float v) { return std::abs(v); });
  if (peak > 0.0f)
    std::ranges::transform(out, out.begin(), [scale = 0.9f / peak](float v) { return 1.f - v * scale; });

  auto output = output_;
  if (output.empty())
    output = std::format("{}.wav", loaded.meta_path.stem().string());
  // The slowdown maps the envelope rate to the stamped WAV rate, so the Audacity
  // timeline is invariant to the decimation N.
  const auto wav_rate = static_cast<std::uint32_t>(es.rate_hz / slowdown_);
  wav::write_mono_float(output, out, wav_rate);

  std::println("wrote {} ({} samples @ {} Hz = real/{:g} after /{} decimation); carrier {:.4f} MHz, cutoff {:g} MHz",
      output.string(), out.size(), wav_rate, slowdown_, decimate_, loaded.vision_carrier_hz / 1e6, cutoff_ / 1e6);
  return 0;
}

} // namespace palindrome::cli
