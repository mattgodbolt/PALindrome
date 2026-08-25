#pragma once

#include "palindrome/buffer.hpp"
#include "palindrome/deposit_backend.hpp"
#include "palindrome/splat.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace palindrome::video {

// The reference DepositBackend: a float framebuffer in memory, splats applied
// by SplatDeposit (threaded by output band across `lanes`), faded by a single
// streaming multiply per field, quantised on the calling thread. Slabs are
// batched to the field internally - the band-threaded apply wants a whole
// field of work so the fan/join is paid once per field and every band has
// records - so commit() only extends the pending run, and end_field()'s fade
// is owed rather than applied. Both land, in order (fade, then records), the
// next time the phosphor is actually needed. Bit-exact in the lane count.
//
// That laziness is what makes latch() free: the latched frame is the live
// buffer as it stands, and the fade and records that arrive after it stay
// unapplied, so nothing has to be copied unless the live phosphor is read
// (or a fresh latch supersedes it, which drops the old one). A single-image
// render latches every field and reads only the last, so it never copies.
class CpuDepositBackend final : public DepositBackend {
public:
  CpuDepositBackend(
      std::size_t width, std::size_t height, std::size_t channels, std::size_t lanes, SplatKernels kernels);

  void prepare(std::size_t max_records) override;
  [[nodiscard]] std::span<SplatRecord> acquire(std::size_t max) override;
  void commit(std::size_t n) override;
  void end_field(float decay) override;
  void latch() override;
  void readout(const Readout &ro, FrameSink sink) override;
  void readout_latched(const Readout &ro, FrameSink sink) override;

private:
  void land(); // bring bright_ up to date: the owed fade, then pending_ (idempotent)
  [[nodiscard]] Frame quantise(const std::vector<float> &bright, const Readout &ro) const;

  std::size_t width_;
  std::size_t height_;
  std::size_t channels_; // 1 (grey) or 3 (RGB)
  SplatDeposit deposit_;
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
  // The latched frame: Live = bright_ before the owed fade and pending_ land,
  // Copied = latch_bright_ (made only when land() had to move bright_ on).
  enum class Latch { None, Live, Copied };
  Latch latch_ = Latch::None;
  std::vector<float> latch_bright_;
  std::optional<float> owed_decay_; // end_field()'s fade, applied by the next land()
  // The record staging. [0, size()) is committed and waiting to land; acquire()
  // hands out the capacity beyond that, commit() extends size() over it.
  Buffer<SplatRecord> pending_;
};

} // namespace palindrome::video
