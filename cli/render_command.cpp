#include "render_command.hpp"

#include "cli_util.hpp"
#include "palindrome/decoder.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/image.hpp"
#include "palindrome/pipeline_run.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
// /2, AirSpy 20 MS/s raw -> /1. An explicit --decimate overrides this.
std::size_t auto_decimate(double sample_rate_hz, double subcarrier_hz) {
  constexpr double kMaxSubcarrierNyquistFraction = 0.7;
  const double n = kMaxSubcarrierNyquistFraction * 0.5 * sample_rate_hz / subcarrier_hz;
  return std::max<std::size_t>(1, static_cast<std::size_t>(n));
}

// One table-driven parser for the string mode flags, so the error message is
// derived from the same table as the matching and the two can't drift.
template<class E>
std::optional<E> parse_choice(
    std::string_view flag, std::string_view value, std::initializer_list<std::pair<std::string_view, E>> choices) {
  std::string valid;
  for (const auto &[name, mode]: choices) {
    if (value == name)
      return mode;
    valid = valid.empty() ? std::string{name} : std::format("{}, {}", valid, name);
  }
  std::println(std::cerr, "render: --{} must be one of: {}", flag, valid);
  return std::nullopt;
}

// Debug: fold the raw envelope into the frame (sample i -> x = i % width,
// y = i / width), bypassing the sync graph. Set --width to samples/line to
// straighten it. Shows whether the picture is in the envelope at all.
int render_unsynced(const LoadedRecording &loaded, const EnvelopeOptions &opts, std::size_t width, std::size_t height,
    std::filesystem::path output) {
  std::vector<float> env;
  stream_envelope(loaded, opts, [&](std::span<const float> e) { env.insert(env.end(), e.begin(), e.end()); });
  if (env.empty()) {
    std::println(std::cerr, "render: no samples read from {}", loaded.data_path.string());
    return 1;
  }
  const auto n = std::min(env.size(), width * height);
  auto hi = env[0];
  auto lo = env[0];
  for (std::size_t i = 0; i < n; ++i) {
    hi = std::max(hi, env[i]);
    lo = std::min(lo, env[i]);
  }
  const auto span = hi > lo ? hi - lo : 1.0f;
  std::vector<std::uint8_t> grey(width * height, 0);
  for (std::size_t i = 0; i < n; ++i)
    grey[i] = static_cast<std::uint8_t>(std::clamp((hi - env[i]) / span, 0.0f, 1.0f) * 255.0f + 0.5f);
  if (output.empty())
    output = std::format("{}.png", loaded.meta_path.stem().string());
  image::write_png_grey(output, grey, static_cast<unsigned>(width), static_cast<unsigned>(height));
  std::println("wrote {} ({}x{}, naive envelope fold, no sync)", output.string(), width, height);
  return 0;
}
} // namespace

void RenderCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("render", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("Render a recording's vision signal to a PNG using a sync-locked horizontal flywheel")
          .add_argument(lyra::opt(output_, "file")["-o"]["--output"]("Output PNG (default: <recording>.png)"))
          .add_argument(lyra::opt(carrier_, "hz")["--carrier"](
              "Carrier Hz (default: from metadata, or a signal scan if the metadata has none)"))
          .add_argument(lyra::opt(scan_)["--scan"](
              "Find the carrier by scanning a RECORDING's spectrum, ignoring the metadata (bench analysis - also "
              "validates the metadata's carrier; not a live mode: a set is tuned, it does not scan)"))
          .add_argument(lyra::opt(live_)["--live"](
              "Live mode: decode a continuous real-int16 SDR stream from stdin (e.g. airspy_rx -r /dev/stdout | "
              "palindrome render --live --sample-rate 20e6 --carrier 3e6). Needs --carrier: the tuner's IF-plan "
              "target, i.e. the channel preset - the AFC absorbs drift from there. Streams raw frames to "
              "--frame-fd until the pipe closes"))
          .add_argument(lyra::opt(sample_rate_, "hz")["--sample-rate"](
              "Live input real sample rate Hz (required with --live; 20e6 for the AirSpy raw stream)"))
          .add_argument(lyra::opt(cutoff_, "hz")["--cutoff"]("Baseband low-pass cutoff Hz (--if flat only)"))
          .add_argument(lyra::opt(sync_cutoff_, "hz")["--sync-cutoff"]("Sync-branch low-pass cutoff Hz"))
          .add_argument(lyra::opt(decimate_, "n")["--decimate"]("Keep 1 sample per N inputs (0 = auto from Nyquist)"))
          .add_argument(lyra::opt(width_, "px")["--width"]("Output image width"))
          .add_argument(lyra::opt(height_, "px")["--height"]("Output image height"))
          .add_argument(lyra::opt(persistence_, "fields")["--persistence"]("Phosphor persistence in field periods"))
          .add_argument(lyra::opt(beam_sigma_, "pitches")["--beam-sigma"](
              "Beam-spot vertical sigma in scanline pitches (raster-relative, so --height/--overscan don't change "
              "the spot; default 0.52 ~ the old 1.1 rows)"))
          .add_argument(lyra::opt(beam_sigma_x_, "cols")["--beam-sigma-x"](
              "Beam-spot horizontal size in output columns (<0 = match --beam-sigma, a round spot)"))
          .add_argument(
              lyra::opt(gamma_, "g")["--gamma"]("Electron-gun gamma (default 2.6, a real tube; 1.0 = linear)"))
          .add_argument(lyra::opt(readout_gamma_, "g")["--readout-gamma"](
              "Encode the phosphor light for a display with this gamma (default 2.2, sRGB-ish; 1.0 = raw linear)"))
          .add_argument(lyra::opt(overscan_, "x")["--overscan"](
              "Fraction of the active picture cropped behind the bezel (default 0.06); negative = the old "
              "full-scan framing, blanking on screen"))
          .add_argument(lyra::opt(eht_sag_, "x")["--eht-sag"](
              "EHT sag at a sustained full-white load (default 0.06: the raster breathes ~3%, dims and defocuses "
              "on bright scenes; 0 = a perfectly regulated supply)"))
          .add_argument(lyra::opt(eht_tc_, "fields")["--eht-tc"]("EHT sag/recovery time constant in field periods"))
          .add_argument(lyra::opt(eht_focus_, "x")["--eht-focus"]("Spot growth at full EHT sag (focus mistracking)"))
          .add_argument(lyra::opt(line_pull_, "x")["--line-pull"](
              "Line-output loading: width stretch after a full-white line (verticals bend next to bright content; "
              "0 disables)"))
          .add_argument(lyra::opt(bcl_, "x")["--bcl"](
              "Beam-current limiter: average beam load above which the contrast is pulled down (default 0.7; "
              "0 disables — an unprotected set)"))
          .add_argument(lyra::opt(bcl_tc_, "fields")["--bcl-tc"]("Beam-current limiter response, field periods"))
          .add_argument(lyra::opt(h_shift_, "x")["--h-shift"](
              "Horizontal centring (an internal service adjustment on a real set; factory default 0 should be "
              "right): shifts the picture right by this fraction of a line"))
          .add_argument(lyra::opt(v_shift_, "x")["--v-shift"](
              "Vertical centring (internal service adjustment): shifts the picture down by this fraction of a field"))
          .add_argument(lyra::opt(if_mode_, "mode")["--if"](
              "IF response: saw80 (default - an 80s single-SAW set: Nyquist flank through the carrier, vestigial "
              "lower sideband, chroma a few dB down, finite sound notch, group-delay ripple) | saw90 (a 90s set: "
              "flat through chroma, deeper notch, cleaner phase) | flat (the ideal symmetric low-pass, the pre-SAW "
              "front end)"))
          .add_argument(lyra::opt(detector_, "mode")["--detector"](
              "Vision detector (saw modes): quasi-sync (default - the TDA-era product detector: an NCO with a "
              "slow phase lock on the carrier, linear through overmodulation and free of VSB quadrature "
              "distortion) | envelope (a diode detector: the magnitude, with the quadrature fold-through of the "
              "early sets)"))
          .add_argument(lyra::opt(sound_notch_db_, "db")["--sound-notch-db"](
              "IF sound rejection in dB, positive (saw modes; default from the template: 26 for saw80, 40 for "
              "saw90 - deliberately finite, the intercarrier residue is real; 0 removes the notch)"))
          .add_argument(lyra::opt(gd_ripple_, "ns")["--gd-ripple"](
              "IF group-delay ripple in ns peak (saw modes; default from the template: 50 for saw80, 8 for saw90)"))
          .add_argument(lyra::opt(agc_mode_, "mode")["--agc"](
              "Level scheme: sync-tip (default - the IF AGC holds the sync tip at 1.0, white is the standard's "
              "geometry) | adaptive (the legacy per-stage trackers, an autocontrast)"))
          .add_argument(lyra::opt(slice_depth_, "x")["--slice-depth"](
              "Sync slice depth below the AGC'd tip (sync-tip mode; default 0.08 sits inside both broadcast 0.24 "
              "and shallow console-modulator sync)"))
          .add_argument(lyra::opt(pwl_, "x")["--pwl"](
              "Peak-white limiter ceiling as a multiple of standard white drive (default 1.25, the TDA3561A's; "
              "0 disables; sync-tip mode only)"))
          .add_argument(lyra::opt(colour_)["--colour"]["--color"]("Decode PAL colour (RGB)"))
          .add_argument(lyra::opt(saturation_, "x")["--saturation"](
              "Colour: chroma gain into the gun matrix (default 0.085; 0.17 in --agc adaptive)"))
          .add_argument(lyra::opt(contrast_, "x")["--contrast"](
              "The contrast pot: video gain ahead of the gun (default 1.6, turned up for the under-modulated "
              "SMS corpus; broadcast wants ~1.0). In --agc adaptive: the readout white point, default 0.85"))
          .add_argument(lyra::opt(h_blank_, "x")["--h-blank"]("Retrace blanking end, as an h_phase fraction"))
          .add_argument(
              lyra::opt(subcarrier_, "hz")["--subcarrier"]("Colour: subcarrier crystal Hz (default 4.43361875 MHz)"))
          .add_argument(lyra::opt(uv_bandwidth_, "hz")["--uv-bandwidth"](
              "Colour: post-demod U/V low-pass corner Hz (0 = decoder default)"))
          .add_argument(lyra::opt(band_lo_, "hz")["--band-lo"]("Colour: chroma band-pass low edge Hz"))
          .add_argument(lyra::opt(band_hi_, "hz")["--band-hi"]("Colour: chroma band-pass high edge Hz"))
          .add_argument(
              lyra::opt(burst_gate_lo_, "x")["--burst-lo"]("Colour: burst gate start, as an h_phase fraction"))
          .add_argument(lyra::opt(burst_gate_hi_, "x")["--burst-hi"]("Colour: burst gate end (h_phase)"))
          .add_argument(lyra::opt(comb_mode_, "mode")["--comb-mode"](
              "Colour: 1H comb — off (PAL-S) | delay-line (PAL-D, adaptive depth) | glass (PAL-D, the real fixed "
              "63.943 us block) | post (default)"))
          .add_argument(lyra::opt(no_delay_line_)["--no-delay-line"]("Colour: alias for --comb-mode off"))
          .add_argument(lyra::opt(ref_tc_, "lines")["--ref-tc"](
              "Colour: APC reference time constant in lines (default 10; slower = more period-faithful, [2,100])"))
          .add_argument(
              lyra::opt(no_killer_)["--no-killer"]("Colour: disable the colour killer (no ident-based chroma muting)"))
          .add_argument(lyra::opt(apc_catch_, "hz")["--apc-catch"](
              "Colour: APC crystal-pull catching range Hz (default 500, as a real crystal; 0 = fixed crystal)"))
          .add_argument(lyra::opt(apc_pull_, "x")["--apc-pull"]("Colour: APC pull rate (fraction of drift per line)"))
          .add_argument(
              lyra::opt(sync_level_, "x")["--sync-level"]("Sync-separator slice level (--agc adaptive mode only)"))
          .add_argument(
              lyra::opt(h_kp_, "x")["--h-kp"]("Horizontal hold: locked (flywheel) AFC kp; 1.0 = direct triggering"))
          .add_argument(lyra::opt(h_ki_, "x")["--h-ki"]("Horizontal hold: locked AFC ki"))
          .add_argument(lyra::opt(h_acq_kp_, "x")["--h-acq-kp"]("Horizontal hold: acquisition AFC kp"))
          .add_argument(lyra::opt(h_acq_ki_, "x")["--h-acq-ki"]("Horizontal hold: acquisition AFC ki"))
          .add_argument(lyra::opt(h_clamp_, "x")["--h-clamp"]("Horizontal hold: omega clamp"))
          .add_argument(lyra::opt(v_level_, "x")["--v-level"]("Vertical hold: vsync slice level"))
          .add_argument(lyra::opt(v_kp_, "x")["--v-kp"]("Vertical hold: field-PLL kp"))
          .add_argument(lyra::opt(v_ki_, "x")["--v-ki"]("Vertical hold: field-PLL ki"))
          .add_argument(lyra::opt(v_tc_, "x")["--v-tc"]("Vertical hold: integrator time constant (lines)"))
          .add_argument(lyra::opt(v_minfield_, "x")["--v-min-field"]("Vertical hold: min field fraction"))
          .add_argument(lyra::opt(frame_stride_, "n")["--frame-stride"](
              "Write a PNG every Nth field boundary (<stem>_NNNN.png); 0 = one image"))
          .add_argument(lyra::opt(frame_fd_, "fd")["--frame-fd"](
              "Live: write raw RGB frames (w*h*channels bytes) to this fd instead of a PNG"))
          .add_argument(lyra::opt(no_sync_)["--no-sync"]("Debug: naive-fold the envelope, bypassing sync"))
          .add_argument(
              lyra::opt(no_threads_)["--no-threads"]("Decode serially (default is a threaded stage pipeline)"))
          .add_argument(lyra::opt(deposit_threads_, "n")["--deposit-threads"](
              "Threads the screen deposit fans a field's splats across (bit-exact); 1 = serial"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to render (e.g. corpus/alex_kidd)")));
}

int RenderCommand::run() const {
  if (width_ == 0 || height_ == 0) {
    std::println(std::cerr, "render: --width and --height must be positive");
    return 1;
  }

  LoadedRecording loaded;
  if (live_) {
    if (!(sample_rate_ > 0.0)) {
      std::println(std::cerr, "render: --live requires --sample-rate (e.g. 20e6 for the AirSpy raw stream)");
      return 1;
    }
    if (no_sync_) {
      std::println(std::cerr, "render: --no-sync cannot combine with --live");
      return 1;
    }
    if (frame_fd_ < 0) {
      std::println(std::cerr, "render: --live requires --frame-fd (raw frames for the MJPEG viewer)");
      return 1;
    }
    if (scan_) {
      std::println(std::cerr, "render: --scan analyses recordings; --live is tuned by --carrier, not by scanning");
      return 1;
    }
    if (!(carrier_ > 0.0)) {
      std::println(std::cerr,
          "render: --live requires --carrier - the tuner's IF-plan target (the channel preset; live_view.py "
          "supplies it). The AFC absorbs drift from there; a set is tuned, it does not scan.");
      return 1;
    }
    // Let a closed reader surface as EPIPE from write() rather than killing us
    // with SIGPIPE (the viewer disconnecting is a normal live-stream event).
    std::signal(SIGPIPE, SIG_IGN);
    loaded.sample_rate_hz = sample_rate_; // the carrier is --carrier verbatim (checked above): the tuner's preset
  }
  else {
    loaded = load_recording(recording_, carrier_, scan_);
    if (loaded.carrier_scanned) {
      if (loaded.metadata_carrier_hz > 0.0)
        std::println("carrier: scanned {:.4f} MHz (metadata said {:.4f} MHz, off by {:+.0f} Hz)",
            loaded.vision_carrier_hz / 1e6, loaded.metadata_carrier_hz / 1e6,
            loaded.vision_carrier_hz - loaded.metadata_carrier_hz);
      else
        std::println("carrier: scanned {:.4f} MHz (no metadata carrier)", loaded.vision_carrier_hz / 1e6);
    }
  }

  // --decimate 0 (the default) means pick the decimation from the signal; any
  // explicit value wins. The subcarrier is the textbook PAL crystal unless a
  // positive --subcarrier overrides it (matching the decoder, which ignores a
  // non-positive value — so auto_decimate never divides by zero or negative).
  const double subcarrier_hz = subcarrier_ > 0.0 ? subcarrier_ : 4.43361875e6;
  const std::size_t decimate = decimate_ != 0 ? decimate_ : auto_decimate(loaded.sample_rate_hz, subcarrier_hz);

  const auto envelope_rate = loaded.sample_rate_hz / static_cast<double>(decimate);

  EnvelopeOptions opts{.cutoff_hz = cutoff_, .decimation = decimate};
  const auto if_mode = parse_choice<IfMode>(
      "if", if_mode_, {{"saw80", IfMode::saw80}, {"saw90", IfMode::saw90}, {"flat", IfMode::flat}});
  if (!if_mode)
    return 1;
  opts.if_mode = *if_mode;
  const auto detector = parse_choice<demod::Detector>(
      "detector", detector_, {{"quasi-sync", demod::Detector::quasi_sync}, {"envelope", demod::Detector::envelope}});
  if (!detector)
    return 1;
  opts.detector = *detector;
  // The flag sentinels (negative = "use the template's value") become absent
  // options here, so EnvelopeOptions carries intent, not magic numbers.
  if (sound_notch_db_ >= 0.0)
    opts.sound_notch_db = sound_notch_db_;
  if (gd_ripple_ >= 0.0)
    opts.gd_ripple_ns = gd_ripple_;

  if (no_sync_)
    return render_unsynced(loaded, opts, width_, height_, output_);

  // The whole video graph as one composite node: it fans the separator's sync
  // bit to the horizontal sweep and vertical sync, then joins both timebases
  // with the picture rail at the phosphor screen. We pump the recording through
  // in blocks, so nothing ever materialises the whole envelope.
  const auto dc = decoder_config(envelope_rate);
  if (!dc)
    return 1;
  video::Decoder decoder{*dc};

  constexpr std::size_t kBlock = std::size_t{1} << 16;
  decoder.prepare(kBlock);

  // The output path is for the file modes; --live streams to --frame-fd, not a file.
  std::filesystem::path output = output_;
  if (output.empty() && !live_)
    output = std::format("{}.png", loaded.meta_path.stem().string());

  // Snapshot at field boundaries: in sequence mode quantise and write every Nth
  // as <stem>_NNNN.png; otherwise just latch the boundary state (a float copy)
  // so the last clean boundary is quantised once at the end — cleaner than the
  // mid-field state wherever the stream happens to end, without paying a full
  // quantise pass per field for frames that are thrown away.
  std::size_t fields_seen = 0;
  std::size_t written = 0;
  const auto save = [](const std::filesystem::path &p, const video::Screen::Frame &f) {
    if (f.channels == 3)
      image::write_png_rgb(p, f.pixels, static_cast<unsigned>(f.width), static_cast<unsigned>(f.height));
    else
      image::write_png_grey(p, f.pixels, static_cast<unsigned>(f.width), static_cast<unsigned>(f.height));
  };
  // Live: raw RGB frames straight to a fd, so a downstream process does the
  // JPEG/MJPEG encode instead of this deposit thread paying the zlib cost per
  // snapshot.
  const auto write_frame_fd = [fd = frame_fd_](const video::Screen::Frame &f) {
    const auto *bytes = f.pixels.data();
    auto left = f.pixels.size();
    while (left > 0) {
      const auto n = ::write(fd, bytes, left);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        throw std::system_error{errno, std::generic_category(), "render: --frame-fd write"};
      }
      if (n == 0)
        throw std::system_error{EIO, std::generic_category(), "render: --frame-fd write returned 0"};
      bytes += n;
      left -= static_cast<std::size_t>(n);
    }
  };
  // ~10 frames/sec at 50 fields/s, throttling the snapshot + downstream encode (the
  // phosphor still paints every field; this only caps how often we snapshot it).
  const std::size_t live_stride = frame_stride_ != 0 ? frame_stride_ : 5;
  const video::Screen::FieldCallback on_field = [&](const video::Screen::FieldEvent &e) {
    if (live_) {
      if (fields_seen++ % live_stride == 0)
        write_frame_fd(e.frame());
      return;
    }
    if (frame_stride_ == 0) {
      e.latch(); // single-image mode: keep the latest clean boundary
      return;
    }
    if (fields_seen++ % frame_stride_ == 0)
      save(numbered_path(output, written++), e.frame());
  };

  // The pipeline: a push source (the real-IF/complex front end, which the
  // decoder just sees as composite-envelope blocks) -> decode -> screen deposit.
  // pipe::run threads it (each stage on its own in-order worker, owned blocks
  // through bounded pools — the live-streaming shape) or runs it inline for
  // --no-threads, from this one description. Either way it's bit-identical.
  //
  // Depth is a field's worth of blocks (~5-6 at this block size), not a handful:
  // the screen deposit applies a whole field of splats in one burst at each field
  // boundary, and the decode stage can only run ahead through it - overlapping the
  // burst instead of stalling on it - if the pool can hold that many blocks.
  constexpr std::ptrdiff_t kInFlight = 16;
  EnvelopeStream es;
  pipe::run(
      !no_threads_, kInFlight, //
      [&](const auto &emit) {
        es = live_ ? stream_envelope_live(sample_rate_, carrier_, opts, emit, kBlock)
                   : stream_envelope(loaded, opts, emit, kBlock);
      },
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

  if (!live_ && frame_stride_ == 0)
    save(output, decoder.latched_frame()); // falls back to the live state if no field completed

  if (colour_)
    std::println("colour: crystal {:.4f} MHz (APC pull {:+.1f} Hz), burst amplitude {:.4g}, burst swing {:.1f} deg, "
                 "killer gate {:.2f}{}",
        decoder.subcarrier_hz() / 1e6, decoder.subcarrier_hz() - subcarrier_hz, decoder.burst_amplitude(),
        decoder.burst_swing_deg(), decoder.killer_gain(),
        decoder.killer_gain() < video::ChromaDecoder::kKillerSwitch ? " (COLOUR KILLED)" : "");

  if (decoder.limiter_gain() < 0.99)
    std::println(
        "limiters: protection gain {:.3f} (beam-current x peak-white, contrast pot excluded)", decoder.limiter_gain());
  if (decoder.agc_gain() > 0.0)
    std::println("AGC: front-end gain {:.3f}x (sync tip held at 1.0)", decoder.agc_gain());
  const double line_hz = decoder.line_omega() * envelope_rate;
  const double field_hz = decoder.field_omega() * envelope_rate;
  const auto what = live_ ? std::string{"live stream ended"}
                          : (frame_stride_ > 0 ? std::format("wrote {} frames {}_NNNN.png (every {} fields)", written,
                                                     output.stem().string(), frame_stride_)
                                               : std::format("wrote {}", output.string()));
  const auto afc = es.afc_offset_hz ? std::format(" (AFC {:+.2f} kHz)", *es.afc_offset_hz / 1e3) : std::string{};
  std::println("{} ({}x{}); envelope @ {:g} MS/s after /{} decimation, carrier {:.4f} MHz{}; "
               "horizontal {} {} edges @ {:.1f} Hz ({:+.2f}%); vertical locked {} fields @ {:.2f} Hz ({:+.2f}%)",
      what, width_, height_, envelope_rate / 1e6, decimate, es.carrier_hz / 1e6, afc,
      decoder.hold_locked() ? "locked" : "STILL ACQUIRING after", decoder.accepted_edges(), line_hz,
      100.0 * (line_hz - video::kNominalLineHz) / video::kNominalLineHz, decoder.detected_fields(), field_hz,
      100.0 * (field_hz - video::kNominalFieldHz) / video::kNominalFieldHz);
  return 0;
}

std::optional<video::DecoderConfig> RenderCommand::decoder_config(double envelope_rate) const {
  video::DecoderConfig dc{.sample_rate_hz = envelope_rate, .sync_lp_cutoff_hz = sync_cutoff_};
  const auto agc = parse_choice<video::AgcMode>(
      "agc", agc_mode_, {{"sync-tip", video::AgcMode::sync_tip}, {"adaptive", video::AgcMode::adaptive}});
  if (!agc)
    return std::nullopt;
  dc.agc_mode = *agc;
  const bool adaptive = dc.agc_mode == video::AgcMode::adaptive;
  dc.colour = colour_;

  auto &screen = dc.screen;
  screen.width = width_;
  screen.height = height_;
  screen.persistence_fields = persistence_;
  screen.beam_sigma = beam_sigma_;
  screen.beam_sigma_cols = beam_sigma_x_;
  screen.gamma = gamma_;
  // The pot defaults follow the level scheme (see render_command.hpp): the
  // sync-tip values are the provisional SMS calibration, the adaptive ones
  // are what every pre-AGC render used, so the legacy mode looks legacy.
  screen.saturation = saturation_ >= 0.0 ? saturation_ : (adaptive ? 0.17 : 0.085);
  screen.contrast = contrast_ >= 0.0 ? contrast_ : (adaptive ? 0.85 : 1.6);
  screen.deposit_lanes = deposit_threads_;
  screen.readout_gamma = readout_gamma_;
  screen.eht_sag = eht_sag_;
  screen.eht_tc_fields = eht_tc_;
  screen.eht_focus = eht_focus_;
  screen.line_pull = line_pull_;
  screen.bcl_threshold = bcl_;
  screen.bcl_tc_fields = bcl_tc_;
  screen.h_blank = h_blank_;
  screen.pwl_threshold = pwl_;
  // Overscan: map the nominal active picture box — the 52 us active line of
  // the 64 us period, and the picture lines after the vertical interval —
  // cropped by the overscan fraction (half per side) onto the full frame, so
  // blanking and the picture edges live behind the bezel as on a real set. A
  // negative overscan keeps the old full-scan framing (the whole 64 us line).
  if (overscan_ >= 0.0) {
    if (overscan_ >= 0.5) {
      std::println(std::cerr, "render: --overscan must be below 0.5 (got {:g})", overscan_);
      return std::nullopt;
    }
    // The visible window sits ~1 us EARLIER in the line than the textbook
    // active region (10.5..62.5 us): an average set's sweep reached the tube
    // face while the line was still blanked, so the left edge showed a sliver
    // of blanking-black before active video began — the factory framing that
    // consoles (whose content hugs the start of active) relied on to keep
    // their left edge on screen, with no adjustment needed.
    constexpr double kActiveHLo = 9.5 / 64.0;
    constexpr double kActiveHHi = 61.5 / 64.0;
    constexpr double kActiveVLo = 25.0 / 312.5; // the vertical interval's blanked lines
    constexpr double kActiveVHi = 1.0;
    const double crop_h = 0.5 * overscan_ * (kActiveHHi - kActiveHLo);
    const double crop_v = 0.5 * overscan_ * (kActiveVHi - kActiveVLo);
    screen.h_window_lo = kActiveHLo + crop_h;
    screen.h_window_hi = kActiveHHi - crop_h;
    screen.v_window_lo = kActiveVLo + crop_v;
    screen.v_window_hi = kActiveVHi - crop_v;
  }
  // The centring pots: moving the WINDOW left shows content further left, i.e.
  // the picture moves right — the H-shift adjustment on a real set's back
  // panel, which is how a console picture that hugs one edge of the nominal
  // active box gets centred. Applied to whichever framing is in effect. A shift
  // past a full line/field is meaningless (and an extreme value would map the
  // beam outside the splat coordinate range, which the Screen also rejects).
  if (std::abs(h_shift_) > 1.0 || std::abs(v_shift_) > 1.0) {
    std::println(std::cerr, "render: --h-shift and --v-shift must be within [-1, 1]");
    return std::nullopt;
  }
  screen.h_window_lo -= h_shift_;
  screen.h_window_hi -= h_shift_;
  screen.v_window_lo -= v_shift_;
  screen.v_window_hi -= v_shift_;

  if (subcarrier_ > 0.0) // else the crystal default (textbook fsc)
    dc.chroma.subcarrier_hz = subcarrier_;
  if (uv_bandwidth_ > 0.0)
    dc.chroma.uv_bandwidth_hz = uv_bandwidth_;
  if (band_lo_ > 0.0)
    dc.chroma.band_lo_hz = band_lo_;
  if (band_hi_ > 0.0)
    dc.chroma.band_hi_hz = band_hi_;
  dc.chroma.burst_gate_lo = burst_gate_lo_;
  dc.chroma.burst_gate_hi = burst_gate_hi_;
  dc.chroma.ref_tc_lines = ref_tc_;
  if (no_killer_)
    dc.chroma.killer_threshold = 0.0;
  dc.chroma.apc_catch_range_hz = apc_catch_;
  dc.chroma.apc_pull = apc_pull_;
  if (no_delay_line_) // deprecated alias, overridden by an explicit --comb-mode
    dc.chroma.comb_mode = video::CombMode::off;
  if (!comb_mode_.empty()) {
    const auto comb = parse_choice<video::CombMode>("comb-mode", comb_mode_,
        {{"off", video::CombMode::off}, {"post", video::CombMode::post}, {"delay-line", video::CombMode::delay_line},
            {"glass", video::CombMode::glass}});
    if (!comb)
      return std::nullopt;
    dc.chroma.comb_mode = *comb;
  }

  dc.sep.slice_depth = slice_depth_;
  dc.sep.sync_level = sync_level_;
  dc.hsweep.pll_kp = h_kp_;
  dc.hsweep.pll_ki = h_ki_;
  dc.hsweep.acq_kp = h_acq_kp_;
  dc.hsweep.acq_ki = h_acq_ki_;
  dc.hsweep.omega_clamp = h_clamp_;
  dc.vsync.vsync_level = v_level_;
  dc.vsync.pll_kp = v_kp_;
  dc.vsync.pll_ki = v_ki_;
  dc.vsync.integrator_tc_lines = v_tc_;
  dc.vsync.min_field_fraction = v_minfield_;
  return dc;
}

} // namespace palindrome::cli
