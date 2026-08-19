#include "palindrome/cpu_deposit.hpp"

#include "palindrome/pow01.hpp"
#include "palindrome/restrict_ptr.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace palindrome::video {

namespace {
// The readout kernels take restrict_ptr because the byte stores would
// otherwise be assumed to alias everything (char aliases all) - including the
// framebuffer and the vectors' own control blocks - which blocks the
// autovectorisation both loops rely on. pow01 rather than std::pow in the
// encoded kernel for the same reason: an errno-setting libm call pins the
// loop scalar (issue #60).
void readout_linear(restrict_ptr<const float> bright, restrict_ptr<std::uint8_t> px, std::size_t n, float scale) {
  for (std::size_t idx = 0; idx < n; ++idx)
    px[idx] = static_cast<std::uint8_t>(std::clamp(bright[idx] * scale, 0.0f, 255.0f) + 0.5f);
}

void readout_encoded(
    restrict_ptr<const float> bright, restrict_ptr<std::uint8_t> px, std::size_t n, float scale, float inv) {
  for (std::size_t idx = 0; idx < n; ++idx) {
    const float lit = std::clamp(bright[idx] * scale, 0.0f, 1.0f);
    px[idx] = static_cast<std::uint8_t>(255.0f * dsp::pow01(lit, inv) + 0.5f);
  }
}
} // namespace

CpuDepositBackend::CpuDepositBackend(
    std::size_t width, std::size_t height, std::size_t channels, std::size_t lanes, SplatKernels kernels) :
    width_{width}, height_{height}, channels_{channels}, deposit_{width, height, channels, lanes, std::move(kernels)},
    bright_(width * height * channels, 0.0f) {}

void CpuDepositBackend::prepare(std::size_t max_records) {
  pending_.reserve(max_records);
  deposit_.prepare(max_records); // size the deposit's index buffer for a field
}

std::span<SplatRecord> CpuDepositBackend::acquire(std::size_t max) {
  // The tail beyond the committed run. Grow (doubling) only if the committed
  // run plus this request overran the prepare() budget - a caller that skipped
  // prepare, e.g. a test, starts from nothing. Nothing is acquired across the
  // reserve, so the relocation invalidates no live span.
  const auto committed = pending_.size();
  const auto need = committed + max;
  if (need > pending_.capacity())
    pending_.reserve(std::max(need, pending_.capacity() ? pending_.capacity() * 2 : std::size_t{1} << 16));
  return {pending_.data() + committed, max};
}

void CpuDepositBackend::commit(std::size_t n) {
  // write_n only moves the logical size (no copy, no zero-fill); the records
  // are already in place, written through the acquired span.
  static_cast<void>(pending_.write_n(pending_.size() + n));
}

void CpuDepositBackend::apply_pending() {
  if (pending_.empty())
    return;
  deposit_.apply(pending_.view(), bright_);
  pending_.clear();
}

void CpuDepositBackend::end_field(float decay) {
  apply_pending();
  for (float &b: bright_)
    b *= decay;
}

void CpuDepositBackend::latch() {
  apply_pending();
  latch_bright_.assign(bright_.begin(), bright_.end());
}

Frame CpuDepositBackend::quantise(const std::vector<float> &bright, const Readout &ro) const {
  // Scale by the white reference (the steady-state phosphor brightness a
  // full-white pixel reaches), one shared scale so hue is preserved. Cells
  // above it clip into white, as a real tube does - no per-frame statistic, so
  // the exposure is causal and doesn't breathe.
  std::vector<std::uint8_t> pixels(bright.size());
  if (ro.gamma == 1.0) {
    // Linear readout: the raw phosphor light, straight scale-and-quantise (the
    // buffer already holds the displayed charge; decay is applied per field).
    const float scale = ro.white > 0.0 ? static_cast<float>(255.0 * ro.gain / ro.white) : 0.0f;
    readout_linear(bright.data(), pixels.data(), bright.size(), scale);
  }
  else {
    // The "camera": encode the linear light for a display that will decode it
    // with ro.gamma, so the viewer sees the phosphor's light and not a
    // double-gamma'd version. Once per emitted frame, not per sample.
    const float scale = ro.white > 0.0 ? static_cast<float>(ro.gain / ro.white) : 0.0f;
    readout_encoded(bright.data(), pixels.data(), bright.size(), scale, static_cast<float>(1.0 / ro.gamma));
  }
  return Frame{.pixels = std::move(pixels), .width = width_, .height = height_, .channels = channels_};
}

void CpuDepositBackend::readout(const Readout &ro, FrameSink sink) {
  apply_pending();
  sink(quantise(bright_, ro));
}

void CpuDepositBackend::readout_latched(const Readout &ro, FrameSink sink) {
  if (latch_bright_.empty()) {
    readout(ro, std::move(sink));
    return;
  }
  sink(quantise(latch_bright_, ro));
}

} // namespace palindrome::video
