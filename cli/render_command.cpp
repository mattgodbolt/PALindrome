#include "render_command.hpp"

#include "cli_util.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/image.hpp"
#include "palindrome/pipeline_run.hpp"
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

namespace {
// foo/bar.png + 7 -> foo/bar_0007.png. An empty parent stays a bare filename.
std::filesystem::path numbered_path(const std::filesystem::path &base, std::size_t idx) {
  return base.parent_path() / std::format("{}_{:04}{}", base.stem().string(), idx, base.extension().string());
}

// The largest decimation that still keeps the colour subcarrier at or below
// 0.7*Nyquist of the decimated rate — so it stays out of the anti-alias roll-off
// where group delay climbs (the AirSpy's near-Nyquist problem). Decimating is
// purely a speed lever, so take as much as the signal allows: RX888 32 MS/s ->
// /2, AirSpy 10 MS/s -> /1. An explicit --decimate overrides this.
std::size_t auto_decimate(double sample_rate_hz, double subcarrier_hz) {
  constexpr double kMaxSubcarrierNyquistFraction = 0.7;
  const double n = kMaxSubcarrierNyquistFraction * 0.5 * sample_rate_hz / subcarrier_hz;
  return std::max<std::size_t>(1, static_cast<std::size_t>(n));
}
} // namespace

void RenderCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("render", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("Render a recording's vision signal to a PNG using a sync-locked horizontal flywheel")
          .add_argument(lyra::opt(output_, "file")["-o"]["--output"]("Output PNG (default: <recording>.png)"))
          .add_argument(lyra::opt(carrier_, "hz")["--carrier"]("Carrier Hz (default: rx888:vision_if_hz)"))
          .add_argument(lyra::opt(cutoff_, "hz")["--cutoff"]("Baseband low-pass cutoff Hz"))
          .add_argument(lyra::opt(sync_cutoff_, "hz")["--sync-cutoff"]("Sync-branch low-pass cutoff Hz"))
          .add_argument(lyra::opt(decimate_, "n")["--decimate"]("Keep 1 sample per N inputs (0 = auto from Nyquist)"))
          .add_argument(lyra::opt(width_, "px")["--width"]("Output image width"))
          .add_argument(lyra::opt(height_, "px")["--height"]("Output image height"))
          .add_argument(lyra::opt(persistence_, "fields")["--persistence"]("Phosphor persistence in field periods"))
          .add_argument(lyra::opt(beam_sigma_, "rows")["--beam-sigma"]("Beam-spot vertical size in output rows"))
          .add_argument(lyra::opt(beam_sigma_x_, "cols")["--beam-sigma-x"](
              "Beam-spot horizontal size in output columns (<0 = match --beam-sigma, a round spot)"))
          .add_argument(lyra::opt(gamma_, "g")["--gamma"]("Electron-gun gamma (1.0 = linear)"))
          .add_argument(lyra::opt(colour_)["--colour"]["--color"]("Decode PAL colour (RGB)"))
          .add_argument(lyra::opt(saturation_, "x")["--saturation"]("Colour: chroma gain into the gun matrix"))
          .add_argument(lyra::opt(contrast_, "x")["--contrast"]("Readout white point (AGC-relative; the contrast pot)"))
          .add_argument(lyra::opt(h_blank_, "x")["--h-blank"]("Retrace blanking end (h_phase; ~0.21 at 10 MS/s)"))
          .add_argument(
              lyra::opt(subcarrier_, "hz")["--subcarrier"]("Colour: subcarrier crystal Hz (default 4.43361875 MHz)"))
          .add_argument(
              lyra::opt(burst_gate_lo_, "x")["--burst-lo"]("Colour: burst gate start (h_phase; ~0.16 at 10 MS/s)"))
          .add_argument(lyra::opt(burst_gate_hi_, "x")["--burst-hi"]("Colour: burst gate end (h_phase)"))
          .add_argument(lyra::opt(no_delay_line_)["--no-delay-line"]("Colour: disable the PAL-D line-pair comb"))
          .add_argument(lyra::opt(sync_level_, "x")["--sync-level"]("Sync-separator slice level"))
          .add_argument(lyra::opt(h_kp_, "x")["--h-kp"]("Horizontal hold: AFC kp"))
          .add_argument(lyra::opt(h_ki_, "x")["--h-ki"]("Horizontal hold: AFC ki"))
          .add_argument(lyra::opt(h_clamp_, "x")["--h-clamp"]("Horizontal hold: omega clamp"))
          .add_argument(lyra::opt(v_level_, "x")["--v-level"]("Vertical hold: vsync slice level"))
          .add_argument(lyra::opt(v_kp_, "x")["--v-kp"]("Vertical hold: field-PLL kp"))
          .add_argument(lyra::opt(v_ki_, "x")["--v-ki"]("Vertical hold: field-PLL ki"))
          .add_argument(lyra::opt(v_tc_, "x")["--v-tc"]("Vertical hold: integrator time constant (lines)"))
          .add_argument(lyra::opt(v_minfield_, "x")["--v-min-field"]("Vertical hold: min field fraction"))
          .add_argument(lyra::opt(frame_stride_, "n")["--frame-stride"](
              "Write a PNG every Nth field boundary (<stem>_NNNN.png); 0 = one image"))
          .add_argument(lyra::opt(no_sound_trap_)["--no-sound-trap"]("Disable the sound-carrier notch"))
          .add_argument(lyra::opt(sound_q_, "q")["--sound-q"]("Sound-trap notch Q"))
          .add_argument(lyra::opt(no_sync_)["--no-sync"]("Debug: naive-fold the envelope, bypassing sync"))
          .add_argument(
              lyra::opt(no_threads_)["--no-threads"]("Decode serially (default is a threaded stage pipeline)"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to render (e.g. corpus/alex_kidd)")));
}

int RenderCommand::run() const {
  if (width_ == 0 || height_ == 0) {
    std::println(std::cerr, "render: --width and --height must be positive");
    return 1;
  }

  const auto loaded = load_recording(recording_, carrier_);

  // --decimate 0 (the default) means pick the decimation from the signal; any
  // explicit value wins. The subcarrier is the textbook PAL crystal unless a
  // positive --subcarrier overrides it (matching the decoder, which ignores a
  // non-positive value — so auto_decimate never divides by zero or negative).
  const double subcarrier_hz = subcarrier_ > 0.0 ? subcarrier_ : 4.43361875e6;
  const std::size_t decimate = decimate_ != 0 ? decimate_ : auto_decimate(loaded.sample_rate_hz, subcarrier_hz);

  const auto envelope_rate = loaded.sample_rate_hz / static_cast<double>(decimate);

  const EnvelopeOptions opts{
      .cutoff_hz = cutoff_, .decimation = decimate, .no_sound_trap = no_sound_trap_, .sound_q = sound_q_};

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
    const auto n = std::min(env.size(), width_ * height_);
    auto hi = env[0];
    auto lo = env[0];
    for (std::size_t i = 0; i < n; ++i) {
      hi = std::max(hi, env[i]);
      lo = std::min(lo, env[i]);
    }
    const auto span = hi > lo ? hi - lo : 1.0f;
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
  video::DecoderConfig dc{
      .sample_rate_hz = envelope_rate, .width = width_, .height = height_, .sync_lp_cutoff_hz = sync_cutoff_};
  dc.persistence_fields = persistence_;
  dc.beam_sigma_rows = beam_sigma_;
  dc.beam_sigma_cols = beam_sigma_x_;
  dc.gamma = gamma_;
  dc.colour = colour_;
  dc.saturation = saturation_;
  dc.contrast = contrast_;
  dc.h_blank = h_blank_;
  if (subcarrier_ > 0.0) // else the crystal default (textbook fsc)
    dc.chroma.subcarrier_hz = subcarrier_;
  dc.chroma.burst_gate_lo = burst_gate_lo_;
  dc.chroma.burst_gate_hi = burst_gate_hi_;
  dc.chroma.delay_line = !no_delay_line_;
  dc.sep.sync_level = sync_level_;
  dc.hsweep.pll_kp = h_kp_;
  dc.hsweep.pll_ki = h_ki_;
  dc.hsweep.omega_clamp = h_clamp_;
  dc.vsync.vsync_level = v_level_;
  dc.vsync.pll_kp = v_kp_;
  dc.vsync.pll_ki = v_ki_;
  dc.vsync.integrator_tc_lines = v_tc_;
  dc.vsync.min_field_fraction = v_minfield_;
  video::Decoder decoder{dc};

  constexpr std::size_t kBlock = std::size_t{1} << 16;
  decoder.prepare(kBlock);

  std::filesystem::path output = output_;
  if (output.empty())
    output = std::format("{}.png", loaded.meta_path.stem().string());

  // Snapshot at field boundaries: in sequence mode write every Nth as
  // <stem>_NNNN.png; otherwise keep the last clean boundary for a single image
  // (cleaner than the mid-field state wherever the stream happens to end).
  std::size_t fields_seen = 0;
  std::size_t written = 0;
  std::optional<video::Screen::Frame> last_frame;
  const auto save = [](const std::filesystem::path &p, const video::Screen::Frame &f) {
    if (f.channels == 3)
      image::write_png_rgb(p, f.pixels, static_cast<unsigned>(f.width), static_cast<unsigned>(f.height));
    else
      image::write_png_grey(p, f.pixels, static_cast<unsigned>(f.width), static_cast<unsigned>(f.height));
  };
  const video::Screen::FieldCallback on_field = [&](const video::Screen::Frame &f) {
    if (frame_stride_ == 0) {
      last_frame = f; // single-image mode: keep the latest clean boundary
      return;
    }
    if (fields_seen++ % frame_stride_ == 0)
      save(numbered_path(output, written++), f);
  };

  // The pipeline: a push source (the real-IF/complex front end, which the
  // decoder just sees as composite-envelope blocks) -> decode -> screen deposit.
  // pipe::run threads it (each stage on its own in-order worker, owned blocks
  // through bounded pools — the live-streaming shape) or runs it inline for
  // --no-threads, from this one description. Either way it's bit-identical.
  constexpr std::ptrdiff_t kInFlight = 4;
  EnvelopeStream es;
  pipe::run(
      !no_threads_, kInFlight, //
      [&](const auto &emit) { es = stream_envelope(loaded, opts, emit, kBlock); },
      pipe::transform<video::DecodedBlock>(
          kInFlight, [&](std::span<const float> env, video::DecodedBlock &out) { decoder.decode_into(out, env); }),
      pipe::sink([&](const video::DecodedBlock &block) { decoder.deposit(block, on_field); }));
  for (const auto &w: es.warnings)
    std::println(std::cerr, "render: warning: {}", w);

  if (decoder.accepted_edges() == 0 || decoder.detected_fields() == 0) {
    std::println(std::cerr, "render: never locked ({} line edges, {} fields) — nothing to draw",
        decoder.accepted_edges(), decoder.detected_fields());
    return 1;
  }

  if (frame_stride_ == 0)
    save(output, last_frame ? *last_frame : decoder.snapshot());

  if (colour_)
    std::println("colour: crystal {:.4f} MHz, burst amplitude {:.4g}, burst swing {:.1f} deg",
        decoder.subcarrier_hz() / 1e6, decoder.burst_amplitude(), decoder.burst_swing_deg());

  const double line_hz = decoder.line_omega() * envelope_rate;
  const double field_hz = decoder.field_omega() * envelope_rate;
  const auto what = frame_stride_ > 0 ? std::format("wrote {} frames {}_NNNN.png (every {} fields)", written,
                                            output.stem().string(), frame_stride_)
                                      : std::format("wrote {}", output.string());
  std::println("{} ({}x{}); envelope @ {:g} MS/s after /{} decimation, carrier {:.4f} MHz; "
               "horizontal locked {} edges @ {:.1f} Hz ({:+.2f}%); vertical locked {} fields @ {:.2f} Hz ({:+.2f}%)",
      what, width_, height_, envelope_rate / 1e6, decimate, loaded.vision_carrier_hz / 1e6, decoder.accepted_edges(),
      line_hz, 100.0 * (line_hz - 15625.0) / 15625.0, decoder.detected_fields(), field_hz,
      100.0 * (field_hz - 50.0) / 50.0);
  return 0;
}

} // namespace palindrome::cli
