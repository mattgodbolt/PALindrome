#pragma once

#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace palindrome {

// Set FTZ (bit 15) and DAZ (bit 6) in MXCSR so subnormal floating-point values
// (float and double alike - MXCSR governs all SSE/AVX arithmetic) flush to zero
// instead of triggering microcode assists. A strong but mistuned signal - a
// smooth envelope with no sync pulses, so the slicer stays silent for minutes
// (noise chatters the slicer and never gets here) - decays the chain's leaky
// integrators (the vertical-sync duty tracker first) into subnormal doubles,
// and every FMA touching one then pays a ~100-cycle assist - measured live at
// 42.5M fp_assist.any per 3 s, halving decode throughput (issue #126). No
// stage operates anywhere near denormal range on a healthy locked signal, so
// flushing changes nothing there. MXCSR is per-thread state: call this at the
// start of every thread that runs signal processing.
inline void flush_denormals_to_zero() {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
  _mm_setcsr(_mm_getcsr() | 0x8040u);
#else
  // Non-x86: no equivalent wired up yet (AArch64 would set FPCR.FZ/FZ16).
#endif
}

} // namespace palindrome
