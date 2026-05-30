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
          .add_argument(lyra::opt(lines_, "n")["--lines"]("Rows to render"))
          .add_argument(lyra::opt(start_line_, "n")["--start-line"]("Skip this many locked lines before the top row"))
          .add_argument(lyra::opt(no_sound_trap_)["--no-sound-trap"]("Disable the sound-carrier notch"))
          .add_argument(lyra::opt(sound_q_, "q")["--sound-q"]("Sound-trap notch Q"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to render (e.g. corpus/alex_kidd)")));
}

int RenderCommand::run() const {
  if (width_ == 0 || lines_ == 0) {
    std::println(std::cerr, "render: --width and --lines must be positive");
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

  // The branching video graph: the demod envelope fans out to the sync
  // separator (timing rail) and the renderer (picture rail), rejoining in the
  // renderer. All three are streaming nodes; we pump the recording through in
  // blocks, so nothing ever materialises the whole envelope.
  video::SyncSeparator separator{video::SyncSeparatorConfig{.sample_rate_hz = envelope_rate}};
  video::HorizontalSweep sweep{video::HorizontalSweepConfig{.sample_rate_hz = envelope_rate}};
  video::FrameRenderer renderer{
      video::FrameRendererConfig{.width = width_, .lines = lines_, .start_line = start_line_}};

  constexpr std::size_t kBlock = std::size_t{1} << 16;
  const std::size_t env_block = vision.chain.max_output_for(kBlock);
  separator.prepare(env_block);
  sweep.prepare(env_block);

  stream_blocks_through_chain(
      loaded.data_path, vision.chain,
      [&](std::span<const float> env) {
        const std::span<const video::SyncSample> sync = separator.process(env); // timing rail
        const std::span<const video::BeamSample> beam = sweep.process(sync);
        renderer.process(env, beam); // join: picture rail (env) + timing rail (beam)
      },
      kBlock);

  const auto frame = renderer.finish();
  if (frame.total_locked_lines == 0) {
    std::println(std::cerr, "render: the sweep never locked a line — nothing to draw");
    return 1;
  }

  auto output = output_;
  if (output.empty())
    output = std::format("{}.png", loaded.meta_path.stem().string());
  image::write_png_grey(output, frame.grey, static_cast<unsigned>(frame.width), static_cast<unsigned>(frame.lines));

  const double locked_line_hz = sweep.omega() * envelope_rate;
  std::println("wrote {} ({}x{}); envelope @ {:g} MS/s after /{} decimation, carrier {:.4f} MHz; "
               "sweep accepted {} edges (rejected {}), locked to {:.2f} Hz ({:+.2f}% from nominal); "
               "painted {} of {} locked lines (skipped {})",
      output.string(), frame.width, frame.lines, envelope_rate / 1e6, decimate_, loaded.vision_carrier_hz / 1e6,
      sweep.accepted_edges(), sweep.rejected_edges(), locked_line_hz, 100.0 * (locked_line_hz - 15625.0) / 15625.0,
      frame.painted_lines, frame.total_locked_lines, start_line_);
  return 0;
}

} // namespace palindrome::cli
