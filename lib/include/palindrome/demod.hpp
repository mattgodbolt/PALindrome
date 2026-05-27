#pragma once

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
  AmEnvelope(double sample_rate_hz, double carrier_hz, double cutoff_hz, std::size_t num_taps = 127,
      dsp::Window window = dsp::Window::Hamming, std::size_t decimation = 1);

  // Demodulate `in`, appending one output sample per `decimation()` inputs to
  // `out`.
  void process(std::span<const float> in, std::vector<float> &out);

  [[nodiscard]] std::size_t decimation() const { return i_filter_.decimation(); }

private:
  std::complex<double> phasor_{1.0, 0.0}; // running down-conversion phase
  std::complex<double> step_{1.0, 0.0}; // per-sample phase rotation
  dsp::Fir i_filter_; // baseband low-pass, in-phase
  dsp::Fir q_filter_; // baseband low-pass, quadrature
  unsigned since_renorm_{}; // samples since last phasor renormalise

  // Scratch reused across process() calls to avoid per-block reallocation.
  std::vector<float> mixed_i_;
  std::vector<float> mixed_q_;
  std::vector<float> filtered_i_;
  std::vector<float> filtered_q_;
};

} // namespace palindrome::demod
