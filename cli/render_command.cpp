#include "render_command.hpp"

#include "cli_util.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/image.hpp"
#include "palindrome/video.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
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

  const auto env = stream_int16le_through_chain(loaded.data_path, vision.chain);
  if (env.empty()) {
    std::println(std::cerr, "render: no samples read from {}", loaded.data_path.string());
    return 1;
  }

  const double envelope_rate = loaded.sample_rate_hz / static_cast<double>(decimate_);

  // envelope -> sync separator -> horizontal sweep. Each stage owns its output
  // buffer; the spans stay valid because we consume each before the next call.
  video::SyncSeparator separator{video::SyncSeparatorConfig{.sample_rate_hz = envelope_rate}};
  separator.prepare(env.size());
  const std::span<const video::SyncSample> sliced = separator.process(env);

  video::HorizontalSweep sweep{video::HorizontalSweepConfig{.sample_rate_hz = envelope_rate}};
  sweep.prepare(sliced.size());
  const std::span<const video::BeamSample> beam = sweep.process(sliced);

  // Find the framed window: the sample where line `start_line_` begins, and
  // the count of locked lines available, by walking the line_start flags.
  constexpr auto kNoSample = std::numeric_limits<std::size_t>::max();
  std::size_t frame_start = kNoSample;
  std::size_t total_locked_lines = 0;
  for (std::size_t i = 0; i < beam.size(); ++i) {
    if (!beam[i].line_start)
      continue;
    if (total_locked_lines == start_line_ && frame_start == kNoSample)
      frame_start = i;
    ++total_locked_lines;
  }
  if (frame_start == kNoSample) {
    std::println(std::cerr, "render: only {} locked lines available, can't skip {}", total_locked_lines, start_line_);
    return 1;
  }

  std::size_t frame_end = beam.size();
  std::size_t lines_remaining = lines_;
  for (std::size_t i = frame_start + 1; i < beam.size(); ++i) {
    if (!beam[i].line_start)
      continue;
    if (--lines_remaining == 0) {
      frame_end = i;
      break;
    }
  }
  // How many rows actually get painted: capped by what the sweep locked, so the
  // summary reports the real count rather than the requested one when the clip
  // ran short. The rest of the image stays black.
  const std::size_t available = total_locked_lines - start_line_;
  const std::size_t framed = std::min(lines_, available);

  // Contrast-stretch using only the framed window's envelope, so a pre-lock
  // transient or VBI spike outside the frame doesn't compress the picture.
  float hi = -std::numeric_limits<float>::infinity();
  float lo = std::numeric_limits<float>::infinity();
  for (std::size_t i = frame_start; i < frame_end; ++i) {
    hi = std::max(hi, beam[i].envelope);
    lo = std::min(lo, beam[i].envelope);
  }
  const float span = hi > lo ? hi - lo : 1.0f;

  // Accumulate-mean splat: with ~1024 samples/line vs width_ pixels, more than
  // one sample lands in most pixels — average them rather than last-write-wins,
  // so horizontal detail isn't an arbitrary bin-edge pick.
  std::vector<float> pixel_sum(width_ * lines_, 0.0f);
  std::vector<std::uint32_t> pixel_count(width_ * lines_, 0);
  std::size_t y = 0;
  for (std::size_t i = frame_start; i < frame_end; ++i) {
    if (i > frame_start && beam[i].line_start) {
      if (++y >= lines_)
        break;
    }
    auto x = static_cast<std::size_t>(static_cast<double>(beam[i].h_phase) * static_cast<double>(width_));
    if (x >= width_)
      x = width_ - 1;
    pixel_sum[y * width_ + x] += beam[i].envelope;
    ++pixel_count[y * width_ + x];
  }

  std::vector<std::uint8_t> grey(width_ * lines_);
  for (std::size_t i = 0; i < grey.size(); ++i) {
    if (pixel_count[i] == 0) {
      grey[i] = 0; // no sample landed here; reads as sync-tip black
    }
    else {
      const float mean = pixel_sum[i] / static_cast<float>(pixel_count[i]);
      const float white = (hi - mean) / span; // 1 at darkest envelope, 0 at sync
      grey[i] = static_cast<std::uint8_t>(std::clamp(white, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  }

  auto output = output_;
  if (output.empty())
    output = std::format("{}.png", loaded.meta_path.stem().string());
  image::write_png_grey(output, grey, static_cast<unsigned>(width_), static_cast<unsigned>(lines_));

  const double locked_line_hz = sweep.omega() * envelope_rate;
  std::println("wrote {} ({}x{}); envelope @ {:g} MS/s after /{} decimation, carrier {:.4f} MHz; "
               "sweep accepted {} edges (rejected {}), locked to {:.2f} Hz ({:+.2f}% from nominal); "
               "painted {} of {} locked lines (skipped {})",
      output.string(), width_, lines_, envelope_rate / 1e6, decimate_, loaded.vision_carrier_hz / 1e6,
      sweep.accepted_edges(), sweep.rejected_edges(), locked_line_hz, 100.0 * (locked_line_hz - 15625.0) / 15625.0,
      framed, total_locked_lines, start_line_);
  return 0;
}

} // namespace palindrome::cli
