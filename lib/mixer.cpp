#include "palindrome/mixer.hpp"

#include "palindrome/cmul.hpp"

#include <cmath>
#include <numbers>

namespace palindrome::dsp {

namespace {
constexpr double two_pi = 2.0 * std::numbers::pi;
} // namespace

Mixer::Mixer(double carrier_hz, double sample_rate_hz) {
  // Mixing by e^{-i*omega} shifts +carrier down to DC, one step per sample.
  const double omega = two_pi * carrier_hz / sample_rate_hz;
  step_ = std::polar(1.0, -omega);
  std::complex<double> rot{1.0, 0.0};
  for (std::size_t l = 0; l < kLanes; ++l) {
    rot_re_[l] = static_cast<float>(rot.real());
    rot_im_[l] = static_cast<float>(rot.imag());
    rot = cmul(rot, step_);
  }
  step_block_ = rot; // step^kLanes, having stepped kLanes times above
}

void Mixer::mix_group(restrict_ptr<const float> x, restrict_ptr<float> i, restrict_ptr<float> q,
    restrict_ptr<const float> rot_re, restrict_ptr<const float> rot_im, float br, float bi) {
  for (std::size_t l = 0; l < kLanes; ++l) {
    const float re = br * rot_re[l] - bi * rot_im[l];
    const float im = br * rot_im[l] + bi * rot_re[l];
    i[l] = x[l] * re;
    q[l] = x[l] * im;
  }
}

void Mixer::prepare(std::size_t max_in) {
  i_out_.reserve(max_in);
  q_out_.reserve(max_in);
}

Mixer::Iq Mixer::process(std::span<const float> in) {
  const std::size_t n = in.size();
  const auto *x = in.data();
  auto *ip = i_out_.write_n(n).data();
  auto *qp = q_out_.write_n(n).data();
  const auto *rre = rot_re_.data();
  const auto *rim = rot_im_.data();

  // Full groups: one scalar base phasor feeds a whole vector of elementwise
  // mixes via the constant per-lane rotation table. Forming the phasor inline
  // (base*rot) fuses into the multiply by x, so the compiler emits FMAs and
  // never materialises the phasor. Holding the phasor per-lane instead — to drop
  // the base*rot — splits this into a mix pass plus a separate complex-multiply
  // advance pass that can't fuse: benchmarked ~30% slower, so it stays as is.
  std::size_t k = 0;
  for (; k + kLanes <= n; k += kLanes) {
    mix_group(x + k, ip + k, qp + k, rre, rim, base_.real_f(), base_.imag_f());
    base_.advance(step_block_);
  }

  // Tail: fewer than kLanes samples. Mix each from the same fixed base — the
  // snapshot (br, bi), never base_ itself — and advance base_ one step per sample
  // in the same pass, so its phase stays exact for the next call.
  const auto r = n - k;
  const auto br = base_.real_f();
  const auto bi = base_.imag_f();
  for (std::size_t l = 0; l < r; ++l) {
    ip[k + l] = x[k + l] * (br * rre[l] - bi * rim[l]);
    qp[k + l] = x[k + l] * (br * rim[l] + bi * rre[l]);
    base_.advance(step_);
  }

  return {i_out_.view(), q_out_.view()};
}

} // namespace palindrome::dsp
