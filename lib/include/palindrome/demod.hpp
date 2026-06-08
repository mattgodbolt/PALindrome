#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/fir.hpp"

#include <complex>
#include <cstddef>
#include <span>
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

// Envelope (AM) demodulator for an analytic (one-sided complex) signal, with the
// vision carrier at some offset from DC. It blocks DC, mixes that offset to DC (a
// complex multiply), low-pass filters the I/Q with a windowed-sinc FIR, and
// outputs the magnitude. The decoder reaches it via Hilbert (real IF -> analytic),
// so this is the one envelope path for every recording. A cutoff above the chroma
// offset (~4.8 MHz) keeps the subcarrier; below it (~3 MHz) gives luma only.
class ComplexAmEnvelope {
public:
  // carrier_offset_hz: the vision carrier's offset from DC (signed). cutoff_hz:
  // baseband low-pass corner, in (0, sample_rate_hz / 2). num_taps sets the FIR
  // length; decimation drops the output rate to sample_rate_hz / decimation (keep
  // cutoff_hz below the decimated Nyquist). Throws std::invalid_argument on bad
  // parameters.
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
  // Complex one-pole DC blocker on the input: kills the zero-IF LO-leakage
  // spike at DC, which would otherwise mix onto the carrier and beat into the
  // envelope (the AirSpy's equivalent of the real path's dsp::DcBlocker).
  std::complex<double> dc_prev_in_{0.0, 0.0};
  std::complex<double> dc_prev_out_{0.0, 0.0};
  dsp::Fir i_filter_; // baseband low-pass, in-phase
  dsp::Fir q_filter_; // baseband low-pass, quadrature
  Buffer<float> mix_i_; // scratch: mixed I before filtering
  Buffer<float> mix_q_; // scratch: mixed Q before filtering
  Buffer<float> out_; // owned envelope output, reused across calls
};

// Forms the analytic (one-sided, complex) signal of a real-sampled input via a
// Type III Hilbert FIR: the quadrature plane is the Hilbert transform of the
// input, the in-phase plane is the input delayed by the FIR's group delay so the
// two align. The result has only positive-frequency content, so a real-IF
// recording can feed ComplexAmEnvelope just like the AirSpy's native complex
// baseband — one demodulator for both. This matters because the real envelope's
// real->I/Q mixer leaves the carrier's negative-frequency image to be removed by
// the low-pass, which only works when the carrier sits high enough that the image
// (at -2*carrier after the mix) clears the chroma cutoff: RX888 vision at 3.56 MHz
// and the AirSpy raw capture's ~3 MHz both do. Making the signal analytic deletes
// that image outright, so the carrier placement isn't load-bearing. The
// quadrature FIR runs as a polyphase even/odd pair (a Type III kernel's non-zero
// taps occupy one array parity, so each output depends on only one input parity),
// parity-routed by a global sample count; that and the matched delay are
// sample-order recurrences, so this stage is bit-exact block-invariant.
class Hilbert {
public:
  // num_taps: Hilbert FIR length. Must be ≡ 3 (mod 4) — odd, for an integer group
  // delay (num_taps-1)/2, and with the kernel's non-zero taps on even array
  // indices, which the polyphase split assumes (the natural 2^k-1 lengths qualify;
  // see the implementation). Throws std::invalid_argument otherwise.
  explicit Hilbert(std::size_t num_taps = kDefaultVisionTaps, dsp::Window window = dsp::Window::Hamming);

  void prepare(std::size_t max_in);

  // Map `in` (real) to the analytic complex signal, one output per input. The
  // returned span is owned by the stage, valid until the next process() call.
  [[nodiscard]] std::span<const std::complex<float>> process(std::span<const float> in);

  [[nodiscard]] std::size_t max_output_for(std::size_t n_in) const noexcept { return n_in; }
  [[nodiscard]] std::size_t input_multiple() const noexcept { return 1; }
  // The analytic output trails the input by this many samples (the FIR group
  // delay, which the I and Q planes share).
  [[nodiscard]] std::size_t group_delay_samples() const noexcept { return delay_; }

private:
  // Polyphase quadrature: a Type III Hilbert kernel's even-offset taps are all
  // zero, so Q[n] depends only on the same-parity input samples. Split the input
  // into its even- and odd-index streams and run each (half-rate) through the
  // half-length even-tap kernel — the same multiply-adds as the full kernel minus
  // the structural zeros, in the same order, so bit-identical at ~half the work.
  dsp::Fir q_even_; // even-tap kernel over the even-index input samples
  dsp::Fir q_odd_; // ... and over the odd-index samples
  Buffer<float> even_in_; // scratch: the deinterleaved even-index input half
  Buffer<float> odd_in_; // scratch: the deinterleaved odd-index input half
  std::vector<float> i_history_; // last `delay_` inputs, carried for the matched in-phase delay
  std::size_t delay_; // (num_taps - 1) / 2: the FIR group delay the I plane matches
  std::size_t samples_seen_ = 0; // global input count, for each block's even/odd parity
  Buffer<std::complex<float>> out_; // owned analytic output, reused across calls
};

} // namespace palindrome::demod
