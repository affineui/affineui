# affineui_lexbor — AffineUI's Lexbor fork

This repository is a **maintained fork of [Lexbor](https://github.com/lexbor/lexbor)** (Apache-2.0), kept as part of the [**AffineUI**](https://github.com/affineui/affineui) project — a two-file, zero-dependency C++ HTML/CSS renderer for native apps and games.

It is the **source of truth** for the Lexbor code AffineUI ships. The patched source is **vendored** (copied) into `affineui/external/lexbor` via [`scripts/sync_lexbor_from_fork.sh`](https://github.com/affineui/affineui/blob/main/scripts/sync_lexbor_from_fork.sh) — AffineUI builds self-contained from that copy and does not depend on this repo at build time. Develop here, then sync.

## Branches
- **`affineui`** — the AffineUI patch set on top of upstream **`v2.4.0`** (this is the source of truth; `external/lexbor` mirrors it).
- **`master`** — pristine upstream Lexbor, untouched, so the patch set stays a clean, rebasable diff against upstream.

## Why this fork exists
AffineUI renders real-world CSS frameworks (Bootstrap, etc.). Stock Lexbor v2.4.0 parses HTML/CSS well but is missing a few framework-critical CSS shorthands and has two small bugs AffineUI hits. Rather than edit the vendored copy in place (where changes would be invisible and lost on the next update), we keep them here as a focused, upstreamable patch stack with full upstream lineage.

## What's different from upstream (v2.4.0)
See [`PATCHES.md`](PATCHES.md) for details. Summary (~1.9k lines across 11 files):

1. **CSS framework shorthands** — adds `border-radius`, `border-color`, `background`, `box-shadow`, `gap`, `row-gap`, `column-gap` to the property table (parsing + serialization + tests). *(bulk of the change: `css/property*`)*
2. **Pseudo-class specificity** — count simple pseudo-classes (`:hover`, `:active`, `:focus`, `:first-child`, …) in the specificity `b` bucket, so `.btn:focus` correctly outranks `.btn`. *(`css/selectors/state.c`)*
3. **`event_destroy` null guard** — `lxb_html_document_event_destroy` no longer falls through to inline-style teardown when `el->list` is null (AffineUI's immediate-mode hits this destroying cascade-matched nodes without `style=""`). *(`html/interfaces/document.c`)*

## Keeping it maintainable
- Patches live only on the `affineui` branch as a diff over the `v2.4.0` tag — reviewable and rebasable onto future Lexbor releases.
- To update Lexbor: rebase the `affineui` branch onto the new upstream tag, resolve, then re-sync into AffineUI.

## License
Lexbor is Apache-2.0 (see [LICENSE](LICENSE)); our additions are under the same terms.
