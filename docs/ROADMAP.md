# Roadmap

Rough sequencing. Items inside a phase are not strictly ordered. Items
marked ✅ are landed; ⏳ are in progress; otherwise open.

## Phase 0 — Scaffolding ✅

- [x] Project layout, license, build system
- [x] Documentation (architecture, design, building, contributing)
- [x] Dependency fetch script
- [x] CI skeleton — three-OS stub-mode on PRs, full-deps on push-to-main
- [x] Stub public API that compiles

## Phase 1 — Minimum viable render ✅

Goal: render `<h1>Hello</h1>` against a colored body on Metal + GL.

- [x] sokol_app + sokol_gfx + NanoVG painter wired, all symbols
      file-local in one TU (encapsulation contract honored)
- [x] fontstash + stb_truetype font pipeline
- [x] lexbor integration: HTML5 parse → DOM walkable through the bridge
- [x] lexbor CSS parse → cascade-ready rule trees
- [x] Cascade resolver covering color, background-color, font-family,
      font-size, font-weight, padding, margin, border, line-height
- [x] Block + flex + inline layout via the Yoga adapter (exceeds the
      original "one block" target)
- [x] Paint driver emits fill_rect + draw_text against `Painter`
- [x] `examples/00_hello` rendering visually correct on Metal + GL
- [x] CI builds + ctest on Linux + macOS + Windows
- [ ] Pixel-level snapshot test for `00_hello` (the file-tree slot
      `tests/snapshots/` is reserved; goldens not generated yet)

## Phase 1.5 — Library shaping ✅

Wasn't on the original roadmap but happened during Phase 1 once it
became obvious users wanted to drop AffineUI into their own window
loops, not the other way around.

- [x] Split `App` into `Renderer` (graphics-only) + `Ui` (facade) +
      window adapters — embedders own their event loop
- [x] First-class sokol adapter (`affineui::sokol::{wire,dispatch,render}`)
- [x] First-class SDL2 adapter (`affineui::sdl::{wire,dispatch,render}`)
- [x] Single umbrella include `<affineui/affineui.h>` — adapters
      autoload via `AFFINEUI_WITH_{SOKOL,SDL}`
- [x] `Ui::on_click("#id" | ".cls" | "tag" | "a,b")` selector handlers

## Phase 2 — Interactive ⏳

- [x] Mouse routing (move + button down/up) wired through `Ui::dispatch`
- [x] Cursor sync (pointer/text/crosshair) inside the event callback so
      Cocoa's `mouseMoved:` lands in the same run-loop turn
- [x] `:hover` pseudo-class via side-table overlay — single-compound
      selectors only (`button:hover`, `.btn:hover`, `#id:hover`).
      Ancestor chain tracking + per-element re-resolve on toggle.
- [x] `:active` (mouse-down state) — same overlay machinery as
      :hover, separate state bit + chain. MouseDown sets, MouseUp
      clears, restyle fires per affected element.
- [x] `:hover` / `:active` widened to multi-simple compounds
      (`.card.clickable:hover`) and descendant combinators
      (`.clickable h1:hover`). Scanner builds CompoundSelector
      chains; matcher does greedy ancestor walk through Block
      parent_idx for descendant combinator semantics.
- [ ] `:hover` / `:active` widened further to `>`, `+`, `~`,
      attribute selectors, and functional pseudos via
      `lxb_selectors_find` with a state-aware match callback
- [x] Vertical wheel scroll on `overflow-y: auto|scroll` containers
      — block tracks scroll_y + content_h, paint clips to container
      bounds and offsets child draws, hit-test sees the same
      effective bounds, simple thumb indicator on the right edge.
- [ ] Horizontal scroll, scrollbar drag, nested-scroll-container
      clip intersection (NanoVG's scissor replaces rather than
      intersects today)
- [ ] Keyboard routing for focused inputs (`on_key_press`, IME stub)
- [x] `examples/01_bootstrap` — Bootstrap-flavored page renders
      pixel-close to the browser reference. Surfaced bugs got fixed
      inline along the way: CSS cascade ordering (lexbor's weak
      walk inverted specificity), missing body bg pre-pass, page-
      padding driven by body CSS not a hardcoded constant,
      width/height standalone parsing, gap-fill scanner for
      border-radius / border-color / gap from CSS rule blocks
      (lexbor 2.4 silently drops them), inline-block sizing for
      `<button>`, text measure ceil to avoid invisible word wrap,
      font-weight cascade arm, SF Pro as default `-apple-system`
      face, bold from .ttc face index, synthetic medium for weight
      500 via double-draw (matches Chrome/Safari fake-bold
      behavior when no Medium variant is installed).
- [x] `background:` shorthand recovery via the gap-fill scanner
      (lexbor exposes only `background-color` longhand). `border-color`
      / `border-radius` / `gap` already landed earlier in the same
      pass. Color portion only — image / position / size still TBD
      when those paint features land.
- [x] `font-family` / `font-style` / `line-height` longhand cascade
      arms. font-style picks italic / bold-italic faces (loaded from
      .ttc face indices on macOS); line-height threads through both
      Yoga measure and NanoVG paint via a new Painter contract; font-
      family interns through StyleStore::font_family_of so the
      element's resolve_font lookup picks the right family.
- [ ] `font:` shorthand decomposition (lexbor doesn't expose it; we
      can recover via the gap-fill scanner the same way we did with
      `background:`)
- [ ] font-family fallback walk — today only the first family in the
      list is honored; spec says try each in order until an installed
      face is found
- [x] Per-corner `border-radius` (TL/TR/BR/BL) — CSS 1/2/3/4-value
      shorthand pairing applied at scan time; Painter grows
      `*_rounded_rect_varying` ops backed by NVG's
      `nvgRoundedRectVarying`; works in both CSS rule blocks and
      inline `style="..."` attributes.
- [ ] Elliptical radii (`border-radius: H / V`, separate horizontal
      and vertical radii per corner)
- [x] Synthetic line-box wrapper so multiple inline / inline-block
      siblings share a row (collect_blocks tracks an open run; the
      first inline child opens a flex-row-wrap synthetic Block;
      next block-level sibling closes it; paint skips the wrapper).

## Phase 3 — Imm-mode layer ⏳

Phase 2D shipped: clear-and-rebuild reconciler driving lexbor DOM
mutation. Public surface (`mount`, `use_state`, `on_click`,
`invalidate`) is stable; the body is the next swap.

- [x] Call-site-path identity via `std::source_location` hash + key
      override surface
- [x] `use_state<T>` slot map keyed by call-site, persists across
      re-renders, dtor on document destruction
- [x] `on_click` handler routing through auto-stamped
      `aui-imm-{hash}` element ids
- [x] `imm::invalidate()` + dirty tracking — view fn doesn't run per
      frame
- [x] `examples/04_imm_counter`
- [x] Clear-and-rebuild reset path (nulls `doc->ev_remove` /
      `ev_destroy` to dodge lexbor's broken cascade-tear-down on
      cascade-attached subtrees; see *Known issues* below)
- [x] Dumb-reconcile (set-until-mismatch). ImmRuntime walks a
      cursor stack alongside the parent stack; matching elements
      (same tag + `aui-imm-{hash}` id) reuse in place, mismatches
      destroy the tail and rebuild. Text-only changes update node
      content with no DOM churn. State slots stay tied to stable
      DOM nodes across renders — same public surface as Phase 2D
      clear-and-rebuild, entirely different body.
- [ ] Virtual-DOM data structures (only needed if we promote past
      dumb-reconcile to keyed-list reconciliation)
- [ ] `on_change` / `on_input` handlers (today they're stubs)
- [ ] `examples/05_imm_todo` — keyed list rendering, exercises
      add/remove/reorder

## Phase 4 — Polish & performance

- [x] Retained root-layer renderer/compositor documented in
      [RENDERER_COMPOSITOR.md](RENDERER_COMPOSITOR.md): display-list
      caching, dirty rects, partial raster, direct root-layer
      composition with NanoVG fallback, prepared transformed/clip range
      replay culling, stats, and the next multi-layer compositor
      direction.
- [ ] Multi-layer compositor promotion: split stable transformed,
      opacity, scrolling, and animation-heavy subtrees into retained
      layer textures, then animate transform/opacity at composite time
      without document paint or root-layer raster.
- [ ] `border-radius` per-corner correctness (currently uniform only)
- [ ] `box-shadow` (drop + inset)
- [ ] CSS transitions reaching the painter (driven by retained DOM,
      no imm re-render)
- [ ] Core Decius/Bootstrap widget conformance before broadening SVG:
      panels, buttons, forms, generated content, icon fonts, gradients,
      shadows, layout, and animation filmstrips.
- [ ] Background image: `cover`, `contain`, `repeat-x/y`, `no-repeat`
- [ ] Form widgets: `<input type=text|button|checkbox|radio>`
- [ ] DPI-aware text rendering corrections (we plumb fb pixels vs
      points correctly; intrinsic text height still uses the parked
      hand-tuned constant — fix is to lean on real `nvgTextMetrics`)
- [ ] Low-priority Decius checkbox icon polish: verify browser and
      AffineUI vertical centering for the `decius-icons` check glyph
      inside `.dcs-check__box`; if both are off, fix the icon/CSS
      upstream, otherwise fix generated-content/icon-font baseline
      alignment in the renderer.
- [ ] Image cache LRU with size cap
- [ ] Layout-cache invalidation per dirty subtree (don't relayout the
      whole document for one attribute change)

## Phase 5 — Reach

- [ ] Material Design 3 sample (`examples/02_material`)
- [ ] Scoped SVG rendering per [SVG_ARCHITECTURE.md](SVG_ARCHITECTURE.md):
      retained scenes, stable raster cache, dynamic vector data, Decius
      jacks/LCD/knobs/cables/graphs first; no broad SVG engine.
- [ ] CSS hot-reload tool (`tools/ui-preview` watches files)
- [ ] WebGPU backend wired through sokol_gfx
- [ ] C ABI completed and documented (skeleton lives in
      `src/c_api.cpp`)
- [ ] Rust bindings example (off-tree)
- [ ] Snapshot test corpus (golden images for the painter)
- [ ] HarfBuzz integration hook for complex shaping

## Phase 6 — Maybe

Things that *could* land if there's demand. None are committed:

- [ ] CSS Grid (filling lexbor gaps, possibly upstreaming)
- [ ] Broader SVG beyond the scoped UI subset (filters, masks,
      markers, text-in-SVG, scripting-adjacent APIs only if a real
      target requires them)
- [ ] Accessibility tree → platform AX bridges
- [ ] Embedded JS via QuickJS (one possible direction; controversial)

## Known issues / parked

- **Lexbor destroy-after-cascade corruption.** Destroying cascade-
  attached elements via `lxb_dom_node_destroy_deep` corrupts
  `doc->css.styles` AVL pool — next insert hangs in
  `lexbor_avl_node_balance`. Even `lxb_html_element_inner_html_set`
  (lexbor's canonical mutation API) hangs the same way under our
  setup. Most lexbor users parse → walk → drop document, so this
  path is unexercised upstream. Our workaround in
  `src/imm/imm_runtime.cpp::clear_children` nulls `doc->ev_remove` +
  `doc->ev_destroy` for the destroy walk, leaving orphan cascade
  entries that get reclaimed with the document. Phase 3 dumb-
  reconcile sidesteps the issue entirely.
- **Lexbor `event_destroy` NULL deref.** Patched on the `affineui_lexbor`
  fork (the source of truth). Real upstream bug, narrow path, no
  upstream issue filed yet.
- **Text vertical metric.** Intrinsic height in the Yoga adapter
  uses a hand-tuned constant. Plumbing real `nvgTextMetrics` is a
  trivial fix that's parked behind Phase 4 polish.

## Explicit non-goals

See [ARCHITECTURE.md § Non-goals](ARCHITECTURE.md#non-goals-explicit).
