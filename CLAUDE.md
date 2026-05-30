# PALindrome — project guidance for Claude Code

Project-specific conventions live here. **Do not edit the user's global/home
`~/.claude/CLAUDE.md`** — it's a symlink to a dotfiles repo outside this project,
so changes there leak everywhere. Project rules belong in this file.

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
