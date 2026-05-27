# Architecture

## Goal

Render real HTML5 + CSS to a GPU surface in a small, self-contained C++20
engine. Unlike "bring your own renderer" engines (litehtml, RmlUi), the
HTML5 parser, CSS parser, selector matcher, cascade, style resolver, box
layout, and inline layout are **all owned in this tree**. We pull in
tiny focused helpers for the genuinely hard, narrow problems — flexbox
math (Yoga), glyph rasterization (stb_truetype), vector painting
(NanoVG), GPU + windowing (sokol).

The goal is to own the seams. The historical pain with embedding a
general-purpose engine is the seam between *its* assumptions and *your*
renderer — coordinate systems, units, allocation policies, layout
incrementalism — being underspecified. By owning both sides of every
seam, we keep the engine small *and* keep it ours.

The graphics-side retained renderer and compositor pipeline is documented
separately in [RENDERER_COMPOSITOR.md](RENDERER_COMPOSITOR.md). That doc
is the source of truth for display-list caching, dirty rects, root-layer
raster, composition, and the path toward compositor-only animation.

## Two front-ends, one engine

AffineUI exposes **two equivalent front-ends** that converge on the same
retained DOM:

1. **Retained mode** — you give it an HTML string. Edit HTML, edit CSS,
   call `Document::set_html()`. Best for designer-authored layouts,
   tool UIs, hot-reloadable styling.

2. **Immediate mode** — you provide a *view function* that calls
   (`imm::div()`, `imm::button("Click")`, `imm::use_state(0)`). The imm
   runtime invokes it **only when something might have changed** (a
   state hook mutated, an event handler ran, an explicit
   `imm::invalidate()`). The resulting virtual DOM is diffed against
   the previous one and the retained DOM is patched (React-style
   reconciliation). Best for game UI, in-engine tools, anything where
   the UI mirrors app state.

   **The view function is not per-frame.** Painting runs every frame
   off the retained DOM. The view function only re-runs when its
   inputs have plausibly changed, so static or near-static UIs cost
   essentially nothing.

You can mix them — a retained HTML shell with imm-mode islands inside.

## App/session model

`App` is the product-level application/session object. It owns the
state that makes one UI independent from another: retained document,
immediate-mode runtime state, event handlers, resource policy,
invalidation state, and the connection to a presentation backend.

The backend is an attachment point, not the app model. The same API
shape must support:

- a local AffineUI renderer driving a native window or embedded surface;
- a remote browser transport for development, testing, or server-hosted
  UI;
- multiple `App` objects in one process, so one process can host a
  multi-user server with isolated sessions.

Backends may share process-level facilities such as GPU devices,
transport listeners, font caches, or worker pools, but user-visible app
state must stay owned by the `App` instance. This keeps the C++ API, the
future Python "Gradio-style" API, and any remote-browser bridge aligned:
they all create and mutate app/session objects, then choose where those
objects present.

### Backend messaging

The native and browser backends should use the same conceptual queue
boundary. An `App` owns one or more typed queues; callers and backends
post messages, and the app drains them at deterministic points in its
tick/frame cycle:

- host -> app: viewport changes, pointer/keyboard input, lifecycle, and
  resource requests;
- app -> host: presentation updates, cursor changes, invalidation,
  timers/animation wakeups, and optional diagnostics;
- app -> app API caller: widget events and state changes.

For an embedded/native UI these messages are just same-process function
calls that enqueue small message objects. For a browser UI, the same
messages can be serialized by a transport adapter such as WebSocket and
posted into the same queues on the other side. The remote browser is a
presentation and input adapter, not a separate source of truth. It may
render HTML/CSS using the browser, but durable widget state and
application behavior remain in the `App`.

This is deliberately not a full browser protocol. The goal is a simple
post/drain model that can run the same retained/immediate application
locally or remotely, keep Python/C++ APIs identical, and allow server
processes to host many `App` sessions without special cases.

### Callback lifetime model

Widget callbacks are registered as a guarded delegate: a weak handle to
the object or state that owns the callback's lifetime, plus a method or
callable to invoke while that handle is alive.

The direct API should make that shape visible:

```cpp
add_on_change(handle, method_or_delegate);
remove_on_change(handle, method_or_delegate);

slider.on_change = aui::event_handler(handle, method_or_delegate);
```

Dispatch resolves the handle before invoking the delegate. If the handle
is expired, the callback is not called and the registration can be
pruned. This keeps callbacks safe when apps, sessions, components, or
state slots are destroyed. The handle does not extend lifetime; it is a
nullable capability with defined empty semantics.

Named methods and stable delegate objects can be removed with
`remove_on_change(handle, method_or_delegate)`. Anonymous lambdas should
also return an event token/subscription so callers have a precise removal
path without relying on lambda identity.

The implementation should be one shared callback-list substrate used by
C++, Python, and later C#. It should not grow a separate event system per
language binding. The core representation should favor:

- compact contiguous slots with stable callback ids;
- tombstone removal plus opportunistic compaction outside dispatch;
- a small function pointer/trampoline plus user payload for C/Python
  bindings;
- typed C++ wrappers for method pointers and guarded lambdas;
- no per-callback virtual base class on the hot path;
- no allocation for common method/delegate registration beyond any
  payload storage the binding explicitly needs.

Python can map bound methods to this substrate with `weakref.WeakMethod`
and a single C trampoline. The C++ API can map `this` + method pointer to
the same slot layout. Mixed C++/Python apps therefore dispatch through
the same callback list, see the same ordering/removal semantics, and do
not pay for duplicate observer machinery.

## Layered view

```
┌────────────────────────────────────────────────────────────────────┐
│ Application (game / tool / embedder)                               │
└──────────────────┬──────────────────────────────┬──────────────────┘
                   │ HTML / CSS strings           │ imm::div() / imm::button() / ...
┌──────────────────▼────────────┐  ┌──────────────▼─────────────────┐
│   Document::set_html()        │  │  imm::Reconciler               │
│                               │  │   • builds virtual DOM tree    │
│                               │  │   • diffs vs prev tree         │
│                               │  │   • patches DOM (add/remove    │
│                               │  │     /reorder/setAttr)          │
└──────────────────┬────────────┘  └─────────────┬──────────────────┘
                   │                              │
                   └──────────────┬───────────────┘
                                  ▼
┌────────────────────────────────────────────────────────────────────┐
│ AffineUI engine (this tree)                                        │
│                                                                    │
│   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐        │
│   │   DOM    │──▶│  Style   │──▶│  Layout  │──▶│  Paint   │        │
│   │          │   │ cascade  │   │  block   │   │  driver  │        │
│   │  Element │   │  +       │   │  inline  │   │          │        │
│   │  + Text  │   │ selector │   │  text    │   │          │        │
│   │  nodes   │   │ matcher  │   │  ┌─────┐ │   │          │        │
│   └──────────┘   └──────────┘   │  │Yoga │ │   │          │        │
│        ▲             ▲           │  │flex │ │   │          │        │
│        │             │           │  └─────┘ │   │          │        │
│   ┌────┴────┐    ┌───┴────┐      └────┬─────┘   └────┬─────┘        │
│   │ HTML5   │    │  CSS   │           │              │              │
│   │ parser  │    │ parser │           ▼              ▼              │
│   │ (ours)  │    │ (ours) │      ┌─────────────────────────┐        │
│   └─────────┘    └────────┘      │  Painter (interface)    │        │
│                                  └────────────┬────────────┘        │
└────────────────────────────────────────────────┬───────────────────-┘
                                                 │
                              ┌──────────────────▼──────────────────┐
                              │  Default painter impl               │
                              │  NanoVG → sokol_gfx                 │
                              │  fontstash + stb_truetype           │
                              │  stb_image (for <img>)              │
                              └──────────────────┬──────────────────┘
                                                 ▼
                                ┌─────────────────────────────┐
                                │   sokol_gfx + sokol_app     │
                                │ Metal / D3D11 / GL3 / WGPU  │
                                └─────────────────────────────┘
```

## Frame loop

```
sokol_app frame callback:
  ├── pump input events → Document::dispatch(Event)
  │     ├── hit-test against retained box tree
  │     ├── update :hover / :active / focus state
  │     └── invoke registered handlers
  │           └── if handler mutated state → imm dirty bit set
  │
  ├── if imm is dirty:
  │     ├── invoke embedder's view fn → new VDOM
  │     ├── diff(prev VDOM, new VDOM)
  │     └── apply patches to Document's DOM
  │
  ├── if DOM or viewport changed since last layout:
  │     └── relayout dirty subtree(s)
  │
  ├── Painter::begin_frame()
  ├── Document::draw(painter)
  │     └── traverse box tree, emit paint ops
  ├── Painter::end_frame()     ← flushes to sokol_gfx
  └── sg_commit()
```

## The engine, module by module

### DOM (`src/dom/`)

Hand-rolled. Element / Text / Document nodes; parent + sibling pointers;
attribute map (small-vector of `{key, value}` pairs, no hash overhead
for the typical few-attribute case); class list as a pre-tokenized
`small_vector<string_id>` so selector matching doesn't re-tokenize on
every check. String interning for tag names, attribute names, class
names.

Aspirational size: ~500 LOC. ~24 bytes per node payload before
children.

### HTML5 parser (`src/html/`)

A pragmatic subset parser, not a spec-faithful tokenizer. We are not a
web browser — our input is our own UI markup, which is well-formed.

Handles:
- Standard tags, attributes, text nodes
- Self-closing tags (`<img>`, `<br>`, `<input>`)
- Comments
- Entity references (`&amp;` `&lt;` `&#xNN;` `&#NN;`)
- `<style>` and `<link rel=stylesheet>` redirect their contents to the
  CSS parser
- Embedded `<script>` is *parsed* but ignored at execution time (we
  have no JS engine)

Does **not** handle:
- Malformed input (extra `</p>` etc.) — we error rather than recover
- `<!DOCTYPE>` quirks mode
- HTML5 templates / shadow DOM
- The full ~200-state spec tokenizer

Escape hatch: an embedder who needs robust parsing can replace this
module with a Gumbo binding. The DOM is the same on the other side.

Aspirational size: ~1.2k LOC.

### CSS parser (`src/css/`)

Hand-rolled tokenizer + rule parser. Handles:
- Tokens: ident, hash, string, number, percentage, dimension, function,
  at-keyword, delim, whitespace, comment
- Selectors: type, class, id, attribute (`[name=...]`, `[name~=...]`,
  `[name|=...]`, `[name^=...]`, `[name$=...]`, `[name*=...]`),
  pseudo-classes (`:hover`, `:active`, `:focus`, `:first-child`,
  `:last-child`, `:nth-child(n)`, `:not()`), descendant /
  child / adjacent / sibling combinators, comma lists
- Declarations: property + value list + `!important`
- At-rules: `@media`, `@font-face`, `@keyframes`, `@import`, `@supports`
  (parsed; partial evaluation)
- Shorthands: expanded at parse time (`margin`, `padding`, `border`,
  `background`, `font`)
- Functions: `rgb()`, `rgba()`, `hsl()`, `url()`, `var()`, `calc()`
  (calc is parsed; limited evaluation initially — `+ - * /` with
  matching units)
- Custom properties: `--name: value;` and `var(--name, fallback)`

Edge cases we punt initially: counter functions, attr() in non-content
contexts, `@container`, `:has()`, complex grid track lists.

Aspirational size: ~1.5k LOC (tokenizer + parser + AST).

### Selector matcher (`src/style/selector.cpp`)

Right-to-left matching with a small bloom filter over ancestor class
hashes (the standard optimization). For each element, computes the
matching rule set; specificity ties broken by source order.

Aspirational size: ~600 LOC.

### Cascade + style resolution (`src/style/cascade.cpp`, `src/style/computed.cpp`)

Walks the DOM tree, accumulates matched declarations per element,
resolves them through origin (user-agent / author / inline) →
specificity → source order → `!important` flipping. Produces a
`ComputedStyle` per element. Then resolves to `UsedStyle` during layout
(percentages to absolute, `auto` to concrete, inheritance for inherited
properties).

The `ComputedStyle` struct is the hot one. Designed to be ~256 bytes,
trivially copyable, layout-cache-friendly.

Aspirational size: ~800 LOC.

### Layout (`src/layout/`)

The heart of the project. Three modes interleaved:

- **Block layout** (`block.cpp`) — block-in-block, with margins,
  padding, borders, `width`, `height`, `auto` resolution. ~600 LOC.
- **Inline layout** (`inline.cpp`) — line boxes, runs of text and
  inline elements, baseline alignment, vertical-align, line-height,
  white-space. ~600 LOC. This is the trickiest single file.
- **Flex layout** — Yoga. We translate `ComputedStyle` to Yoga's
  `YGNodeStyleSet*` calls, layout, read back frames. ~200 LOC of
  adapter.
- **Table layout** — deferred. Most modern CSS layouts don't need
  it; bootstrap uses tables for tabular data only.

Output is a **box tree** parallel to the DOM. Each box knows its
content rect, padding rect, border rect, margin rect, and which
element produced it (for input hit-testing).

### Text (`src/text/`)

`shaper.h` is an interface; default impl is "one glyph per codepoint."
HarfBuzz impl behind `AFFINEUI_ENABLE_HARFBUZZ`. Line breaker is naive
whitespace by default; libunibreak as an opt-in upgrade.
Glyph atlas via fontstash + stb_truetype. Measurement results cached
per `(font, run)` for the layout pass.

### Paint driver (`src/paint/`)

Traverses the box tree, emits `Painter::fill_rect` /
`Painter::stroke_rect` / `Painter::draw_text` / `Painter::draw_image`
calls. Handles `box-shadow`, `border-radius`, `background-image`,
`background-gradient`, `border` per side. Clip stack via
`Painter::push_clip` / `pop_clip`.

Aspirational size: ~700 LOC.

### Painter (`src/paint/nanovg_painter.cpp` + unity TU)

Default `Painter` implementation. Each engine call lowered to NanoVG.
NanoVG, in turn, talks to sokol_gfx. This is the only file that
touches NanoVG and the only place GPU draw calls are made.

Aspirational size: ~500 LOC.

#### Why the paint backend is a unity TU

To honor "sokol is local to our code" — i.e. a host that *also* uses
sokol must not link-collide with our copy — every sokol, NanoVG,
fontstash, stb_truetype, and stb_image symbol compiled by AffineUI is
**file-local**. We achieve this by consolidating all dep
implementations into a single translation unit:

```
src/paint/sokol_unity.c  (or .cpp)
  ├── #define SOKOL_API_DECL  static
  ├── #define SOKOL_API_IMPL  static
  ├── #define SOKOL_IMPL
  ├── #include "sokol_gfx.h"
  ├── #include "sokol_app.h"
  ├── #include "sokol_glue.h"
  ├── #define NANOVG_IMPL                static
  ├── #include "nanovg.h"   (and nanovg.c via include)
  ├── #include "nanovg_sokol.c"  (the sokol GL/Metal/D3D backend)
  ├── #define FONTSTASH_IMPLEMENTATION   static
  ├── #include "fontstash.h"
  ├── #define STB_TRUETYPE_IMPLEMENTATION static
  ├── #include "stb_truetype.h"
  ├── #define STB_IMAGE_IMPLEMENTATION    static
  └── #include "stb_image.h"
```

Above that, the unity TU exposes a tiny C ABI (`aui_paint_*`) declared
in `src/internal/paint_c_abi.h`. The C++ `Painter` implementation
(`nanovg_painter.cpp`) forwards every method to those functions. No
other TU in AffineUI ever sees `sg_*`, `nvg*`, `fons*`, or `stbi*`
symbols.

This pattern is the cost of the encapsulation guarantee. The cost is
small — one TU with ~80k LOC of vendored code parsed once per build —
and the win is that AffineUI integrates into any host without
duplicate-symbol surprises.

`AFFINEUI_HOST_PROVIDES_SOKOL=ON` (and the matching flags for NanoVG /
fontstash / stb_*) suppresses the corresponding sections of the unity
TU so the host's sokol context is used instead. The C ABI surface
stays the same; only its implementation knows the difference.

### App (`src/app/`)

Owns sokol_app lifecycle. Pumps events, maps OS events to our `Event`
type, dispatches into `Document`. Holds the embedder's view function
for imm mode. Drives the frame loop.

Aspirational size: ~400 LOC.

### imm reconciler (`src/imm/`)

Virtual DOM data structures, call-site identity (via
`std::source_location`), diff algorithm (Levenshtein-style with
key-stable matching),
patch ops applied to the retained DOM. State hooks
(`use_state<T>(initial)`) stored in a per-document map keyed by
call-site path. Dirty bit drives whether the next paint re-invokes
the view function.

Aspirational size: ~600 LOC.

## Aspirational engine size budget

```
src/dom/                ~500   LOC
src/html/               ~1200  LOC
src/css/                ~1500  LOC
src/style/              ~1400  LOC
src/layout/             ~1500  LOC + Yoga adapter (~200)
src/text/               ~400   LOC
src/paint/              ~1200  LOC
src/imm/                ~600   LOC
src/app/                ~400   LOC
─────────────────────────────────
total engine            ~8.7k  LOC
```

Plus vendored helpers under `external/`:

```
Yoga                    ~5k    LOC
NanoVG                  ~5k    LOC
sokol_*                 ~50k   LOC (single-header, mostly platform glue)
stb_truetype            ~6k    LOC
fontstash               ~1k    LOC
stb_image               ~3k    LOC
─────────────────────────────────
total helpers           ~70k   LOC (only ~10–15k actually compiled per build)
```

Stripped release binary target: **< 2 MB** with a working Bootstrap
demo.

## Memory model

- One `Document` owns one DOM tree, one ComputedStyle table, one box
  tree, plus image/font references it pulled in. Destroying the
  document releases everything.
- DOM nodes live in an arena (bump allocator), freed in one shot when
  the document is destroyed or `set_html` is called again.
- Box tree is a separate arena. Layout invalidation marks subtrees
  dirty; the arena is wiped and re-populated for dirty subtrees, leaving
  clean subtrees in place.
- Image cache: process-wide LRU keyed by URL string, size-capped.
- Font registry: process-wide; faces reference-counted by fontstash.

## Threading

Single-threaded by default. Layout and paint happen on the frame thread.
`Document::set_html` can be called from any thread *between* frames;
concurrent layout/paint is not supported.

## Extensibility points

- **Custom protocols.** `App::Config::resource_loader` lets you serve
  `app://`, `mem://`, etc.
- **Custom elements.** A registry maps tag name → factory that returns
  a custom `Box` subclass. Useful for `<game-canvas>` that wants to
  render via raw sokol_gfx calls into the same pass.
- **Style hot-reload.** `Document::reload_stylesheets()` re-parses the
  CSS without touching the DOM.
- **Custom painter.** Implement the `Painter` interface to render
  somewhere other than sokol_gfx (off-screen image, another graphics
  stack, hardware composer).
- **Custom text shaper.** `Shaper` interface; default impl is glyph-
  per-codepoint, HarfBuzz available, write your own if needed.

## Non-goals (explicit)

- JavaScript execution
- Network stack (HTTPS, WebSockets) — bring your own
- Accessibility tree / screen reader support (not yet)
- Print layout, multi-column, CSS grid (deferred to Phase 6)
- Subpixel-accurate sRGB compositing parity with Chromium
- Robust parsing of malformed HTML (use Gumbo as a parser swap-in
  if you need this)
- Complex-script text shaping by default (HarfBuzz hook available)

See [DESIGN.md](DESIGN.md) for the rationale behind specific design
choices.
