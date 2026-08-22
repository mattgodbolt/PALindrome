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
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t channels = 0; // 1 = grey, 3 = interleaved RGB
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
// out. The Screen's per-sample control pass writes a stream of SplatRecords
// straight into staging the backend owns (acquire/commit - no copy for any
// backend: the CPU backend hands out the tail of its record buffer, a GPU one
// a mapped upload slot); everything from there on - accumulating them into the
// framebuffer, the per-field fade, and the quantised readout - is the
// backend's. The CPU backend (CpuDepositBackend) is the reference: its output
// is what the goldens are pinned to.
//
// readout delivers the Frame through a callback rather than returning it, so
// an asynchronous backend (a GPU readback) fits the same shape: the CPU backend
// calls the sink synchronously, before readout returns; an asynchronous backend
// may move the sink into its completion and call it later. The Screen's
// synchronous accessors (snapshot / latched_frame / FieldEvent::frame) need a
// synchronous backend - they throw if the sink has not fired by return, and a
// late call lands in heap state the sink owns, harmlessly - and the
// asynchronous FieldEvent form is deliberately left for the GPU stages.
class DepositBackend {
public:
  virtual ~DepositBackend() = default;
  DepositBackend(const DepositBackend &) = delete;
  DepositBackend &operator=(const DepositBackend &) = delete;

  using FrameSink = std::function<void(Frame)>;

  // Size the record staging for the largest run of records between two
  // end_field()s, so the streaming path never allocates.
  virtual void prepare(std::size_t max_records) = 0;
  // Staging for up to max records, in sample order, to be written by the caller
  // then handed over with commit(n <= max). Only one acquired span is live at a
  // time: the span is invalidated by commit(), end_field(), latch(), readout()
  // and readout_latched() (and by the next acquire()); records written but not
  // committed when any of those runs are dropped.
  [[nodiscard]] virtual std::span<SplatRecord> acquire(std::size_t max) = 0;
  // The first n records of the acquired span are a slab, ready to land. The
  // backend may apply them now or batch them; they are landed by the next
  // end_field(), latch() or readout.
  virtual void commit(std::size_t n) = 0;
  // The field boundary: land anything committed, then fade the whole phosphor
  // by decay (a multiply) before the next field paints on top.
  virtual void end_field(float decay) = 0;
  // Copy the phosphor as it stands now (after landing anything committed) aside,
  // for readout_latched() later.
  virtual void latch() = 0;
  // Quantise the live phosphor (after landing anything committed) / the latched
  // copy, delivering the Frame to sink. readout_latched() before any latch()
  // reads the live phosphor, as readout() does.
  virtual void readout(const Readout &ro, FrameSink sink) = 0;
  virtual void readout_latched(const Readout &ro, FrameSink sink) = 0;

protected:
  DepositBackend() = default;
};

} // namespace palindrome::video
