#include "render_command.hpp"

#include "cli_util.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/image.hpp"
#include "palindrome/video.hpp"

#include <algorithm>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <print>
#include <span>
#include <vector>

namespace palindrome::cli {

void RenderCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("render", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("Render a recording's vision signal to a PNG using a sync-locked horizontal flywheel")
          .add_argument(lyra::opt(output_, "file")["-o"]["--output"]("Output PNG (default: <recording>.png)"))
          .add_argument(lyra::opt(carrier_, "hz")["--carrier"]("Carrier Hz (default: rx888:vision_if_hz)"))
          .add_argument(lyra::opt(cutoff_, "hz")["--cutoff"]("Baseband low-pass cutoff Hz"))
          .add_argument(lyra::opt(sync_cutoff_, "hz")["--sync-cutoff"]("Sync-branch low-pass cutoff Hz"))
          .add_argument(lyra::opt(decimate_, "n")["--decimate"]("Keep 1 sample per N inputs"))
          .add_argument(lyra::opt(width_, "px")["--width"]("Output image width"))
          .add_argument(lyra::opt(height_, "px")["--height"]("Output image height"))
          .add_argument(lyra::opt(no_sound_trap_)["--no-sound-trap"]("Disable the sound-carrier notch"))
          .add_argument(lyra::opt(sound_q_, "q")["--sound-q"]("Sound-trap notch Q"))
          .add_argument(lyra::opt(no_sync_)["--no-sync"]("Debug: naive-fold the envelope, bypassing sync"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to render (e.g. corpus/alex_kidd)")));
}

int RenderCommand::run() const {
  if (width_ == 0 || height_ == 0) {
    std::println(std::cerr, "render: --width and --height must be positive");
    return 1;
  }

  const auto loaded = load_recording(recording_, carrier_);

  const double envelope_rate = loaded.sample_rate_hz / static_cast<double>(decimate_);

  const EnvelopeOptions opts{
      .cutoff_hz = cutoff_, .decimation = decimate_, .no_sound_trap = no_sound_trap_, .sound_q = sound_q_};

  if (no_sync_) {
    // Debug: fold the raw envelope into the frame (sample i -> x = i % width,
    // y = i / width), bypassing the sync graph. Set --width to samples/line to
    // straighten it. Shows whether the picture is in the envelope at all.
    std::vector<float> env;
    stream_envelope(loaded, opts, [&](std::span<const float> e) { env.insert(env.end(), e.begin(), e.end()); });
    if (env.empty()) {
      std::println(std::cerr, "render: no samples read from {}", loaded.data_path.string());
      return 1;
    }
    const std::size_t n = std::min(env.size(), width_ * height_);
    float hi = env[0];
    float lo = env[0];
    for (std::size_t i = 0; i < n; ++i) {
      hi = std::max(hi, env[i]);
      lo = std::min(lo, env[i]);
    }
    const float span = hi > lo ? hi - lo : 1.0f;
    std::vector<std::uint8_t> grey(width_ * height_, 0);
    for (std::size_t i = 0; i < n; ++i)
      grey[i] = static_cast<std::uint8_t>(std::clamp((hi - env[i]) / span, 0.0f, 1.0f) * 255.0f + 0.5f);
    std::filesystem::path out = output_;
    if (out.empty())
      out = std::format("{}.png", loaded.meta_path.stem().string());
    image::write_png_grey(out, grey, static_cast<unsigned>(width_), static_cast<unsigned>(height_));
    std::println("wrote {} ({}x{}, naive envelope fold, no sync)", out.string(), width_, height_);
    return 0;
  }

  // The whole video graph as one composite node: it fans the separator's sync
  // bit to the horizontal sweep and vertical sync, then joins both timebases
  // with the picture rail at the phosphor screen. We pump the recording through
  // in blocks, so nothing ever materialises the whole envelope.
  video::Decoder decoder{video::DecoderConfig{
      .sample_rate_hz = envelope_rate, .width = width_, .height = height_, .sync_lp_cutoff_hz = sync_cutoff_}};

  constexpr std::size_t kBlock = std::size_t{1} << 16;
  decoder.prepare(kBlock);

  // stream_envelope hides the real-IF vs complex-baseband front end; the decoder
  // just sees composite-envelope blocks.
  const auto es = stream_envelope(loaded, opts, [&](std::span<const float> env) { decoder.process(env); }, kBlock);
  for (const auto &w: es.warnings)
    std::println(std::cerr, "render: warning: {}", w);

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
