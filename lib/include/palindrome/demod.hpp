#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/fir.hpp"
#include "palindrome/mixer.hpp"
#include "palindrome/pipeline.hpp"

#include <complex>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

// RF demodulation building blocks. Each demodulator is a stateful, streaming
// stage: construct it once, then feed successive blocks of samples through
// process(). Phase and filter state carry across calls, so the result is
// identical whether you push the whole signal in one block or stream it in
// small chunks — which is what lets these stages compose into a pipeline
// (capture -> demod -> sync detect -> ...) and, later, sit behind senders.
namespace palindrome::demod {

// Default windowed-sinc FIR length for the vision low-pass: enough taps for a
// clean ~5 MHz transition without excessive group delay. Shared default across
// the demodulators and the vision-chain recipe.
inline constexpr std::size_t kDefaultVisionTaps = 127;

// Envelope (AM) demodulator for a real-sampled IF, as used for PAL composite
// vision. It mixes the chosen carrier down to baseband, low-pass filters the
// I/Q components with a windowed-sinc FIR, and outputs their magnitude — i.e.
// the AM envelope. It expects a DC-free input (see dsp::DcBlocker): an unblocked
// offset mixes down onto the carrier and beats into the envelope.
class AmEnvelope {
public:
  // sample_rate_hz: input sample rate. carrier_hz: IF carrier to demodulate.
  // cutoff_hz: baseband low-pass corner; must be in (0, sample_rate_hz / 2).
  // num_taps sets the FIR length (sharper transition as it grows, at the cost
  // of group delay). decimation drops the output rate to sample_rate_hz /
  // decimation; keep cutoff_hz below the decimated Nyquist (rate / (2*decim)) to
  // avoid aliasing. Throws std::invalid_argument on bad parameters.
  AmEnvelope(double sample_rate_hz, double carrier_hz, double cutoff_hz, std::size_t num_taps = kDefaultVisionTaps,
      dsp::Window window = dsp::Window::Hamming, std::size_t decimation = 1);

  // Budget internal storage for blocks of up to `max_in` samples (one-time;
  // process() also grows lazily, so this is an optimisation, not a requirement).
  void prepare(std::size_t max_in);

  // Demodulate `in`, returning a view of one output sample per `decimation()`
  // inputs. The returned span is owned by the demodulator and valid only until
  // the next process() call.
  [[nodiscard]] std::span<const float> process(std::span<const float> in);

  [[nodiscard]] std::size_t max_output_for(std::size_t n_in) const noexcept { return i_filter_.max_output_for(n_in); }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return i_filter_.input_multiple(); }
  [[nodiscard]] std::size_t decimation() const noexcept { return i_filter_.decimation(); }

private:
  dsp::Mixer mixer_; // down-converts the carrier to baseband I/Q
  dsp::Fir i_filter_; // baseband low-pass, in-phase
  dsp::Fir q_filter_; // baseband low-pass, quadrature
  Buffer<float> out_; // owned envelope output, reused across calls
};

// Envelope (AM) demodulator for a COMPLEX baseband recording, as the AirSpy
// gives: the signal is already at baseband with the vision carrier a small
// offset from DC. It mixes that offset to DC (a complex multiply), low-pass
// filters the I/Q with a windowed-sinc FIR, and outputs the magnitude. Simpler
// than the real-IF AmEnvelope — the input is already complex, so there's no
// real->I/Q mixer and no DC-blocker dance (the carrier isn't at DC). Cutoff
// below the chroma offset (~3 MHz) gives a luma-only envelope.
class ComplexAmEnvelope {
public:
  // carrier_offset_hz: the vision carrier's offset from DC (signed). cutoff_hz:
  // baseband low-pass corner, in (0, sample_rate_hz / 2). Other parameters as
  // AmEnvelope. Throws std::invalid_argument on bad parameters.
  ComplexAmEnvelope(double sample_rate_hz, double carrier_offset_hz, double cutoff_hz,
      std::size_t num_taps = kDefaultVisionTaps, dsp::Window window = dsp::Window::Hamming, std::size_t decimation = 1);

  void prepare(std::size_t max_in);

  // Demodulate `in`, one output sample per `decimation()` inputs. The returned
  // span is owned by the demodulator, valid until the next process() call.
  [[nodiscard]] std::span<const float> process(std::span<const std::complex<float>> in);

  [[nodiscard]] std::size_t max_output_for(std::size_t n_in) const noexcept { return i_filter_.max_output_for(n_in); }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return i_filter_.input_multiple(); }
  [[nodiscard]] std::size_t decimation() const noexcept { return i_filter_.decimation(); }

private:
  std::complex<double> step_; // e^{-i*omega}; advances the mix phasor one sample
  std::complex<double> phasor_{1.0, 0.0}; // running mix phase, carried across calls
  std::size_t since_renorm_ = 0; // samples since phasor_ was renormalised
  dsp::Fir i_filter_; // baseband low-pass, in-phase
  dsp::Fir q_filter_; // baseband low-pass, quadrature
  Buffer<float> mix_i_; // scratch: mixed I before filtering
  Buffer<float> mix_q_; // scratch: mixed Q before filtering
  Buffer<float> out_; // owned envelope output, reused across calls
};

// The TV-IF strip as a single recipe: optional sound-carrier notch, then a
// DC blocker, then the AmEnvelope. Shared between the inspection commands
// (`demod`, `render`) and anything else that wants the same composite envelope.
struct VisionChainConfig {
  double sample_rate_hz;
  double vision_carrier_hz;
  std::optional<double> sound_trap_hz; // nullopt => no sound trap
  double sound_q = 10.0;
  double cutoff_hz = 5.0e6;
  std::size_t num_taps = kDefaultVisionTaps;
  dsp::Window window = dsp::Window::Hamming;
  std::size_t decimation = 1;
};

// Built chain plus the human-readable bookkeeping a CLI can print: a note per
// inserted trap, and any non-fatal warnings (e.g. cutoff above the decimated
// Nyquist). Owns the Chain by value; move it out if you need to.
struct VisionChain {
  Chain chain;
  std::vector<std::string> trap_notes;
  std::vector<std::string> warnings;
};

// Build the vision IF strip from `cfg`. Throws std::invalid_argument (from the
// underlying stages) on bad parameters.
[[nodiscard]] VisionChain build_vision_chain(const VisionChainConfig &cfg);

} // namespace palindrome::demod
