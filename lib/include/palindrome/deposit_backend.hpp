#pragma once

#include "palindrome/splat.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace palindrome::video {

// An 8-bit readout of the phosphor: the picture as a viewer's camera sees it.
struct Frame {
  std::vector<std::uint8_t> pixels; // width*height*channels, row-major
  std::size_t width;
  std::size_t height;
  std::size_t channels; // 1 = grey, 3 = interleaved RGB
};

// How the phosphor's linear light maps onto the 8-bit frame. white is the light
// level that reads as full scale (the white reference times the phosphor's
// steady-state gain); gain is the readout-side contrast (the pot, in tracked-
// white mode; 1 otherwise) applied above white; gamma is the encode exponent
// for the display that will decode the frame (1 = linear, the raw light).
struct Readout {
  double white;
  double gain;
  double gamma;
};

// The phosphor side of the Screen: where the splats land, fade, and are read
// out. The Screen's per-sample control pass produces a stream of SplatRecords
// and hands them over in slabs; everything from there on - accumulating them
// into the framebuffer, the per-field fade, and the quantised readout - is the
// backend's. The CPU backend (CpuDepositBackend) is the reference: its output
// is what the goldens are pinned to. A GPU backend can batch the slabs into
// uploads and read back asynchronously, which is why readout delivers the
// Frame through a callback rather than returning it: the CPU backend calls
// back synchronously, before readout returns; an asynchronous backend may call
// back later, from its own completion.
class DepositBackend {
public:
  virtual ~DepositBackend() = default;

  using FrameSink = std::function<void(Frame)>;

  // The beam-spot kernels the records index by (class, bin). The tables must
  // outlive the backend. Call before the first enqueue.
  virtual void set_kernels(const SplatKernels &kernels) = 0;
  // Size the record staging for the largest batch between two end_field()s, so
  // the streaming path never allocates.
  virtual void prepare(std::size_t max_records) = 0;
  // A slab of records, in sample order. The backend may apply them now or
  // batch them; they are landed by the next end_field(), latch() or readout.
  virtual void enqueue(std::span<const SplatRecord> records) = 0;
  // The field boundary: land anything pending, then fade the whole phosphor by
  // decay (a multiply) before the next field paints on top.
  virtual void end_field(float decay) = 0;
  // Copy the phosphor as it stands now aside, for readout_latched() later.
  virtual void latch() = 0;
  // Quantise the live phosphor (after landing anything pending) / the latched
  // copy, delivering the Frame to sink.
  virtual void readout(const Readout &readout, const FrameSink &sink) = 0;
  virtual void readout_latched(const Readout &readout, const FrameSink &sink) = 0;
};

} // namespace palindrome::video
