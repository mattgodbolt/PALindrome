# PALindrome — project guidance for Claude Code

## C++ conventions

- **Almost Always Auto.** Prefer `auto` / `const auto` for locals wherever the
  type is already obvious from the initialiser — a `static_cast`/construction to
  a named type, a bool expression, a `.size()`/`.data()`/`.view()`/`process()`
  result, an iterator. Spell the type out only when it genuinely aids clarity.
  Mind the traps: `auto k = 0` deduces `int`, not `size_t`; `auto x = aFloat`
  drops a `float`→`double` widening; small unsigned operands promote to `int`.
  Leave literal-initialised numerics and width-sensitive arithmetic explicit.
- **Use `std::format` for all string building**, never `+` / `<<`
  concatenation — error messages, logs, paths. Turn `std::string{"x: "} + v`
  into `std::format("x: {}", v)`.
- **`switch` over an enum with no `default`**, so adding a value fails to compile
  (`-Wswitch -Werror`) until it's handled; end with `std::unreachable()`.
- Comments explain "why", not "what".
- **Parsimonious includes (IWYU).** Include exactly what a file names, directly:
  no more (don't pull a header for a symbol you don't use, and don't lean on a
  transitive include — if you name `std::span`, include `<span>` yourself), and no
  less. A `.cpp` includes its own header first, then what it adds. Splitting a file
  is the moment to prune: each piece should carry only the includes its own code
  needs, not the union the monolith did.

### Precision: `float` for the signal, `double` for accumulators

Converting between `float` and `double` isn't free — it costs a `cvt` per value
on (or near) a hot loop's critical path, and a `double` in a body that could be
wide-`float` halves the SIMD width. So the split is deliberate, not per-variable
guesswork:

- **`double`** is for **state that accumulates across samples**: phase/frequency
  integrators (the sweeps' PI loops) and slow-release leaky integrators (the sync
  separator's peak/floor trackers), where the per-sample update is tiny relative
  to the running value, so rounding compounds or stair-steps over thousands to
  millions of samples.
- **`float`** is for **everything that flows through**: samples, FIR taps, LUT
  weights, the framebuffer, and per-sample arithmetic.
- **At the boundary, snapshot the control value *down* to `float` at the point of
  use — never promote the data *up* to `double`.** The control's precision was
  already spent in its own recurrence; the product only needs `float`. (The
  `ComplexAmEnvelope` and chroma NCO mixes snapshot their double phasor to float
  per sample; the phasor still *advances* in double.)

The decision is then non-cognitive: *does this value accumulate over many
samples?* → `double`; otherwise `float`, and snapshot any `double` it touches.

## Invariants

- **Block-invariance.** A streaming stage's output must not *meaningfully* depend
  on how the input is chunked into blocks — the target is live RF, not finite
  files, so the same signal split differently has to decode the same picture.
  Where it's free this is bit-exact (`==`): state recurrences run in sample order,
  FIRs accumulate taps in natural order, so chunking changes literally nothing and
  `==` is the cheapest possible test. That's the default — keep it where it costs
  nothing.

  But bit-exactness is the *test*, not the *goal*. A performance change that
  reassociates or vectorises arithmetic (a blocked-SIMD recurrence, a wider
  reduction) can shift the result by a few ULPs depending on chunk alignment, and
  that is an acceptable trade for a real SIMD win when no strong reason says the
  exact operation order carries meaning — there's no canonically "correct" grouping
  of lossy float ops. The Mixer already lives here (block-invariant only to ~1e-7,
  tested with a tolerance, not `==`). When you make that trade: switch that stage's
  block-invariance test from `==` to a tolerance, say why in the comment, and
  regenerate the golden image — it's a regression *tool*, not a contract.

  Two things stay strict, because keeping them costs nothing: (1) **thread /
  scheduling invariance** — the threaded `render` pins each stage to one in-order
  (FIFO) thread so the threaded result equals the serial decode *exactly* (same
  ops, same order); never trade that away. (2) **feedback loops** (the sync PLLs,
  the AGC, the colour APC) — a reassociation that's harmless in a feed-forward FIR
  can let tiny errors compound and the loop diverge over a long run, so before
  accepting ULP drift inside a recurrence, verify the lock still holds over a full
  decode.
