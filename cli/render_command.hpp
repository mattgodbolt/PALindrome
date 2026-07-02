#pragma once

#include "palindrome/decoder.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <lyra/lyra.hpp>

namespace palindrome::cli {

// `palindrome render <recording>` — a sync-locked picture out of a recording.
// The signal flows through the analog-style video graph (AM envelope -> sync
// separator -> horizontal sweep + vertical sync -> phosphor screen): the beam
// paints brightness at (x = h_phase*width, y = v_phase*height) onto a decaying
// phosphor. Both timebases lock automatically, so there's no manual framing;
// interlace falls out of the half-line field offset.
class RenderCommand {
public:
  void add_to(lyra::cli &cli, std::function<int()> &action);

private:
  int run() const;
  // The flags -> DecoderConfig mapping (validation included): nullopt after
  // printing the error, so run() just bails. Split out of run(), which
  // otherwise interleaves it with the pipeline plumbing.
  [[nodiscard]] std::optional<video::DecoderConfig> decoder_config(double envelope_rate) const;
  // One source of truth for flag defaults: wherever a flag simply forwards to a
  // library config field, its default comes FROM that field (via kDefaults), so
  // the two can never drift. Literals below are only the deliberate CLI
  // opinions (the period-faithful look over the library's neutral physics) and
  // the negative/zero "unset" sentinels.
  static constexpr video::DecoderConfig kDefaults{};
  std::filesystem::path recording_;
  std::filesystem::path output_;
  double carrier_{0.0}; // 0 => take the vision carrier from metadata (or scan if none)
  bool scan_{false}; // force a signal scan for the carrier even if metadata has one
  bool live_{false}; // decode a continuous stdin SDR stream instead of a recording file
  double sample_rate_{0.0}; // live input real sample rate Hz (the file path had it in metadata)
  // Deliberately below EnvelopeOptions' 5.0 MHz default (which demod/sync keep):
  // the flat-mode picture cutoff that keeps chroma while clearing the sound
  // carrier after decimation.
  double cutoff_{4.8e6};
  double sync_cutoff_{kDefaults.sync_lp_cutoff_hz}; // narrow low-pass on the sync-detection branch
  std::size_t decimate_{0}; // 0 => auto from the sample rate (keep chroma below 0.7*Nyquist)
  std::size_t width_{720};
  std::size_t height_{576};
  double persistence_{kDefaults.screen.persistence_fields}; // phosphor persistence in field periods
  double beam_sigma_{kDefaults.screen.beam_sigma}; // beam-spot vertical sigma in scanline pitches (raster-relative)
  double beam_sigma_x_{kDefaults.screen.beam_sigma_cols}; // beam-spot horizontal size in cols; <0 = match (round)
  double gamma_{2.6}; // electron-gun gamma (a real tube's curve; the source pre-corrects ~1/2.2)
  double readout_gamma_{2.2}; // the PNG "camera" encode; 1.0 = raw linear light
  double overscan_{0.06}; // fraction of the active picture cropped behind the bezel; < 0 = full scan
  double eht_sag_{0.06}; // EHT sag at a sustained full-white load (0 disables)
  double eht_tc_{kDefaults.screen.eht_tc_fields}; // EHT sag/recovery time constant, field periods
  double eht_focus_{kDefaults.screen.eht_focus}; // spot growth at full sag
  double line_pull_{0.003}; // width stretch after a full-white line (0 disables)
  double bcl_{0.7}; // beam-current limiter threshold (average load; 0 disables)
  double bcl_tc_{kDefaults.screen.bcl_tc_fields}; // BCL response, field periods
  double h_shift_{0.0}; // horizontal centring pot: + moves the picture right (h_phase units)
  double v_shift_{0.0}; // vertical centring pot: + moves the picture down (v_phase units)
  // Levels: the front-end AGC scheme and the controls that hang off it.
  // Defaults are PROVISIONAL, tuned so the under-modulated SMS corpus fills
  // the range out of the box; level-set against more sources per issue #46
  // (broadcast-standard modulation wants contrast ~1.0, saturation ~0.17).
  std::string agc_mode_{"sync-tip"}; // "sync-tip" (absolute levels) | "adaptive" (legacy trackers)
  // The IF response: which set's SAW curve the vision carrier passes through.
  std::string if_mode_{"saw80"}; // "saw80" | "saw90" | "flat" (the pre-B2 ideal low-pass)
  std::string detector_{"quasi-sync"}; // "quasi-sync" (TDA-era product) | "envelope" (diode); saw modes
  double sound_notch_db_{-1.0}; // IF sound rejection, positive dB; negative = template default
  double gd_ripple_{-1.0}; // IF group-delay ripple, ns peak; negative = template default
  double slice_depth_{kDefaults.sep.slice_depth}; // sync slice below the AGC'd tip (sync-tip mode)
  double pwl_{kDefaults.screen.pwl_threshold}; // peak-white limiter ceiling, ×standard white drive (0 disables)
  // Colour decode (PAL-D chroma channel). Off => a grey render.
  bool colour_{false};
  // saturation/contrast default per --agc mode (negative = unset): sync-tip
  // gets the pot-up SMS calibration (0.085 / 1.6), adaptive keeps the values
  // every pre-AGC render used (0.17 / 0.85), so a bare --agc adaptive
  // reproduces the legacy output exactly.
  double saturation_{-1.0}; // chroma gain (fraction of white reference)
  double contrast_{-1.0}; // the contrast pot: video gain pre-gamma (sync-tip mode)
  double h_blank_{kDefaults.screen.h_blank}; // retrace blanking end, h_phase
  double subcarrier_{0.0}; // subcarrier crystal Hz; 0 => textbook 4.43361875 MHz
  double uv_bandwidth_{0.0}; // post-demod U/V low-pass corner Hz; 0 => decoder default
  double band_lo_{0.0}; // chroma band-pass edges Hz; 0 => decoder default
  double band_hi_{0.0};
  double burst_gate_lo_{kDefaults.chroma.burst_gate_lo}; // burst gate, h_phase window
  double burst_gate_hi_{kDefaults.chroma.burst_gate_hi};
  bool no_delay_line_{false}; // deprecated alias for --comb-mode off
  std::string comb_mode_; // "off" | "post" | "delay-line" | "glass"; empty => decoder default (post)
  double ref_tc_{kDefaults.chroma.ref_tc_lines}; // APC reference time constant in lines (EMA rate = 1/ref_tc)
  bool no_killer_{false}; // disable the colour killer (no ident-based chroma muting)
  double apc_catch_{kDefaults.chroma.apc_catch_range_hz}; // APC crystal pull catch range Hz (0 = fixed crystal)
  double apc_pull_{kDefaults.chroma.apc_pull}; // APC pull rate (fraction of measured drift per line)
  std::size_t frame_stride_{0}; // 0 => one image; N => a PNG every Nth field boundary
  int frame_fd_{-1}; // live: write raw RGB frames to this fd instead of a PNG (the MJPEG server encodes them)
  bool no_sync_{false}; // debug: naive-fold the envelope, bypassing the sync graph
  bool no_threads_{false}; // decode serially instead of the default stage pipeline
  std::size_t deposit_threads_{kDefaults.screen.deposit_lanes}; // screen-deposit lanes (bit-exact); 1 = serial
  // Sync/sweep hold knobs — the rest of the decoder, surfaced for the tuner.
  double sync_level_{kDefaults.sep.sync_level}; // separator slice level
  double h_kp_{kDefaults.hsweep.pll_kp}; // horizontal hold: locked (flywheel) AFC gains + omega clamp
  double h_ki_{kDefaults.hsweep.pll_ki};
  double h_acq_kp_{kDefaults.hsweep.acq_kp}; // horizontal hold: acquisition gains (pre-coincidence)
  double h_acq_ki_{kDefaults.hsweep.acq_ki};
  double h_clamp_{kDefaults.hsweep.omega_clamp};
  double v_level_{kDefaults.vsync.vsync_level}; // vertical hold (vsync slice + PI)
  double v_kp_{kDefaults.vsync.pll_kp};
  double v_ki_{kDefaults.vsync.pll_ki};
  double v_tc_{kDefaults.vsync.integrator_tc_lines}; // vertical integrator tc
  double v_minfield_{kDefaults.vsync.min_field_fraction}; // min field fraction
};

} // namespace palindrome::cli
