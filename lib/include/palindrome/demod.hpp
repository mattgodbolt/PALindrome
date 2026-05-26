#pragma once

#include <complex>
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
// I/Q components, and outputs their magnitude — i.e. the AM envelope.
//
// NOTE: the low-pass is a single one-pole IIR per component. That is a cheap
// placeholder good enough to eyeball the signal; it is a gentle 6 dB/octave
// roll-off, not a brick wall, so expect aliasing of out-of-band energy. A
// proper FIR/decimating filter is future work.
class AmEnvelope {
public:
  // sample_rate_hz: input sample rate. carrier_hz: IF carrier to demodulate.
  // cutoff_hz: low-pass corner for the recovered baseband; must be in
  // (0, sample_rate_hz / 2). Throws std::invalid_argument otherwise.
  AmEnvelope(double sample_rate_hz, double carrier_hz, double cutoff_hz);

  // Demodulate `in`, appending one output sample per input sample to `out`.
  void process(std::span<const float> in, std::vector<float> &out);

private:
  std::complex<double> phasor_{1.0, 0.0}; // running down-conversion phase
  std::complex<double> step_{1.0, 0.0};   // per-sample phase rotation
  double i_{};                            // low-pass state, in-phase
  double q_{};                            // low-pass state, quadrature
  double alpha_{};                        // one-pole smoothing coefficient
  unsigned since_renorm_{};               // samples since last phasor renormalise
};

} // namespace palindrome::demod
