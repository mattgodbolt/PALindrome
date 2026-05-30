#pragma once

#include "palindrome/buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Video reconstruction as a branching, streaming filter graph, modelled on the
// analog TV signal path. The demodulated composite fans out the way it does in
// a real set — to the sync separator AND to the picture tube — so the picture
// (brightness) rail and the timing rail are separate branches that rejoin at
// the renderer:
//
//   envelope --+--> [SyncSeparator] --> [HorizontalSweep] ---+--> [FrameRenderer]
//              |                          (timing rail)      |
//              +------------ picture rail (envelope) --------+
//
// Each filter stage is a streaming block (prepare / process(span)->span) with
// state carried across calls, so output is independent of how the input is
// chunked — which is what lets a driver pump fixed-size blocks through the whole
// graph with bounded memory (the target being live RF, not finite files). The
// renderer is a join SINK: two aligned inputs in, a framebuffer accumulated, no
// span out. Because the branches don't decimate (decimation happens upstream in
// the vision chain, before the fan-out), the two rails stay sample-aligned and
// the join is a plain zip.
namespace palindrome::video {

// === Sync separator ===

// Output of the sync separator: just the sliced one-bit sync signal — true
// while the envelope sits in the sync region (above the slice level, since
// vision is negatively modulated so sync tips are the envelope peaks). True
// during EVERY sync pulse (line sync and the vertical-interval broad /
// equalising pulses alike); telling them apart is downstream's job. It is a
// struct, not a bare bool, so the sandcastle-style gating levels (burst gate,
// clamp, blanking) can join it later without changing the stage signature.
struct SyncSample {
  bool sync = false;
};

struct SyncSeparatorConfig {
  double sample_rate_hz;
  // Slice level as a fraction of the tracked floor(white) -> peak(sync-tip)
  // range. PAL puts the sync tip at 100% of the carrier and black at ~75%, so
  // a slice around 0.85 sits safely inside the sync region.
  double sync_level = 0.85;
};

// Slices the composite envelope into a clean one-bit sync signal. Tracks a
// running peak (sync tip) and floor (active-video white) so the slice point
// follows the recording's amplitude, with hysteresis so chroma ripple on a
// transition doesn't chatter the output. No timing loop here — it just slices.
class SyncSeparator {
public:
  explicit SyncSeparator(const SyncSeparatorConfig &cfg);

  void prepare(std::size_t max_in);
  [[nodiscard]] std::span<const SyncSample> process(std::span<const float> envelope);

  [[nodiscard]] std::size_t max_output_for(std::size_t n) const noexcept { return n; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

private:
  SyncSeparatorConfig cfg_;
  double peak_ = 0.0; // running sync-tip level
  double floor_ = 0.0; // running white level
  bool sync_ = false; // current sliced state (hysteresis)
  bool seeded_ = false; // peak/floor seeded from the first sample yet?
  Buffer<SyncSample> out_;
};

// === Horizontal sweep (AFC + flywheel) ===

// Output of the horizontal sweep: the beam's horizontal position as a phase in
// [0, 1) (0 at the locked line start), and line_start true on the single sample
// where the sweep locks a new line. The envelope is NOT here — it rides the
// picture branch; the renderer joins the two rails by index.
struct BeamSample {
  float h_phase = 0.0f;
  bool line_start = false;
};

struct HorizontalSweepConfig {
  double sample_rate_hz;
  double nominal_line_hz = 15625.0;
  // Pulse-width window the AFC accepts, as fractions of a line. Line sync is
  // ~4.7 us (~0.073 line); equalising pulses ~2.35 us (~0.037); broad pulses
  // ~27 us (~0.43). The (0.05, 0.15) window passes line sync and rejects both
  // vertical-interval pulse kinds — the period-correct pulse-width slicer.
  double min_pulse_fraction = 0.05;
  double max_pulse_fraction = 0.15;
  // Horizontal hold: reject a sync edge arriving sooner than this fraction of
  // a line after the last accepted one (chroma-ripple retriggers, etc.). The
  // flywheel free-runs across the gap, which is the whole point of a flywheel.
  double min_line_fraction = 0.85;
  // AFC PI loop. kp snaps phase to the sync anchor; ki tracks the recording's
  // true line rate. omega is anti-windup clamped to +/- omega_clamp of nominal
  // so a sustained biased error during acquisition can't drive it to zero
  // (which would invert the lockout-gap formula min_line_fraction / omega).
  double pll_kp = 1.0;
  double pll_ki = 1.0e-5;
  double omega_clamp = 0.2;
};

// The horizontal timebase: a free-running phase oscillator (the flywheel)
// pulled into lock by a PI loop on the line-sync edges the separator found.
// Pulse-width and horizontal-hold gating keep vertical-interval pulses and
// chroma retriggers out of the loop; between accepted edges the oscillator
// coasts, so dropouts and rejected pulses don't disturb the sweep.
class HorizontalSweep {
public:
  explicit HorizontalSweep(const HorizontalSweepConfig &cfg);

  void prepare(std::size_t max_in);
  [[nodiscard]] std::span<const BeamSample> process(std::span<const SyncSample> in);

  [[nodiscard]] std::size_t max_output_for(std::size_t n) const noexcept { return n; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }

  // Diagnostics: accepted edges drove the AFC; rejected edges were gated out;
  // omega is the current oscillator rate in cycles/sample (× input rate = the
  // locked line rate in Hz).
  [[nodiscard]] std::size_t accepted_edges() const noexcept { return accepted_; }
  [[nodiscard]] std::size_t rejected_edges() const noexcept { return rejected_; }
  [[nodiscard]] double omega() const noexcept { return omega_; }

private:
  HorizontalSweepConfig cfg_;
  double omega_; // cycles/sample, == nominal_line_hz / sample_rate_hz at construction
  double phase_ = 0.0; // [0, 1), advances by omega_ each sample
  double leading_edge_phase_ = 0.0; // phase_ captured at the current pulse's leading edge
  bool prev_sync_ = false; // sync bit at the previous sample, for edge detection
  std::size_t sample_index_ = 0; // total samples across all process() calls
  std::size_t leading_edge_sample_ = 0; // sample_index_ at the current pulse's leading edge
  std::size_t last_accepted_sample_ = 0; // sample_index_ of the last accepted line-sync edge
  bool have_accepted_ = false; // any edge accepted yet? (the hold gate has no prior edge before the first)
  std::size_t accepted_ = 0;
  std::size_t rejected_ = 0;
  Buffer<BeamSample> out_;
};

// === Frame renderer (the join sink) ===

struct FrameRendererConfig {
  std::size_t width;
  std::size_t lines;
  std::size_t start_line = 0; // skip this many locked lines before the top row
};

// The picture tube: a join sink that paints the brightness rail (envelope) at
// the beam position the timing rail (BeamSample) supplies. Streaming — it is
// fed aligned (envelope, beam) blocks and accumulates into a framebuffer,
// counting locked lines off line_start as they arrive (no pre-scan). Pixels
// over-sampled by more than one envelope sample are accumulate-meaned; contrast
// is stretched over the painted window. finish() maps the accumulators to grey.
class FrameRenderer {
public:
  explicit FrameRenderer(const FrameRendererConfig &cfg);

  void prepare(std::size_t max_in);
  void process(std::span<const float> envelope, std::span<const BeamSample> beam);

  struct Frame {
    std::vector<std::uint8_t> grey; // width*lines, row-major
    std::size_t width;
    std::size_t lines;
    std::size_t painted_lines; // rows that actually received samples
    std::size_t total_locked_lines; // lines the sweep locked across the whole stream
  };
  [[nodiscard]] Frame finish() const;

private:
  FrameRendererConfig cfg_;
  std::vector<float> sum_; // per-pixel envelope sum
  std::vector<std::uint32_t> count_; // per-pixel sample count
  float hi_; // running max envelope over painted samples
  float lo_; // running min envelope over painted samples
  bool have_line_ = false; // seen the first line_start yet?
  std::size_t line_index_ = 0; // current locked-line index (0 at first line_start)
  std::size_t total_locked_ = 0; // line_starts seen so far
};

} // namespace palindrome::video
