# Optimization strategy

The performance shape of AffineUI is the central architectural pressure
on every other decision. Read [DESIGN.md § Real-time render
architecture](DESIGN.md#real-time-render-architecture) first for the
high-level pipeline and
[RENDERER_COMPOSITOR.md](RENDERER_COMPOSITOR.md) for the current retained
renderer/compositor implementation; this document is the concrete
playbook.

The headline claim:

> **Idle UI costs microseconds. Animations cost composite-only.
> Content changes cost only the affected layer.**

Three optimization families let us hit that:

1. **Animation** — keep animating frames off the engine entirely.
2. **CSS dependency tracking** — when content does change, recompute the
   smallest possible slice.
3. **Compositing** — make the always-on per-frame work negligible.

Each family is a list of techniques. Some land in Phase 2; some are
deferred but the architecture must *not preclude them*.

---

## 1. Animation

### 1.1 Composite-only animatable properties

The following CSS properties animate **without touching style, layout,
paint, or rasterize stages** — they update one uniform on a cached layer
texture and that's it:

| Property | Composite cost |
|---|---|
| `transform: translate / rotate / scale / skew / matrix` | one uniform |
| `opacity` | one uniform |
| `filter: blur(N)` (within a per-layer blur budget) | one uniform + blur pass |
| `scroll-offset` inside `overflow: scroll/auto` | one uniform on inner layer |

Every other property animates the *slow* path (style → paint → rasterize
→ composite). The contract is: if you want a 120 fps animation, animate
one of these properties; otherwise expect that frame to do real work.

### 1.2 Layer promotion via `will-change`

```css
.tooltip { will-change: transform, opacity; }
```

When this hint appears, the element is promoted to its own layer
**before** the animation runs. First-frame hitches — texture allocation,
rasterize, atlas insert — happen during idle time instead of mid-
animation. Without the hint, the engine still promotes when it sees an
active transition/animation on a composite-only property, but the
promotion frame may be visible.

Other automatic promotion triggers (mirror Chrome's heuristics for the
same reasons):

- non-identity `transform` (static or animated)
- `opacity < 1`
- `position: fixed`
- `overflow: hidden | scroll | auto` with scrollable children
- 3D transforms or `perspective`
- explicit imm-mode `key()` on an animation-marked list item

We deliberately do **not** promote every element — too many small
layers becomes a texture-binding bottleneck. The defaults are tuned for
the modal/tooltip/slide-in patterns that dominate real UI.

### 1.3 GPU-driven keyframe interpolation

Each active animation is a tiny struct:

```cpp
struct ActiveAnimation {
    LayerId      layer;
    PropertyId   property;     // transform / opacity / filter
    KeyframeId   curve;        // index into shared keyframe table
    double       start_time;
    float        duration_s;
    uint8_t      iteration_count;  // 0 = infinite
    uint8_t      easing;       // linear / cubic-bezier preset / spring
    bool         paused;
};
```

The compositor reads `Document::animation_clock()` at frame start,
samples each `ActiveAnimation`, computes the interpolated value, and
writes it into the layer's transform/opacity uniform.

No re-entry into the engine. No restyle. No relayout. The compositor
does the math directly.

### 1.4 Spring physics as a first-class easing

Modern UI motion ("snap to position," "scroll bounce," "drawer slide")
is naturally a damped harmonic, not a Bezier. Springs are cheap to
evaluate (`x_t = (x_0 - x_target) * exp(-ζω·t) * cos(ω·√(1-ζ²)·t) + x_target`),
and one set of `(stiffness, damping, mass)` parameters covers most of
what designers tweak. We ship spring as a first-class easing alongside
the standard Bezier presets.

### 1.5 Off-screen and occluded animations sleep

Layers fully outside the viewport, or fully covered by an opaque
later-in-z layer, **don't sample their animations**. The clock keeps
running; when the layer becomes visible again, animations catch up
based on elapsed time. This is the same trick browsers use for
`<video>` and `requestAnimationFrame` in background tabs.

### 1.6 Frame-rate adaptive scheduling

`sokol_app` reports the display's refresh rate. We sample animations at
exact frame boundaries derived from that rate, not at an oversampled
real-time clock. At 120 Hz that's 8.3 ms per sample; at 60 Hz, 16.6 ms.
No phase drift, no double-sampling, no missed frames.

### 1.7 Animation budget exhaustion fallback

If active animation count exceeds a budget (~32 concurrent), the
compositor batches uniform updates into a single buffer write and
issues one instanced draw call. Past ~256 animations, we degrade
gracefully — long-running animations near the end of their duration get
sampled at half rate. Configurable.

---

## 2. CSS dependency optimization

The default "re-cascade everything on any change" approach is what
makes desktop UI feel slow. The five techniques below take incremental
restyle from O(document) to O(affected subset).

### 2.1 Rule indexing by rightmost simple selector

At stylesheet parse time, each rule is bucketed by its rightmost simple
selector:

```
.foo .bar     → bucket "bar"
div > p       → bucket "p"
#nav a:hover  → bucket "a"
```

An element of tag `T` with classes `{c1, c2}` and id `id1` only
considers rules in buckets `T`, `c1`, `c2`, `id1`. For a typical
Bootstrap-style stylesheet this drops candidate-rule count by ~100x.

### 2.2 Bloom filter prefilter for ancestor checks

For each element, build a Bloom filter of all its ancestors'
tag/class/id hashes — ~64 bits, single cache-line access. Before
descending into the multi-step matcher for a rule like `.modal .header
.title`, check whether `.modal` and `.header` are *possibly* in the
ancestor set. False positives are cheap (fall through to real match);
true negatives skip the rule entirely.

### 2.3 Invalidation sets

For each rule, compute and store at parse time: "what DOM mutation
could change which elements match me?" Stored as a compact set of
(class-add, class-remove, attr-change, sibling-insert) triggers.

Runtime: when an element's class list changes from `{a, b}` to
`{a, c}`, the engine consults invalidation sets keyed on `b` and `c`,
walks just the rules in those sets, and marks just the elements those
rules can affect as style-dirty. Everything else stays clean.

This is the single biggest win for the imm-mode reconciler, which
generates lots of small class/attr changes.

### 2.4 Computed-style sharing

Siblings with identical matched-rule sets *and* identical inherited
values share one immutable `ComputedStyle`. Refcounted. Common case:
`<li>` items in a list, or rows in a data table.

When a sibling diverges (different class, different state), it
allocates a new `ComputedStyle` and the refcount drops. Worst case is
the same as no-sharing; best case is one allocation for hundreds of
list items.

### 2.5 Inheritance cache pointer

For each inherited property (`color`, `font-family`, etc.), each
element stores a pointer to the nearest ancestor that *owns* a value
(not inherited). Lookup is one indirection regardless of tree depth.
On style invalidation the pointer updates lazily.

### 2.6 Subtree dirty bits + dirty list

DOM mutations propagate a subtree-dirty bit upward (so traversal can
skip clean siblings) and add the mutated element to a per-document
dirty list. The next restyle pass walks the dirty list, not the whole
document.

### 2.7 Custom property dependency tracking

`var(--color)` is a runtime indirection. Each element that uses a
custom property registers as a dependent on that property's *defining
node*. When the custom property's value changes, only registered
dependents are style-dirtied — not every descendant of the defining
node.

Critical for modern theme-toggle patterns where one `:root --bg-color`
change should invalidate ~10 elements that actually reference it, not
the whole document.

### 2.8 Pre-computed selector specificity

Specificity computation happens once at parse time and is stored
inline in the rule. The cascade pass becomes a single integer
comparison.

### 2.9 String interning everywhere

Tag names, class names, attribute names, custom property names →
interned to a `u32` at first sight. Equality compares are integer
compares. Hash-table lookups bypass string hashing.

### 2.10 Restyle batching within a frame

Multiple DOM mutations in the same frame (think: imm reconciler
applying ten patches) coalesce into one restyle pass. The pass runs
once, just before layout, against the union of dirty elements.

### 2.11 `@media` query caching

Media queries evaluate against the viewport once per viewport change,
not per element. Result is cached as a bitmask; rule lookup masks
against the active media set.

### 2.12 Specificity-stratified rule application

Within a bucket, rules are sorted at parse time by (origin,
specificity, source-order). The cascade pass scans in order and stops
as soon as it has a winner for each property — important for
`!important` and tight loops.

---

## 3. Compositing

The compositor is the only stage that runs every frame for idle UI.
Its budget is microseconds.

### 3.1 Display-list hashing

Each layer's display list has a rolling hash computed as paint emits
it. If the new hash equals the cached hash after a restyle, rasterize
is **skipped entirely** — the cached layer texture is reused unchanged.

This is the catch-all "your style invalidation triggered but the
actual paint output is identical" optimization. Cheap to maintain;
catches a surprising fraction of pseudo-invalidations.

### 3.2 Texture atlas packing

Small layer textures pack into a shared atlas (initial size 4096×4096).
The compositor sees N layers but binds 1–2 atlases. Atlas insertion is
a simple skyline packer; eviction is LRU.

Cutoff: layers larger than ~512×512 bypass the atlas and get their own
texture. Atlas is for the long tail of small layers (tooltips, badges,
icons).

### 3.3 Quad batching

Layers sharing the same atlas + same blend mode + monotonically
increasing z merge into one draw call with N instances. For a typical
UI this collapses 30 layers into 2–3 draws.

### 3.4 Damage tracking

The compositor tracks "what rectangle changed since last frame." For
static UI with one animating layer, only that layer's quad is uploaded
and only the union rect is presented. The rest of the framebuffer is
unchanged (scissor + no-clear strategy).

### 3.5 Texture pool / recycler

When a layer goes away, its texture goes to a per-size free-list, not
to `glDeleteTextures`. Modal opens/closes, tooltip flickers,
imm-reconciliation churn — none of these allocate GPU memory after
warm-up.

### 3.6 Premultiplied alpha throughout

Layer textures are stored premultiplied. The composite shader is:

```glsl
out = layer_color * layer_alpha + dst * (1.0 - layer_alpha);
```

No per-pixel multiply, no special-casing. Rasterize writes
premultiplied; compositor reads premultiplied.

### 3.7 Mipmap generation for scaled layers

Layers that animate `transform: scale(< 1)` get mipmaps generated at
rasterize time. Trilinear sample at composite. Avoids the shimmer
artifact when scaling down without much per-frame cost.

### 3.8 Sub-pixel positioning is composite-only

Rasterization writes at the pixel grid. The composite uniform's
translate is float-valued. Sub-pixel motion (smooth scroll, easing
ends) updates the uniform; the texture stays put. Avoids re-rasterizing
on every frame of fractional motion.

### 3.9 Tiled rasterization for oversized layers

A layer larger than the viewport (long scrollable list) rasterizes only
its visible tiles. Each tile is a 256×256 region; the compositor
assembles them at composite time. Off-screen tiles aren't rasterized
until scrolled into view.

### 3.10 Zero-allocation per frame

The compositor maintains all its buffers across frames. One VAO,
N atlas textures, one shader, one uniform buffer, one quad-instance
buffer (grown as needed, never shrunk). Per-frame work is: update
uniforms, update damaged regions, issue draws, swap.

### 3.11 Single-layer fast path

If the entire document fits in one layer (no transforms, no opacity,
no scrolling) we skip the texture indirection entirely. Rasterize
straight to the backbuffer. This is the common case for static dialogs
and small tool windows — they pay zero compositing cost.

### 3.12 Z-order sorted once per layout

Layers carry a stable z value derived from CSS `z-index` and stacking-
context rules. Sorted once after each layout; per-frame is a linear
traverse. Insertions update the sorted list in O(log N).

### 3.13 Occlusion culling

A layer fully covered by an opaque later-in-z layer skips composite.
A simple front-to-back pass over opaque layers builds an opaque mask;
covered layers are dropped.

### 3.14 Compositor and rasterizer share fonts/atlases

The rasterizer's font atlas (managed by fontstash) is bound to the
same GL context the compositor uses. No texture copying, no API
hand-off. Both stages see the same `GLuint`.

---

## Phasing

Not all of this lands at once. Phase 2 ships the foundations; later
phases unlock the upper-bound performance.

| Phase | Animation | CSS deps | Compositing |
|---|---|---|---|
| **2 (MVP)** | composite-only transform/opacity; auto layer promotion | rule indexing; specificity pre-compute; string interning | display-list hashing; texture pool; single-layer fast path |
| **3** | will-change hint; spring easing; frame-rate adaptive | bloom prefilter; subtree dirty bits; restyle batching | atlas packing; quad batching; damage tracking |
| **4** | off-screen sleep; budget fallback | invalidation sets; computed-style sharing; custom-prop deps | mipmaps; tiled rasterize; occlusion culling |
| **5+** | designer-tunable spring presets | inheritance cache; sibling invalidator graphs | sub-pixel composite; specialized blur shaders |

What's **non-negotiable for Phase 2**: the *seams*. A retrofit of any
optimization above must not require rewriting the engine's data flow.
The next section lists the seam commitments that protect that.

---

## Seam commitments (Phase 2)

These shapes are load-bearing — every other decision honors them so
the optimizations above can be retrofitted without a rewrite.

- **ComputedStyle is a value type, refcountable, ~256 B.** Sharing is
  a future drop-in; the type doesn't change.
- **Display list is compact paint ops plus derived replay metadata.**
  `vector<PaintOp>` and the text pool define the content hash; prepared
  bounds and jumps such as transformed-range and clip-range metadata are
  derived after recording and may be ignored by consumers that do not
  need culling.
- **Layer owns one texture handle + one transform matrix + one
  opacity.** Animation is "update the matrix/opacity uniform."
- **Style invalidation flows through a `restyle_queue` not direct
  cascade calls.** Adding invalidation sets later replaces the queue's
  producer, not its shape.
- **Cascade is a separate engine phase**, not interleaved with layout.
  Layout consumes immutable ComputedStyle.
- **Composite is a free function** (well: a class with no virtuals)
  driven by a layer tree and a clock. Adding atlas packing, damage
  tracking, or occlusion culling is internal to that function.
- **The rasterizer and compositor share the same GL context** and
  font atlas. No data hand-off between them — they read the same
  GPU-side resources.

If we get these shapes right, the engine pays for Phase 2 once and
unlocks Phase 3–5 incrementally. Get them wrong and we rewrite.
