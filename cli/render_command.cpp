#include "render_command.hpp"

#include "cli_util.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <print>
#include <span>
#include <vector>

namespace palindrome::cli {

namespace {
// PAL line rate. The frame is 625 of these; at 32 MS/s a line is 2048 samples
// exactly, at 16 MS/s 1024 — but only nominally, hence the rolling hack output.
constexpr double kPalLineHz = 15625.0;
} // namespace

void RenderCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("render", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("Render a recording's vision signal to a PNG (naive fold, no sync lock yet)")
          .add_argument(lyra::opt(output_, "file")["-o"]["--output"]("Output PNG (default: <recording>.png)"))
          .add_argument(lyra::opt(carrier_, "hz")["--carrier"]("Carrier Hz (default: rx888:vision_if_hz)"))
          .add_argument(lyra::opt(cutoff_, "hz")["--cutoff"]("Baseband low-pass cutoff Hz"))
          .add_argument(lyra::opt(decimate_, "n")["--decimate"]("Keep 1 sample per N inputs"))
          .add_argument(lyra::opt(width_, "px")["--width"]("Image width (default: round(samples per line))"))
          .add_argument(lyra::opt(lines_, "n")["--lines"]("Rows to render"))
          .add_argument(lyra::opt(start_line_, "n")["--start-line"]("Skip this many lines before the top row"))
          .add_argument(lyra::opt(no_sound_trap_)["--no-sound-trap"]("Disable the sound-carrier notch"))
          .add_argument(lyra::opt(sound_q_, "q")["--sound-q"]("Sound-trap notch Q"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to render (e.g. corpus/alex_kidd)")));
}

int RenderCommand::run() const {
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
    std::println(std::cerr, "render: warning: {}", w);

  const auto env = stream_int16le_through_chain(loaded.data_path, vision.chain);
  if (env.empty()) {
    std::println(std::cerr, "render: no samples read from {}", loaded.data_path.string());
    return 1;
  }

  const double output_rate = loaded.sample_rate_hz / static_cast<double>(decimate_);
  const double samples_per_line = output_rate / kPalLineHz;
  const std::size_t width = width_ != 0 ? width_ : static_cast<std::size_t>(std::lround(samples_per_line));
  const auto start = static_cast<std::size_t>(std::lround(static_cast<double>(start_line_) * samples_per_line));
  if (width == 0) {
    std::println(std::cerr, "render: computed zero width");
    return 1;
  }
  if (start >= env.size()) {
    std::println(std::cerr, "render: --start-line {} is past the end of the recording", start_line_);
    return 1;
  }

  const std::size_t available_rows = (env.size() - start) / width;
  const std::size_t height = std::min(lines_, available_rows);
  if (height == 0) {
    std::println(std::cerr, "render: not enough samples for even one {}-wide row", width);
    return 1;
  }

  // Contrast-stretch the framed region: vision is negatively modulated, so the
  // envelope peaks at sync (blacker than black) and dips toward white. Map the
  // window's max -> 0 (black) and min -> 255 (white). This is a stand-in for the
  // proper sync-locked black-level clamp that comes with the levels stage.
  const std::span<const float> frame{env.data() + start, width * height};
  const auto [lo_it, hi_it] = std::ranges::minmax_element(frame);
  const float lo = *lo_it;
  const float hi = *hi_it;
  const float span = hi > lo ? hi - lo : 1.0f;

  std::vector<std::uint8_t> grey(width * height);
  std::ranges::transform(frame, grey.begin(), [hi, span](float v) {
    const float white = (hi - v) / span; // 1 at the darkest envelope, 0 at sync
    return static_cast<std::uint8_t>(std::clamp(white, 0.0f, 1.0f) * 255.0f + 0.5f);
  });

  auto output = output_;
  if (output.empty())
    output = std::format("{}.png", loaded.meta_path.stem().string());
  image::write_png_grey(output, grey, static_cast<unsigned>(width), static_cast<unsigned>(height));

  std::println("wrote {} ({}x{}, {:.2f} samples/line @ {:g} MS/s after /{} decimation); carrier {:.4f} MHz",
      output.string(), width, height, samples_per_line, output_rate / 1e6, decimate_, loaded.vision_carrier_hz / 1e6);
  return 0;
}

} // namespace palindrome::cli
