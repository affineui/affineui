# Contributing

Welcome. AffineUI is small enough that one well-chosen PR can move the
project a meaningful step forward, and small enough that one careless PR
can wreck the size budget. Help us stay in the first category.

## Ground rules

1. **Read [ARCHITECTURE.md](ARCHITECTURE.md) and [DESIGN.md](DESIGN.md)
   first.** Most "why don't we just..." questions are answered there.
2. **The size budget is a feature.** Stripped release binary stays
   under 2 MB with the bootstrap demo. PRs that meaningfully grow the
   binary need to justify it.
3. **Vendored deps are off-limits for direct edits.** For Lexbor, patch
   the `affineui_lexbor` fork (the source of truth) and sync it into
   `external/lexbor` with `scripts/sync_lexbor_from_fork.sh`. Same model
   for NanoVG via the `affineui_nanovg` fork.
4. **No new dependencies without discussion.** Open an issue first. The
   bar is: "must be single-header (or two-file), permissively
   licensed, and replace meaningful code we'd otherwise write."

## Development setup

```bash
git clone <your fork>
cd affineui
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

`CMakePresets.json` defines a `dev` preset with warnings-as-errors,
sanitizers (Address + UB) on Debug, and `compile_commands.json`
exported.

## Code style

- C++20. Idiomatic, but cheap to compile. Avoid heavy templates in
  headers. Yoga is C++20, so the project standard follows.
- Formatting: `clang-format -i` (uses `.clang-format`).
- Linting: `clang-tidy` (uses `.clang-tidy`).
- Naming: `lower_case` functions / variables, `CamelCase` types,
  `lower_case` namespaces. Private members suffixed `_`.
- Public headers are kept include-light. Forward-declare; don't include
  litehtml or sokol from `include/affineui/`.
- Temporary workarounds are allowed only when they deliberately unblock
  development. Mark them with `AFFINEUI_TEMPORARY`, explain the missing
  final behavior in the nearby comment, and add or link a roadmap item so
  they are easy to audit before production hardening.

## Commits

- One logical change per commit.
- Subject line ≤ 72 chars, imperative ("Fix flexbox baseline alignment",
  not "Fixed").
- Body explains *why*, not just *what*.
- Reference issues with `Closes #123` / `Refs #123`.

## Pull requests

Checklist before opening:

- [ ] `cmake --build build` clean with warnings-as-errors
- [ ] `ctest` passes on at least one platform
- [ ] No new `external/` content added without an `external/README.md`
      entry
- [ ] Public header changes mirrored in the C ABI surface (if relevant)
- [ ] Size impact noted in the PR description for non-trivial changes
      (`size build/libaffineui.a` before/after)

## Where to start

Issues tagged `good first issue` are scoped to ~half a day. Bigger
buckets if you're looking for something meatier:

- **Painter completeness.** `box-shadow`, `filter: blur`, image-tiling
  modes for `background-image`.
- **imm ergonomics.** Style builder helpers (`.padding(8).bg("#222")`),
  better keying defaults, `use_effect()` for side effects.
- **CSS hot-reload tooling.** Watch a stylesheet file, push changes
  into a running app via `Document::reload_stylesheets()`.
- **Layout test corpus.** Snapshot golden images of WPT / litehtml's
  own test set so we can catch regressions when we touch the painter.

## Code of conduct

Be excellent to each other. Disagreements about technical choices are
fine and expected; disagreements about people are not.
