#include "palindrome/cpu_deposit.hpp"

#include "palindrome/pow01.hpp"
#include "palindrome/restrict_ptr.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
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

CpuDepositBackend::CpuDepositBackend(std::size_t width, std::size_t height, std::size_t channels, std::size_t lanes) :
    width_{width}, height_{height}, channels_{channels}, lanes_{lanes}, bright_(width * height * channels, 0.0f) {}

void CpuDepositBackend::set_kernels(const SplatKernels &kernels) {
  deposit_ = std::make_unique<SplatDeposit>(width_, height_, channels_, lanes_, kernels);
}

void CpuDepositBackend::prepare(std::size_t max_records) {
  pending_.reserve(max_records);
  deposit_->prepare(max_records); // size the deposit's index buffer for a field
}

void CpuDepositBackend::enqueue(std::span<const SplatRecord> records) {
  // Stage the slab; grow if this field overran the prepare() budget (a caller
  // that skipped prepare, e.g. a test, starts from nothing).
  const auto have = pending_.size();
  if (have + records.size() > pending_.capacity())
    pending_.reserve(
        std::max(have + records.size(), pending_.capacity() ? pending_.capacity() * 2 : std::size_t{1} << 16));
  const auto slot = pending_.write_n(have + records.size()).subspan(have);
  std::ranges::copy(records, slot.begin());
}

void CpuDepositBackend::apply_pending() {
  if (pending_.empty())
    return;
  deposit_->apply(pending_.view(), bright_);
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

Frame CpuDepositBackend::quantise(const std::vector<float> &bright, const Readout &readout) const {
  // Scale by the white reference (the steady-state phosphor brightness a
  // full-white pixel reaches), one shared scale so hue is preserved. Cells
  // above it clip into white, as a real tube does - no per-frame statistic, so
  // the exposure is causal and doesn't breathe.
  std::vector<std::uint8_t> pixels(bright.size());
  if (readout.gamma == 1.0) {
    // Linear readout: the raw phosphor light, straight scale-and-quantise (the
    // buffer already holds the displayed charge; decay is applied per field).
    const float scale = readout.white > 0.0 ? static_cast<float>(255.0 * readout.gain / readout.white) : 0.0f;
    readout_linear(bright.data(), pixels.data(), bright.size(), scale);
  }
  else {
    // The "camera": encode the linear light for a display that will decode it
    // with readout.gamma, so the viewer sees the phosphor's light and not a
    // double-gamma'd version. Once per emitted frame, not per sample.
    const float scale = readout.white > 0.0 ? static_cast<float>(readout.gain / readout.white) : 0.0f;
    readout_encoded(bright.data(), pixels.data(), bright.size(), scale, static_cast<float>(1.0 / readout.gamma));
  }
  return Frame{.pixels = std::move(pixels), .width = width_, .height = height_, .channels = channels_};
}

void CpuDepositBackend::readout(const Readout &readout, const FrameSink &sink) {
  apply_pending();
  sink(quantise(bright_, readout));
}

void CpuDepositBackend::readout_latched(const Readout &readout, const FrameSink &sink) {
  sink(quantise(latch_bright_, readout));
}

} // namespace palindrome::video
