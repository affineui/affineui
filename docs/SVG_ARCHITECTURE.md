# Scoped SVG Architecture

Status: technical design target. This document is the contract for
adding SVG to AffineUI without adding a second browser-shaped rendering
engine.

AffineUI is not trying to become a general browser. SVG support follows
the same rule as the rest of the HTML/CSS engine: implement the parts
that real native UI needs, implement them correctly, and keep the hot
path small enough to trust in an interactive tool.

The first-class targets are Bootstrap-style HTML/CSS and Decius UI.
Decius icons are compiled into the `decius-icons` font and must render
through the normal text/glyph atlas path. Runtime SVG support is for the
places Decius and similar UI libraries genuinely use SVG as vector
drawing: knobs, patch jacks, LCD digits, patch cables, graphs, scopes,
and small editor overlays.

## Design Goals

1. Preserve the engine shape: DOM -> style -> layout -> display list ->
   raster/cache -> composite.
2. Treat SVG as retained scene data, not as immediate-mode paint code.
3. Make stable SVG behave like an image after first rasterization.
4. Make dynamic SVG mutate numbers, not parsed strings.
5. Keep the first implementation narrow enough to verify with Decius
   conformance and allocation instrumentation.
6. Keep the backend seam clean. NanoVG is the first backend, not the
   architecture.

## Non-Negotiable Rules

- SVG is parsed into retained data. It is never reparsed during a paint
  frame.
- Stable SVG is rasterized into a cached texture or atlas entry. The
  same SVG, same resolved state, same CSS size, and same device scale
  must return the existing texture.
- Dynamic SVG mutates retained numeric data. It must not rebuild XML,
  tokenize `d` strings, allocate path storage, or recreate gradients
  per frame.
- SVG support must not become a shortcut around missing HTML/CSS
  features. Basic widgets, panels, font loading, generated content,
  gradients, shadows, layout, and form controls remain higher-priority
  conformance work.
- Unsupported SVG features fail visibly but safely. They must not crash
  style resolution, layout, or paint.
- No class-specific Decius hacks. Decius is a target corpus, not a
  special-case branch.

## System Overview

```
HTML DOM subtree
    |
    | collect_blocks sees <svg>
    v
SvgSceneBuilder parses SVG subtree once
    |
    | scene handle stored on Block
    v
Layout treats SVG as replaced/foreign content
    |
    | Document::draw emits SVG display-list op
    v
SvgRasterCache lookup by geometry + paint + pixel size + DPI
    |
    +-- hit  -> draw cached texture/atlas region
    |
    +-- miss -> rasterize retained scene through backend, insert cache,
                then draw cached texture/atlas region
```

Dynamic SVG follows the same route, but the scene has mutable numeric
ranges. Dynamic scenes can be marked vector-live so stable subparts
cache while the changing subpart emits vector ops or rasterizes only its
own layer.

## Runtime Scope

### In Scope First

These cover the important Decius and native-tooling use cases.

- Inline `<svg>` as replaced/foreign content.
- `width`, `height`, `viewBox`, `preserveAspectRatio`.
- Basic shapes: `path`, `rect`, `circle`, `ellipse`, `line`,
  `polyline`, `polygon`.
- Path commands needed by Decius: `M`, `L`, `H`, `V`, `C`, `S`, `Q`,
  `A`, `Z`, plus relative forms.
- Fill/stroke paint with `none`, color values, `currentColor`, CSS
  variables after normal style resolution, opacity, stroke width,
  line cap, line join, and dash arrays.
- Simple gradients in `<defs>`: `linearGradient` and `radialGradient`.
- Clip paths only where required by conformance targets.
- `filter: drop-shadow(...)` as a cache key and raster step for small
  SVG widgets; broader SVG filter graphs are out of scope.

### Explicitly Out Of Scope Initially

These stay parked until a real target requires them.

- SVG scripting, events inside SVG, SMIL animation.
- `<foreignObject>`.
- Text layout inside SVG beyond simple fallback needs.
- Masks, complex filter graphs, blend modes, markers, patterns.
- Full CSS selector/cascade behavior inside arbitrary SVG subtrees.
- Spec recovery for malformed SVG.
- Full browser-equivalent SVG DOM APIs.

## Workload Classification

### 1. Icon Font

Example: Decius `<i class="di di-cog"></i>`.

This is not runtime SVG. The SVG sources belong to Decius' build step.
AffineUI runtime responsibility is:

- `@font-face` loading and resource resolution.
- Generated content such as `.di-cog::before { content: "\e024"; }`.
- Correct font-family resolution for `decius-icons`.
- Glyph atlas reuse through the existing font path.

### 2. CSS Drawing

Example: panels, bevels, lit buttons, switches, sliders, faders.

These are regular HTML/CSS boxes. They should be solved through layout,
paint primitives, display-list hashing, and compositor layer caching.
Do not route CSS gradients or shadows through the SVG subsystem.

### 3. Stable Inline SVG

Example: patch jack chrome, LCD seven-segment digits, static graph
chrome.

These are ideal raster-cache candidates. Parse to `SvgScene`, resolve
paint state, rasterize once for the requested size and device scale,
then composite the cached texture.

### 4. Dynamic Vector SVG

Example: knob arcs, patch cables with spring physics, curve editors,
node graph wires, oscilloscope traces.

These should use retained vector scenes or direct vector display-list
ops. The changing state is numeric: endpoints, control points, sweep
angles, colors, opacity, and transforms. The engine must update those
numbers without reparsing XML or path strings.

When a dynamic element has stable subparts, split them:

- static track/ring/background: raster cache
- dynamic arc/cable/trace: retained vector draw or small dynamic layer

## Core Data Model

SVG scene data is owned by `Document`; GPU raster cache entries are
owned by `Renderer`.

```cpp
struct SvgScene {
    SvgViewBox view_box;
    PreserveAspectRatio aspect;
    ArenaRange<SvgPaintDef> paint_defs;
    ArenaRange<SvgNode> nodes;
    Hash geometry_hash;
    SvgSceneDiagnostics diagnostics;
};

struct SvgNode {
    SvgNodeKind kind;
    uint32_t element_serial;
    SvgPaintToken fill;
    SvgPaintToken stroke;
    float stroke_width;
    SvgStrokeFlags stroke_flags;
    SvgTransform transform;
    Range<SvgCommand> path;
    Range<Point> points;
};

struct SvgSceneDiagnostics {
    uint16_t unsupported_node_count;
    uint16_t unsupported_attribute_count;
    uint16_t unsupported_paint_count;
};
```

`geometry_hash` is computed from geometry, transforms, and structural
paint references. Resolved colors are excluded so a theme change can
reuse the same scene and only change the raster key.

Scene nodes store numbers, interned ids, and arena offsets. They do not
store borrowed DOM pointers for paint-time use.

## Pipeline

### 1. DOM Collection

`<svg>` is collected as foreign replaced content, not as normal HTML.
It owns a `SvgSceneHandle` and participates in layout as a box with
intrinsic size from `width`, `height`, or `viewBox`.

```cpp
struct Block {
    ...
    SvgSceneHandle svg_scene;
    SvgLayoutHints svg_hints;
};

struct SvgLayoutHints {
    float intrinsic_width;
    float intrinsic_height;
    bool  has_intrinsic_width;
    bool  has_intrinsic_height;
};
```

The collector must not pass arbitrary SVG elements through the HTML
style resolver. That path is for HTML elements. SVG descendants are
parsed by the SVG scene builder.

If an SVG has no explicit size and no viewBox, use the browser default
intrinsic size for inline SVG. The value should be centralized so
conformance can tune it once.

### 2. Scene Build

The scene builder walks the SVG subtree once and emits compact scene
data. The builder records unsupported features in diagnostics and keeps
the supported subset. Diagnostics feed debug overlays and conformance
logs; they do not abort rendering.

The builder runs when:

- a new SVG DOM subtree appears
- a geometry-affecting SVG attribute changes
- a referenced `<defs>` node changes

The builder does not run when:

- the SVG moves
- only inherited color changes
- only CSS variables used by paint change
- only output size or device scale changes

### 3. Style And Paint Resolution

SVG paint resolution happens after normal element style resolution has
provided inherited values such as `color` and custom properties.

The system keeps authored paint separate from resolved paint:

```cpp
enum class SvgPaintTokenKind : uint8_t {
    None,
    CurrentColor,
    Color,
    UrlRef,
    CssVar,
};

struct SvgResolvedPaint {
    SvgResolvedPaintKind kind;
    Color color;
    GradientHandle gradient;
    float opacity;
};
```

This split is important. A theme change invalidates resolved paint and
the raster key; it does not rebuild `SvgScene`.

The resolved paint key includes:

- `currentColor`
- referenced CSS variables used by SVG paint
- fill/stroke colors and opacity
- stroke width, caps, joins, dashes
- gradient definitions after resolving colors
- relevant filter state

The key does not include pure translation; translation is compositor
state. It does include output pixel size and device scale for raster
cache entries.

### 4. Display List Emission

Paint emits an SVG display-list op that references retained scene data:

```cpp
enum class PaintOpKind : uint8_t {
    ...
    DrawSvg,
};

struct DrawSvgOp {
    uint32_t scene_handle;
    uint32_t paint_key;
    int16_t  x, y, w, h;
    uint16_t flags;
};
```

The display list stays POD. Complex scene data remains in
document-owned or renderer-owned tables referenced by stable handles.

At replay/raster time, `DrawSvg` lowers to one of two paths:

- Cache hit: draw cached texture/atlas region.
- Cache miss or vector-live: rasterize or replay retained vector scene.

The cache-miss path is allowed to rasterize a retained scene. It is not
allowed to reparse SVG text.

### 5. Raster Cache

The SVG raster cache sits beside the existing image/pattern caches. It
uses a structured key:

```cpp
struct SvgRasterKey {
    Hash geometry_hash;
    Hash paint_hash;
    uint16_t pixel_width;
    uint16_t pixel_height;
    uint16_t dpi_x100;
    uint8_t  aspect_mode;
    uint8_t  filter_flags;
};
```

Cache entries are GPU images or atlas regions. Small entries should pack
into the same small-texture atlas strategy used for layers. Large
entries may allocate standalone textures.

The renderer owns the cache because texture handles are backend
resources. The document owns scenes because scene lifetime follows DOM
content.

```cpp
class SvgRasterCache {
public:
    SvgRasterResult lookup(const SvgRasterKey& key);
    SvgRasterResult get_or_rasterize(const SvgScene& scene,
                                     const SvgResolvedPaintTable& paint,
                                     const SvgRasterKey& key,
                                     SvgRasterBackend& backend);
    void trim_to_budget(size_t bytes);
};
```

Eviction is LRU and size-capped. Destroying the originating DOM node
releases scene references, but raster entries may remain until evicted.

## Ownership And Lifetimes

| Data | Owner | Lifetime | Hot-frame mutation |
|---|---|---|---|
| Raw SVG DOM nodes | Lexbor document | DOM lifetime | no |
| `SvgScene` geometry | `Document` arena | until DOM/SVG attr change | no, except dynamic ranges |
| `SvgResolvedPaint` | `Document` or frame scratch | until style/custom props change | no |
| Raster texture/atlas entry | `Renderer` cache | LRU/device lifetime | no |
| Dynamic command values | owning element/layer | state lifetime | yes, numeric only |
| Display-list SVG op | frame display list | one frame | append only |

Device loss destroys renderer-owned textures but not document-owned SVG
scenes. After device restore, cache entries are recreated from retained
scenes on demand.

## Invalidation

SVG invalidation must be narrower than document invalidation.

| Change | Required Work |
|---|---|
| Box translated only | Composite cached texture at new position |
| Box resized or DPR changed | Lookup/rasterize new raster key |
| `color` or CSS variable used by SVG changed | Recompute paint key, lookup/rasterize |
| Shape attribute changed (`d`, `points`, `cx`) | Rebuild scene geometry for that SVG |
| Dynamic path numeric state changed | Update retained commands, repaint only dynamic layer |
| Static ancestor layout changed | Relayout box, reuse scene and maybe texture |
| Unsupported feature encountered | Mark scene partially unsupported; draw supported subset |

The important distinction: style/layout changes may require a new cache
lookup, but they do not imply reparsing the SVG tree.

## Dynamic SVG Contract

Dynamic vector widgets must be authored or lowered into stable command
storage:

```cpp
struct SvgDynamicPath {
    Range<SvgCommand> commands;
    Hash stable_shape_class;
    uint32_t generation;
};
```

Changing a patch cable endpoint updates command numbers and increments
`generation`. It does not replace the command vector. The display list
or raster key can include `generation` for the dynamic layer while
static siblings keep their existing cache entries.

For Decius-style components:

- Knob: static track ring caches; active arc is one retained arc/path.
- Jack: full socket caches; patched indicator can be separate tiny
  overlay or part of a state-keyed cache entry.
- LCD digit: each glyph/color/size is cacheable; changing value selects
  existing digit scenes or cached textures.
- Patch cable: plug heads cache; cable body is retained cubic path.
- Scope/curve graph: grid/background cache; trace path is dynamic.

## Backend Strategy

NanoVG is the first vector backend because it already handles paths,
fills, strokes, arcs, and gradients in the current paint pipeline. The
public `Painter` interface should grow only the minimal vector
primitives needed by the retained scene/display-list seam.

Initial fallback can lower SVG nodes directly into existing or new
Painter ops:

- `fill_polygon`
- `stroke_polyline`
- `fill_path`
- `stroke_path`
- `draw_cached_svg_image`

Longer-term, the NanoVG/sokol backend should rasterize cache misses into
offscreen images or atlas regions, then the compositor draws them like
any other layer texture.

Backend requirements:

- Create/update small RGBA or render-target textures.
- Draw a cached texture into the current pass.
- Rasterize retained vector commands into a texture without exposing
  backend handles to `Document`.
- Survive device loss by invalidating only renderer-owned cache entries.

The initial implementation can replay vector ops directly to NanoVG
while the cache API is introduced. That is acceptable only as an
incremental step; the retained scene and cache keys must be present from
the start so direct replay does not become the architecture.

## Allocation Rules

Hot-frame SVG work must be allocation-free after warm-up.

- Scene node/path storage lives in document-owned arenas.
- Display-list ops reference scene handles or arena ranges.
- Raster cache keys are fixed-size structs.
- Dynamic path updates write into preallocated command arrays or a small
  reusable scratch buffer owned by the element/layer.
- Cache misses may allocate. Cache hits must not.

Instrumentation is part of the design:

```cpp
struct SvgStats {
    uint64_t scene_parses;
    uint64_t scene_rebuilds;
    uint64_t raster_cache_hits;
    uint64_t raster_cache_misses;
    uint64_t raster_cache_evictions;
    uint64_t dynamic_path_updates;
    uint64_t unsupported_features;
    uint64_t bytes_allocated_this_frame;
};
```

Conformance and demo builds should be able to dump these counters. A
steady static frame should show zero scene parses, zero raster misses,
and zero SVG allocations.

## Conformance Plan

SVG tests should be grouped separately from core HTML/CSS tests so they
do not distract from basic widget conformance.

Recommended groups:

- `svg_basic_shapes_*`
- `svg_path_commands_*`
- `svg_paint_current_color_*`
- `svg_gradients_*`
- `svg_viewbox_aspect_*`
- `svg_decius_jack_*`
- `svg_decius_lcd_*`
- `svg_decius_knob_*`
- `svg_decius_cables_*`

Animation and interaction tests should use filmstrip snapshots when the
time dimension matters. For dynamic SVG, expected conformance is not
just final pixels; it includes smooth motion with stable allocation and
no per-frame parsing.

## Implementation Phases

### Phase A: Safety And Shape

- Replace the current `<svg>` skip with a foreign-content block.
- Store a retained `SvgSceneHandle` on the block.
- Support intrinsic sizing from `width`, `height`, and `viewBox`.
- Draw unsupported SVG as an obvious placeholder in debug builds, no
  crash in release.
- Add stats counters even if most values are initially zero.

### Phase B: Basic Static SVG

- Implement retained scene parsing for shapes and simple paths.
- Add minimal Painter/display-list vector ops.
- Render Decius LCD digits and jack sockets without class-specific
  code.
- Add conformance cases for basic shapes, viewBox, currentColor, and
  Decius static widgets.

### Phase C: Raster Cache

- Add `SvgRasterKey` and LRU texture/atlas cache.
- Cache stable SVG by geometry, paint, size, and DPI.
- Add instrumentation counters: scene parses, raster cache hits,
  misses, evictions, bytes allocated.
- Verify repeated frames do not parse or allocate.
- Add device-loss cache invalidation tests if the backend supports
  simulated reset.

### Phase D: Dynamic SVG

- Retain dynamic vector commands for knobs, graph wires, cables, and
  traces.
- Split static subparts from dynamic subparts where possible.
- Add filmstrip conformance tests for knob arcs and patch cables.
- Add allocation checks around animation frames.

### Phase E: Broader SVG Features

Only after core UI conformance and the Decius first-class surfaces are
solid:

- More path commands and edge cases.
- Clip paths needed by graphs/scopes.
- Larger gradient/filter subset.
- Text-in-SVG if a real target requires it.

## Success Criteria

SVG support is successful when:

- Decius icons render through the font system, not SVG.
- Static SVG widgets parse once and then render from cache.
- Dynamic widgets update retained numeric state and avoid per-frame
  parser/string churn.
- Cache-hit frames allocate zero bytes in SVG code.
- The architecture improves performance pressure instead of adding a
  second rendering engine inside AffineUI.
