#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/deposit_backend.hpp"
#include "palindrome/splat.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace palindrome::video {

// The reference DepositBackend: a float framebuffer in memory, splats applied
// by SplatDeposit (threaded by output band across `lanes`), faded by a single
// streaming multiply per field, quantised on the calling thread. Slabs are
// batched to the field internally - the band-threaded apply wants a whole
// field of work so the fan/join is paid once per field and every band has
// records - so enqueue() only stages; the next end_field()/latch()/readout
// lands the batch. Bit-exact in the lane count.
class CpuDepositBackend final : public DepositBackend {
public:
  CpuDepositBackend(std::size_t width, std::size_t height, std::size_t channels, std::size_t lanes);

  void set_kernels(const SplatKernels &kernels) override;
  void prepare(std::size_t max_records) override;
  void enqueue(std::span<const SplatRecord> records) override;
  void end_field(float decay) override;
  void latch() override;
  void readout(const Readout &readout, const FrameSink &sink) override;
  void readout_latched(const Readout &readout, const FrameSink &sink) override;

private:
  void apply_pending(); // land pending_ into bright_, then clear (idempotent)
  [[nodiscard]] Frame quantise(const std::vector<float> &bright, const Readout &readout) const;

  std::size_t width_;
  std::size_t height_;
  std::size_t channels_; // 1 (grey) or 3 (RGB)
  std::size_t lanes_;
  // Phosphor framebuffer. The beam ADDS charge (no per-sample decay); the whole
  // buffer is faded once per field, at end_field(). This is what a viewer's eye
  // integrates - a field paints as one instant, so there's no top-to-bottom
  // brightness ramp from the beam's sweep down the screen, and it's a single
  // streaming multiply (cache- and GPU-friendly) instead of a per-pixel lazy
  // decay keyed on a last-touched timestamp.
  //
  // The alternative is a continuous per-sample decay (each pixel faded forward
  // to the read instant from when it was last hit). That's physically what a
  // CRT does at any single moment - what a fast-shutter camera captures - but it
  // shows the beam-sweep band a human never sees, and it costs a second
  // random-access array plus a per-deposit exp(). To bring that "camera
  // snapshot" look back, the whole removed implementation - last_,
  // sample_index_, the split decay LUTs, decay_for, and the fade-to-now in
  // snapshot() - sits in the commit before "fade the phosphor per field, not per
  // sample" (git log -- lib/video.cpp; show its parent).
  std::vector<float> bright_; // per-pixel-per-channel accumulated phosphor charge
  std::vector<float> latch_bright_; // latch(): the phosphor copied aside (a float memcpy)
  Buffer<SplatRecord> pending_; // the slabs staged since the last landing
  std::unique_ptr<SplatDeposit> deposit_; // built by set_kernels
};

} // namespace palindrome::video
