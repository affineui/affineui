# external/

Vendored dependencies, checked in so a normal clone configures and builds
without network access.

**`lexbor/`, `nanovg/`, and `yoga/` are git SUBTREES** of the maintained
AffineUI forks (`affineui_lexbor` @ `affineui`, `affineui_nanovg` @ `master`,
`affineui_yoga` @ `affineui`). They are the **canonical, in-tree, editable
source**. Edit them here directly (for example: extend Lexbor's CSS or Yoga's
layout solver), build, and the change ships. Sync with the forks via subtree:

```bash
scripts/sync_lexbor_from_fork.sh        # git subtree pull  (fork -> here)
git subtree push --prefix=external/lexbor <fork-path> affineui   # here -> fork
# (sync_nanovg_from_fork.sh / external/nanovg, branch master, likewise)
# (sync_yoga_from_fork.sh / external/yoga, branch affineui, likewise)
```

`sokol/` and `stb/` remain pinned snapshots (refresh = explicit step).

## Inventory

| Dir | Upstream | License | Pinned at | Used for |
|---|---|---|---|---|
| `sokol/` | https://github.com/floooh/sokol | zlib | `master @ <fetch date>` | GPU + windowing (single-header) |
| `nanovg/` | https://github.com/memononen/nanovg | zlib | `master @ <fetch date>` | Vector painter |
| `lexbor/` | local AffineUI fork of https://github.com/lexbor/lexbor | Apache-2.0 | `affineui/v2.4.0` | HTML5 parser, CSS parser, DOM, selector matching |
| `yoga/` | local AffineUI fork of https://github.com/facebook/yoga | MIT | `affineui/v3.2.1` | Flexbox and AffineUI layout extensions |
| `stb/` | https://github.com/nothings/stb | MIT / public domain | `master @ <fetch date>` | `stb_truetype.h`, `stb_image.h` |

## Rules of engagement

1. **Snapshots vs subtrees.** `lexbor/`, `nanovg/`, and `yoga/` are subtrees;
   edit them in-tree (then `git subtree push` to the fork). `sokol/` and
   `stb/` are pinned snapshots; do not edit directly, bump the pin instead.
2. **Pin versions explicitly.** Whatever is committed under `external/` is
   what we test against. Updating a pin = a PR with a CI run.
3. **License attribution.** Each library's own `LICENSE` file stays alongside
   its sources. Distribution artifacts must keep the license banner and any
   required notices.

## How it integrates

- `sokol/`, `nanovg/`, and `stb/` are header-only or near-header-only.
  NanoVG carries the fontstash/stb copies it needs under `nanovg/src/`.
  `cmake/Dependencies.cmake` adds the curated include paths; a few `.c`
  TUs under `src/paint/` and `src/text/` hold their implementation
  definitions.
- `lexbor/` is a real CMake project; we `add_subdirectory(EXCLUDE_FROM_ALL)`
  it and link `lexbor_static`.
- `yoga/` is a real CMake project and an AffineUI-maintained fork; same
  treatment, links `yogacore`.

## Optional (not vendored by default)

| Dir | Why | When to enable |
|---|---|---|
| `harfbuzz/` | Complex-script shaping | `-DAFFINEUI_ENABLE_HARFBUZZ=ON` |
| `libunibreak/` | UAX #14 line breaking | `-DAFFINEUI_ENABLE_UNIBREAK=ON` |
