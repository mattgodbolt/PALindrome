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

## The shape of the workload: a balanced pipeline

For the live case (AirSpy, 20 MS/s real, decimate 1, 720x576 colour, 8 deposit
threads) the per-thread CPU time is roughly:

    source ~= decode ~= sink(deposit coordinator) ~= 3000-3200 samples
    deposit apply helpers                          ~= 1000-1100 each (x7)

The three heavy stages sit within ~10% of each other - none is even 1.1x another.
There is **no single dominant bottleneck**. This is the key fact and it has a hard
consequence:

- **Splitting a stage does not move the wall.** It just hands the bound to the
  next near-equal stage. Measured directly: splitting the decode into sync+chroma
  stages was wall-neutral; a projected front-end (IF-FIR vs detector) split
  measured ~2%. Re-staging a balanced pipeline buys nothing.
- To go faster you must cut **total work** (e.g. wider-SIMD FIRs, or drop a stage
  entirely), not re-shuffle the stages.

Caution when measuring this: per-thread `perf` sample counts rank threads only
coarsely. Stages within ~10% are co-saturated, not "one is the bottleneck" - do
not chase differences that small between near-equal stages. And always fix one
config: the RX888 corpus decimates /2, the AirSpy live path decimates /1 (~2x the
downstream work), so the bottleneck legitimately differs between them.

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

## Levers not yet pulled

- **Wider-SIMD FIRs.** The `d==1` (non-decimating, i.e. the AirSpy live path)
  `convolve_strip` tier is AVX2 (256-bit) on a box that has AVX-512. The FIRs are
  ~20% of the work and appear on both the source and decode threads, so widening
  them cuts total work on the two heaviest stages at once. Held pending the
  `std::simd` toolchain direction rather than hand-rolled AVX-512.
- **GPU deposit.** The 24-byte `SplatRecord` field buffer is exactly the batch a
  GPU scatter would consume; a CUDA path is a scheduler swap, not a rewrite.
