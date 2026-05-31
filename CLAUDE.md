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

## Invariants

- **Block-invariance.** Every streaming stage's output must be independent of how
  the input is chunked into blocks — tested bit-for-bit (`==`), because the target
  is live RF, not finite files. Preserve it: state recurrences run in sample
  order, FIRs accumulate taps in natural order, and the threaded `render` pipeline
  pins each stage to one in-order (FIFO) thread so the result is identical to the
  serial decode. A change that makes the output depend on block size or thread
  scheduling is a bug, not a tradeoff — and the optimisation/threading work leans
  on this, so verify it (golden-image diff and the block-invariance tests).
