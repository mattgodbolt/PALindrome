# architecture: the signal chain at altitude

The top-level README has the signal-flow graph and [render.md](render.md)
walks the decode stage by stage. This page is the shape of the thing: what a
stage is, what flows between them, and who is responsible for what. The
per-stage headers carry the detail; this is the contract they all share.

## The stage protocol

The one-span transform stages - the FIRs and detectors, the AGC, the sync
separator, both timebases, the chroma decoder - share the same three-call
surface:

- `prepare(max_in)` sizes the internal buffers for the largest block a call
  will see. It is the one allocation: the output `Buffer` deliberately never
  grows (no push_back, no capacity branch in the hot loop), so a `process`
  call handed a bigger block than prepare budgeted for throws rather than
  reallocating. The Screen's deposit staging is the one exception that grows
  on demand.
- `process(span) -> span` consumes one block and returns a view of the
  stage's own output buffer, valid until the next call. All state carries
  across calls: FIR delay lines, PLL integrators, the AGC's peak tracker.
  A stage never knows or cares whether it has seen one block or a million.
- `max_output_for(n_in)` bounds the output count for an input count, so a
  caller can size what comes next. The decimating DSP stages (the FIRs and
  the detectors) return fewer; the video stages are 1:1 and theirs is the
  identity.

Two nodes sit outside the one-span shape, deliberately. The Screen is the
graph's join, not a transform: `process` takes the three rails as
same-length spans and emits frames through a field callback rather than
returning a span (so there is no `max_output_for`). The Decoder is the
whole graph reified as one node: `prepare` / `decode_into` / `deposit`, the
two halves a pipeline can run on separate threads.

That uniformity is a load-bearing invariant, not a style preference. Input
shape - block size, looping, settling, decimation policy, when to emit a
frame - is the *driver's* business (whoever calls `process` in a loop:
`render`, `sync`, the tests). No stage grows a knob for it. The full
statement of this rule, and the block-invariance contract that follows from
it, is in the project CLAUDE.md's Invariants section; the short version is
that a stage's output must not meaningfully depend on how the input was
chunked, because the real target is live RF, not files.

## The wire types

Stages speak to each other in small per-sample PODs, defined once in
`video_types.hpp` alongside the nominal PAL timing and the System I level
geometry:

- `float` - the envelope rail: negatively-modulated composite, sync tip at
  1.0 once the AGC has done its job.
- `SyncSample` - the separator's sliced one-bit sync.
- `BeamSample` - the horizontal sweep's beam position (`h_phase` in [0, 1))
  plus a `line_start` flag.
- `VSample` - the vertical flywheel's `v_phase`.
- `ChromaSample` - luma plus the recovered U/V colour-difference pair.

There is no timestamp or metadata riding along: rails are joined by index.
Sample i of the picture rail, the horizontal rail and the vertical rail are
the same instant, which is what lets the Screen take three spans and paint.

## The graph

`video::Decoder` (decoder.hpp) is the branching graph reified as one node:
envelope in, frames out. It owns the AGC, a sync-branch low-pass, the
separator, both timebases, the chroma decoder and the Screen, and hides the
one fan-out (the sync bit feeds both flywheels) and the one join (picture,
horizontal and vertical rails meet at the Screen). The README's mermaid
diagram is this graph; the wiring lives in decoder.cpp, once.

Decoder splits its work into `decode_into()` (everything up to the three
rails, copied into a self-contained `DecodedBlock`) and `deposit()` (the
block onto the phosphor), so a pipeline can run the halves on different
threads.

## The threading model

`pipe::run` (pipeline_run.hpp) executes a source, a chain of transforms and
a sink either serially or threaded from the same description. Threaded, each
stage is pinned to one in-order FIFO worker (a stdexec run_loop on its own
thread), a block apart, with owned buffers handed through bounded pools -
backpressure, so memory stays bounded however fast the source is. Serial
runs the identical stage functions back to back. Because every stage runs
its samples in the same order either way, the threaded decode equals the
serial one bit for bit; that thread-exactness is one of the two invariants
never traded away (the other being the feedback loops' recurrence order).

`render` uses three stages: front end (IF filter and detector), decode
(`decode_into`), deposit. Where the time goes between them is
[performance.md](performance.md)'s story.
