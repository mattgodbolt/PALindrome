# SIMD: non-standard now, `std::simd` later

Two DSP hot paths are hand-vectorised. Both are stop-gaps, knowingly
non-portable, and meant to become `std::simd` once the toolchain allows.

The FIR (`dsp::convolve_strip`) uses AVX2/FMA intrinsics (`<immintrin.h>`,
`_mm256_fmadd_ps`, ...) to carry a strip of output samples in named vector
accumulators across the tap loop, hiding the FMA-latency chain a
single-accumulator dot product stalls on. It's guarded by
`#if defined(__AVX2__) && defined(__FMA__)`; without those (non-x86, or no
AVX2) the scalar `dsp::convolve` - a plain `std::fmaf` dot - is the fallback.
Both sum taps in natural order, so the intrinsic and scalar paths are
bit-identical and the result stays chunking-invariant.

The AM envelope (`demod::envelope_magnitude`) uses a per-function
`[[gnu::optimize("-fno-math-errno", "-fno-trapping-math")]]` so the `sqrt`
lowers to a packed `vsqrtps` without the errno/trap guards. ODR-safe
(anonymous-namespace, single definition) and the precision loss is bounded and
measured - but `[[gnu::optimize]]` is a GCC debug-only feature.

x86-only intrinsics and a GCC-only attribute are both where we don't want to
stay. Plan, when we pick it up: rewrite both in `std::simd` so the lanes are
explicit and portable, needing no target intrinsics or FP-relaxation flags.

The blockers, as of 2026-05: a `std::simd` `convolve` works on GCC 16.1 and
17-trunk, but `simd.math` (`sqrt`) isn't in shipping libstdc++ even on trunk
(it's gated behind GSI-HPC's `VIR_PATCH_MATH`), and libc++ ships no `<simd>`
at all, so Clang has no path. The magnitude's `sqrt` would stay scalar, or
`std::experimental::simd`, until `simd.math` lands. It also needs GCC 16+ in
the build, which Ubuntu 25.10 and the toolchain PPA don't package; a Compiler
Explorer tarball is the likely route.

So the direction for DSP perf work is: reach for `std::simd`, not more
intrinsics. The hand-AVX2 above exists to be deleted - don't extend it for
modest wins. When GCC 16+ lands, the natural sweep is `convolve_strip` first
(it's now the whole front end; the SAW IF is two real convolutions through
it), then `envelope_magnitude` once `simd.math` exists. The `demod::Hilbert`
deinterleave/interleave glue - pure data movement, shuffle/permute, no
`simd.math` needed - was prototyped as hand-AVX2 and deliberately not landed;
it only runs in `--if flat` now, so it sits at the bottom of the list.
