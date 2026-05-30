#include "render_command.hpp"

#include "cli_util.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/image.hpp"
#include "palindrome/video.hpp"

#include <cstddef>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <print>
#include <span>

namespace palindrome::cli {

void RenderCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("render", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("Render a recording's vision signal to a PNG using a sync-locked horizontal flywheel")
          .add_argument(lyra::opt(output_, "file")["-o"]["--output"]("Output PNG (default: <recording>.png)"))
          .add_argument(lyra::opt(carrier_, "hz")["--carrier"]("Carrier Hz (default: rx888:vision_if_hz)"))
          .add_argument(lyra::opt(cutoff_, "hz")["--cutoff"]("Baseband low-pass cutoff Hz"))
          .add_argument(lyra::opt(decimate_, "n")["--decimate"]("Keep 1 sample per N inputs"))
          .add_argument(lyra::opt(width_, "px")["--width"]("Output image width"))
          .add_argument(lyra::opt(height_, "px")["--height"]("Output image height"))
          .add_argument(lyra::opt(no_sound_trap_)["--no-sound-trap"]("Disable the sound-carrier notch"))
          .add_argument(lyra::opt(sound_q_, "q")["--sound-q"]("Sound-trap notch Q"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to render (e.g. corpus/alex_kidd)")));
}

int RenderCommand::run() const {
  if (width_ == 0 || height_ == 0) {
    std::println(std::cerr, "render: --width and --height must be positive");
    return 1;
  }

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

  const double envelope_rate = loaded.sample_rate_hz / static_cast<double>(decimate_);

  // The whole video graph as one composite node: it fans the separator's sync
  // bit to the horizontal sweep and vertical sync, then joins both timebases
  // with the picture rail at the phosphor screen. We pump the recording through
  // in blocks, so nothing ever materialises the whole envelope.
  video::Decoder decoder{video::DecoderConfig{.sample_rate_hz = envelope_rate, .width = width_, .height = height_}};

  constexpr std::size_t kBlock = std::size_t{1} << 16;
  decoder.prepare(vision.chain.max_output_for(kBlock));

  stream_blocks_through_chain(
      loaded.data_path, vision.chain, [&](std::span<const float> env) { decoder.process(env); }, kBlock);

  if (decoder.accepted_edges() == 0 || decoder.detected_fields() == 0) {
    std::println(std::cerr, "render: never locked ({} line edges, {} fields) — nothing to draw",
        decoder.accepted_edges(), decoder.detected_fields());
    return 1;
  }

  const auto frame = decoder.snapshot();
  auto output = output_;
  if (output.empty())
    output = std::format("{}.png", loaded.meta_path.stem().string());
  image::write_png_grey(output, frame.grey, static_cast<unsigned>(frame.width), static_cast<unsigned>(frame.height));

  const double line_hz = decoder.line_omega() * envelope_rate;
  const double field_hz = decoder.field_omega() * envelope_rate;
  std::println("wrote {} ({}x{}); envelope @ {:g} MS/s after /{} decimation, carrier {:.4f} MHz; "
               "horizontal locked {} edges @ {:.1f} Hz ({:+.2f}%); vertical locked {} fields @ {:.2f} Hz ({:+.2f}%)",
      output.string(), frame.width, frame.height, envelope_rate / 1e6, decimate_, loaded.vision_carrier_hz / 1e6,
      decoder.accepted_edges(), line_hz, 100.0 * (line_hz - 15625.0) / 15625.0, decoder.detected_fields(), field_hz,
      100.0 * (field_hz - 50.0) / 50.0);
  return 0;
}

} // namespace palindrome::cli
