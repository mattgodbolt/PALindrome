#pragma once

#include "palindrome/chroma_decoder.hpp"
#include "palindrome/fir.hpp"
#include "palindrome/horizontal_sweep.hpp"
#include "palindrome/screen.hpp"
#include "palindrome/sync_separator.hpp"
#include "palindrome/vertical_sync.hpp"
#include "palindrome/video_types.hpp"

#include <cstddef>
#include <span>
#include <vector>

// The video graph as one streaming node, wiring the separator, the two sweeps,
// the chroma decoder and the screen into the branching pipeline (see the per-stage
// headers). This is the only place that depends on all the stages at once.
namespace palindrome::video {

struct DecoderConfig {
  double sample_rate_hz;
  std::size_t width;
  std::size_t height;
  // The sync separator slices a low-passed copy of the envelope, not the full
  // one: sync only needs the slow pulse shapes, and the chroma/HF that the
  // picture rail keeps would otherwise chatter the slicer (badly so on a noisy
  // complex-baseband capture). The picture rail is untouched, so this trades
  // nothing on the image. Cutoff is a fraction of a sync pulse's bandwidth.
  double sync_lp_cutoff_hz = 1.2e6;
  // The sub-stage configs, so every knob is reachable from one place (the
  // interactive tuner, and eventually live control). Their sample_rate_hz is
  // filled in from this config's at construction, so leave it 0 here.
  SyncSeparatorConfig sep{};
  HorizontalSweepConfig hsweep{};
  VerticalSyncConfig vsync{};
  // Phosphor persistence, in field periods (see ScreenConfig). Higher evens out
  // the brightness between the two interlaced fields; lower sharpens motion.
  double persistence_fields = 1.2;
  // Beam-spot size (see ScreenConfig::beam_sigma_rows/cols). cols < 0 => round.
  double beam_sigma_rows = 0.8;
  double beam_sigma_cols = -1.0;
  double gamma = 1.0; // electron-gun gamma (see ScreenConfig::gamma)
  // Colour: decode chroma and render an RGB triad. Off => the grey rail (the
  // luma envelope straight to the screen, chroma untouched). saturation scales
  // the colour-difference signals into the gun matrix (see ScreenConfig).
  bool colour = false;
  double saturation = 1.0;
  double contrast = 1.0; // readout white point (see ScreenConfig::contrast)
  double h_blank = 0.16; // retrace blanking end, h_phase (see ScreenConfig::h_blank)
  ChromaDecoderConfig chroma{}; // sample_rate_hz filled in at construction
};

// The decode stages' output for one block: the picture rail plus the two timing
// rails, copied out of the per-stage buffers into owned vectors. Self-contained,
// so it can be handed to the screen deposit on another thread — the unit that
// flows down the (eventual) stage pipeline.
struct DecodedBlock {
  std::vector<ChromaSample> picture;
  std::vector<BeamSample> hbeam;
  std::vector<VSample> vbeam;
  void resize(std::size_t n) {
    picture.resize(n);
    hbeam.resize(n);
    vbeam.resize(n);
  }
  [[nodiscard]] std::size_t size() const noexcept { return picture.size(); }
};

// The whole video graph as one streaming node: it owns the separator, the two
// sweeps and the screen, and hides the fan-out (the sync bit feeds both
// timebases) and the 3-way join (the screen). Feed it envelope blocks; read a
// frame off snapshot(). This is the reified branching graph — the wiring lives
// here, once, instead of in each consumer's block lambda.
class Decoder {
public:
  explicit Decoder(const DecoderConfig &cfg);

  void prepare(std::size_t max_in);
  // The decode is split into two halves so a pipeline can run them on separate
  // threads: decode_into() runs the sync/chroma stages into a caller-owned block;
  // deposit() paints that block onto the phosphor screen, firing on_field (if
  // set) at each field boundary. Run back to back they are a full decode.
  void decode_into(DecodedBlock &out, std::span<const float> envelope);
  void deposit(const DecodedBlock &block, const Screen::FieldCallback &on_field = {});
  [[nodiscard]] Screen::Frame snapshot() const { return screen_.snapshot(); }
  [[nodiscard]] Screen::Frame latched_frame() const { return screen_.latched_frame(); }

  [[nodiscard]] std::size_t accepted_edges() const noexcept { return hsweep_.accepted_edges(); }
  [[nodiscard]] std::size_t rejected_edges() const noexcept { return hsweep_.rejected_edges(); }
  [[nodiscard]] double line_omega() const noexcept { return hsweep_.omega(); }
  [[nodiscard]] bool hold_locked() const noexcept { return hsweep_.locked(); }
  [[nodiscard]] std::size_t detected_fields() const noexcept { return vsync_.detected_fields(); }
  [[nodiscard]] double field_omega() const noexcept { return vsync_.omega(); }
  // Colour diagnostics (meaningful only when colour is enabled).
  [[nodiscard]] double subcarrier_hz() const noexcept { return chroma_.subcarrier_hz(); }
  [[nodiscard]] double burst_amplitude() const noexcept { return chroma_.burst_amplitude(); }
  [[nodiscard]] double burst_swing_deg() const noexcept { return chroma_.burst_swing_deg(); }
  [[nodiscard]] double killer_gain() const noexcept { return chroma_.killer_gain(); }

private:
  bool colour_;
  dsp::Fir sync_lp_; // narrow low-pass on the sync branch only (picture rail untouched)
  SyncSeparator sep_;
  HorizontalSweep hsweep_;
  VerticalSync vsync_;
  ChromaDecoder chroma_;
  Screen screen_;
};

} // namespace palindrome::video
