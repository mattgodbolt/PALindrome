# Decoder performance: where the time goes, and what moved it

This records where the render/live pipeline spends its time and which
optimisations paid off, so the next person (or the next attempt) starts from
measurement instead of re-deriving it. Numbers are from an i9-9980XE (18 physical
cores), release `-march=native` LTO.

## The pipeline

`palindrome render` runs a streaming stage pipeline (`pipe::run`), one in-order
FIFO worker thread per stage, bit-identical to the serial `--no-threads` decode:

    source (read + front-end: Hilbert / IF-FIR + detector)
      -> decode (AGC, sync separator, H/V sweeps, chroma decode)
      -> deposit (paint the phosphor framebuffer)

The deposit additionally fans one field's work across a worker pool (see below),
so it is data-parallel *within* the stage as well as pipelined.

## The shape of the workload: a nearly balanced pipeline with a moving limiter

The three heavy stages sit close together, so the limiter is config-dependent
and *moves as levers land*. Per-thread busy fractions, measured directly from
`/proc/<pid>/task/*/stat` over a looped decode (2026-07-09, post #60/#62,
8 deposit threads):

    AirSpy 20 MS/s d==1 live:  source 80%   decode 94%   sink 82%   <- decode limits
    RX888  32 MS/s d==2 path:  source 88%   decode 96%   sink 82%   <- decode limits
                               (source was 96% and the limiter before #62)

Hard consequences, measured repeatedly:

- **Splitting a stage does not move the wall.** It just hands the bound to the
  next near-equal stage. Measured directly: splitting the decode into sync+chroma
  stages was wall-neutral; a projected front-end (IF-FIR vs detector) split
  measured ~2%. Re-staging a balanced pipeline buys nothing.
- **Only cutting the limiter thread's work moves the wall**, and only down to
  the next stage's level - then the bottleneck hands over (see #62's landing:
  a 40% cut to the source thread's fattest kernel moved the 32 MS/s wall by
  exactly the source-over-decode gap, 16%, and the live wall by nothing).
- To go faster, find the limiter *for the config you care about* first, and cut
  **total work on that thread**.

Caution when measuring this: per-thread `perf` sample counts rank threads only
coarsely - use `/proc/<pid>/task/*/stat` utime+stime for the busy fractions.
Stages within ~10% are co-saturated, not "one is the bottleneck". And always
fix one config: the RX888 path decimates /2, the AirSpy live path /1 (~2x the
downstream work), so the balance legitimately differs between them.

## What moved the wall

- **Threaded splat deposit (#52).** The deposit was the single largest work
  share. It is now deferred: the per-sample control pass records a 24-byte
  `SplatRecord` per visible sample into a per-field buffer; at each field boundary
  the whole field is binned into output bands and applied across a persistent
  worker pool (`--deposit-threads N`), each output row owned by one band so the
  result is byte-identical whatever the thread count. Because a field is applied
  in one burst, the pipeline's in-flight depth is raised to a field of blocks
  (`kInFlight`) so the decode stage runs ahead *through* the burst instead of
  stalling on it - the pipelining that makes the threading pay. This is what takes
  colour to real time. 720x576 colour: RX888 /2 corpus 0.52s -> ~0.45-0.49s at 8
  threads; AirSpy /1 live path ~0.87s for a 1.0s second (~13% margin), which is
  why `live_view.py` defaults to `--deposit-threads 8`.
- **Detector sqrt-hoist (#51).** The quasi-sync detector's per-sample `1/|y|`
  normaliser (a sqrt + divide on the carrier-recovery critical path) is lifted to
  a feed-forward `1/|i,q|` computed over the whole block (`|nco|~=1` in lock).
  -37% on the detector recurrence, ~-18.7% on the quasi-sync demod path. That is
  ~10% off the RX888 /2 render, but only ~3% on the AirSpy /1 live path (where the
  deposit dominates and the demod is a smaller slice). Bit-exact block-invariance
  preserved.
- **Fused vision FIR pair (#62).** `VisionIf` ran two independent 255-tap Firs
  over the same input; `FirPair` evaluates both tap sets in one pass with one
  shared window, each window load (and each d==2 deinterleave shuffle) feeding
  both accumulator strips. Those are exactly the resources the strip tiers are
  bound on - load ports at d==1, port-5 shuffles at d==2 - so the pair costs
  ~40% less at an unchanged FMA count, bit-exact (renders byte-identical).
  Kernel -39%/-41%; DemodBench VisionIf 1.16 -> 0.74 ms/block; the 32 MS/s wall
  -16% (its limiter was the source thread, 71% of which was the pair), the
  AirSpy live wall unchanged (decode limits there). The win stopped exactly
  where the moving-limiter model said: at the decode thread's level.

## Dead ends (measured, do not repeat)

- **Per-block fork-join deposit parallelism.** Fanning each 64k-block's splats
  across cores (interleaved-rows or banded) was a net loss: a block is only ~4ms
  of deposit work, and waking parked helper threads per block costs as much as the
  work. The fix was to batch to per *field* (one barrier per field, not per
  block) - the deferred/frame-buffered design above.
- **Decomposing the pipeline into more stages for speed.** A `pipe::run` framework
  rework (templated source type, move-through rails, deduced stages) was built to
  enable finer front-end/decode staging, then abandoned: the pipeline is balanced
  (see above), so the staging is wall-neutral, and two independent design reviews
  judged the framework generality as speculative for a perf win that was not
  there. The lesson: confirm there is a fat stage to cut *before* building the
  machinery to cut it.
- Earlier deposit-layout micro-experiments (RGBA-pad, SoA-planar, per-block SIMD
  splat) all measured losses; see git history around the `perf`/`screen-gun-inline`
  branches.
- **More than 8 deposit threads.** 16 lanes measures *slower* than 8 on the live
  path: the wider burst lights up more cores at once and Skylake-X drops its
  all-core turbo (~7.5% clock, measured), context switches go 2.6x, and 15
  workers + 3 pipeline threads exactly fills the 18 physical cores. 8 lanes is
  the sweet spot on this box for a reason, not by luck.

## Measured neutral, kept for later (issue #61, draft PR #70)

A bit-exact 2x on the splat inner loop - the hottest symbol in the program at
30.4% of cycles - moved **neither the wall nor total CPU** on the live path.
The implementation (pad the per-record kernel pattern to whole 8-float lanes,
deposit each interior row as one contiguous vector op, patterns built through a
64-record arena because Skylake cannot forward pending scalar stores into a
vector load) is verified byte-identical to the scalar loop and lives on branch
`perf/splat-pad24` (PR #70), unmerged. Two structural reasons for the
neutrality:

1. **The burst is hidden.** At 8 lanes the per-field deposit burst fits inside
   the pipeline's slack; the wall is set by the source/decode stages, so no
   deposit-side improvement of any size can move it. (The 4-lane -> 8-lane wall
   gain is the burst still poking out at 4 lanes, not a per-lane cost that
   keeps paying.)
2. **Fanned out, the deposit is memory-bound.** With 8 band workers running
   concurrently, `SplatBench` times are identical before and after the 2x
   instruction cut; only the 1-thread run shows it (-23%). Instruction cuts do
   not cut time in a bandwidth-bound regime.

Size any deposit work in *burst latency on the critical path*, never in % of
program cycles - a pipelined decode hides everything that is not the limiter.
But bottlenecks move: cut the current limiter and the next-slowest stage
inherits the wall (#62 landed and handed the 32 MS/s wall from source to
decode, not to the deposit). If enough decode-side work lands, or the output
grows (higher resolution, more channels), the deposit can become limiting
again - that is the day PR #70 merges, and the line-run accumulator sketched
on #61 (mean real run length 24.7, cuts framebuffer traffic ~7x, attacks the
memory bound directly) becomes the follow-on. Measurement caveat for all splat-loop work: Skylake-X's
JCC erratum swings identical tight loops up to 35% across code layouts - judge
changes on the full-render A/B, never a microbench delta.

## Levers not yet pulled

- **The decode thread - the *current* wall lever on both paths** (post #62 it
  limits everywhere; see the busy-fraction table above). Its spend: its own
  `Fir::process` share (chroma band-pass, U/V low-pass pair, sync low-pass -
  ~34% of the thread), `Decoder::decode_into` (~33%: AGC, mixers, comb
  dispatch), the sweeps (~23%). Issue #63 (chroma pass-3 precision discipline,
  per-line control snapshots, comb dispatch hoist) is the named workpiece. The
  U/V low-pass pair shares *taps* but not input, so the #62 fusion shape does
  not transfer; its sharable resource is only the tap broadcast (~10% by the
  same port math - probably not worth the surface).
- **Wider SIMD via std::simd (gcc 16+).** The strip tiers are AVX2 (256-bit) on
  a box with AVX-512 and two 512-bit FMA units; 512-bit lanes would halve the
  strip count. Held for `std::simd` rather than hand-rolled AVX-512. The
  inventory of conversion sites is marked `TODO(std::simd)` in the code -
  currently `fir.cpp` (`convolve_strip` + `convolve_strip_pair`, the fused
  structure carries over unchanged) and `demod.cpp` (`inv_magnitude`'s
  `[[gnu::optimize]]` fast-math island; gcc 16.1's `<simd>` lacks simd-math
  sqrt, so it waits on that specifically) - grep for the tag rather than
  trusting this list.
- **GPU deposit.** The 24-byte `SplatRecord` field buffer is exactly the batch a
  GPU scatter would consume; a CUDA path is a scheduler swap, not a rewrite.
