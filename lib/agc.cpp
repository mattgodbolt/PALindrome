#include "palindrome/agc.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>

#if defined(__AVX512F__)
#include <immintrin.h>
#endif

namespace palindrome::video {

namespace {

// The serial reference: one multiply and one max per sample, both on the
// loop-carried chain. Also the tail loop under the vector path.
double process_scalar(const float *env, float *dst, std::size_t n, double release, double tip) {
  for (std::size_t k = 0; k < n; ++k) {
    tip = std::max(static_cast<double>(env[k]), tip * release);
    // Snapshot the control down to float at the point of use; the tip itself
    // keeps accumulating in double.
    const auto scale = static_cast<float>(tip > 0.0 ? 1.0 / tip : 0.0);
    dst[k] = env[k] * scale;
  }
  return tip;
}

#if defined(__AVX512F__)
// Unrolled, tip[k] = max(env[k], r*env[k-1], r*(r*env[k-2]), ..., r^(k+1) tip_in),
// each candidate decayed by *repeated* multiplication - the same sequence of
// roundings the serial loop performs - and because rounding is monotone,
// r * max(a, b) == max(r*a, r*b) exactly, so the max may be regrouped freely:
// every lane of a block can gather its own candidates and the result is
// bit-identical to the recurrence. What cannot be regrouped is the decay chain
// itself (fl(fl(x*r)*r) != fl(x*fl(r*r))), so the carried tip still advances
// one scalar multiply per sample; that 4-cycle chain is the floor, but it is
// now the *only* thing on the critical path - the divide, the in-block scan and
// the output all run in vector lanes beside it instead of serialised behind
// the per-sample max.
constexpr std::size_t kLanes = 8;

double process_blocks(const float *env, float *dst, std::size_t n, double release, double tip) {
  const auto r = _mm512_set1_pd(release);
  const auto zero = _mm512_setzero_pd();
  const auto one = _mm512_set1_pd(1.0);
  for (std::size_t k = 0; k + kLanes <= n; k += kLanes) {
    const auto e = _mm256_loadu_ps(env + k);
    const auto e0 = _mm512_cvtps_pd(e);

    // In-block candidates: after d shift-and-decay steps lane m holds
    // r^d env[m-d]; lanes m < d would reach back before the block and are
    // masked out (the carry supplies those candidates).
    auto best = e0;
    auto shifted = e0;
#pragma GCC unroll 8
    for (std::size_t d = 1; d < kLanes; ++d) {
      shifted = _mm512_mul_pd(
          _mm512_castsi512_pd(_mm512_alignr_epi64(_mm512_castpd_si512(shifted), _mm512_castpd_si512(zero), 7)), r);
      best = _mm512_mask_max_pd(best, static_cast<__mmask8>(0xFFu << d), best, shifted);
    }

    // The carry: the incoming tip decayed once per lane, in sample order.
    const double c0 = tip * release;
    const double c1 = c0 * release;
    const double c2 = c1 * release;
    const double c3 = c2 * release;
    const double c4 = c3 * release;
    const double c5 = c4 * release;
    const double c6 = c5 * release;
    const double c7 = c6 * release;
    const auto carry = _mm512_setr_pd(c0, c1, c2, c3, c4, c5, c6, c7);
    const auto tips = _mm512_max_pd(best, carry);

    // Next block's tip: the last lane, taken as scalars so the loop-carried
    // chain is the eight multiplies plus this max and nothing else.
    const auto best_hi = _mm256_extractf128_pd(_mm512_extractf64x4_pd(best, 1), 1);
    const auto best_last = _mm_cvtsd_f64(_mm_unpackhi_pd(best_hi, best_hi));
    tip = std::max(best_last, c7);

    const auto positive = _mm512_cmp_pd_mask(tips, zero, _CMP_GT_OQ);
    const auto scale = _mm512_cvtpd_ps(_mm512_maskz_div_pd(positive, one, tips));
    _mm256_storeu_ps(dst + k, _mm256_mul_ps(e, scale));
  }
  return tip;
}
#endif

} // namespace

Agc::Agc(const AgcConfig &cfg) {
  if (!(cfg.sample_rate_hz > 0.0))
    throw std::invalid_argument{"Agc: sample_rate_hz must be positive"};
  if (!(cfg.nominal_field_hz > 0.0))
    throw std::invalid_argument{"Agc: nominal_field_hz must be positive"};
  if (!(cfg.decay_fields > 0.0))
    throw std::invalid_argument{"Agc: decay_fields must be positive"};
  const double samples_per_field = cfg.sample_rate_hz / cfg.nominal_field_hz;
  release_ = std::exp(-1.0 / (cfg.decay_fields * samples_per_field));
}

void Agc::prepare(std::size_t max_in) { out_.reserve(max_in); }

std::span<const float> Agc::process(std::span<const float> envelope) {
  const std::size_t n = envelope.size();
  const auto dst = out_.write_n(n);

  // Seed from the first sample so the gain is meaningful immediately; the
  // instant attack corrects it the moment the first real sync tip arrives
  // (within a line), so cold start settles in well under a field.
  if (!seeded_ && n > 0) {
    tip_ = static_cast<double>(envelope[0]);
    seeded_ = true;
  }

  std::size_t k = 0;
#if defined(__AVX512F__)
  const std::size_t blocked = n - n % kLanes;
  tip_ = process_blocks(envelope.data(), dst.data(), blocked, release_, tip_);
  k = blocked;
#endif
  tip_ = process_scalar(envelope.data() + k, dst.data() + k, n - k, release_, tip_);

  return out_.view();
}

} // namespace palindrome::video
