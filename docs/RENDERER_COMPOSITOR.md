# Renderer and Compositor Architecture

This document describes the graphics side of AffineUI: how a retained
document becomes GPU pixels, how redraw work is avoided, what the current
compositor does, and where the next compositor/layer work should land.

The current implementation lives primarily in:

- `include/affineui/renderer.h`
- `include/affineui/embed.h`
- `include/affineui/sokol.h`
- `src/render/renderer.cpp`
- `src/internal/display_list.h`
- `src/internal/display_list_painter.h`

## Design Goals

The renderer exists to make this claim true:

> Static UI should cost essentially nothing. Small state changes should
> redraw only the affected region. Animation should move toward
> composite-only work.

That means the renderer must:

- avoid reparsing HTML/CSS during normal frames;
- avoid relayout when computed layout fields did not change;
- avoid re-recording paint when the document is unchanged;
- avoid rerasterizing when recorded paint is byte-identical;
- avoid rerasterizing the full surface for bounded paint-only changes;
- eventually animate transform/opacity by updating compositor state only.

Correctness wins over cleverness. Every optimization has a conservative
fallback to full record, full layout, or full root-layer raster.

## Ownership Model

`Renderer` owns graphics resources. `Document` owns DOM, style, layout,
and paint traversal. `Ui` ties the two together.

`Renderer` owns:

- the NanoVG context;
- the concrete `Painter`;
- the cached `DisplayList`;
- the retained root-layer texture and attachments;
- render statistics;
- sokol_gfx setup only in embedded mode.

`Renderer` does not own:

- the window;
- input;
- the application event loop;
- the host swapchain or host render-target textures.

Per frame, the host lends AffineUI a `FrameTarget`. AffineUI uses the
borrowed render target during the call and retains nothing from it.

## FrameTarget

`FrameTarget` is the boundary object for embedded and standalone
rendering. It contains the transient render-target handles for the
active graphics backend plus:

- framebuffer width and height in pixels;
- DPI scale;
- sample count;
- optional viewport rectangle;
- clear/load behavior;
- commit behavior.

The target is borrowed for one call. It is never stored.

The standalone sokol adapter builds a `FrameTarget` from
`sglue_swapchain()`. Embedded callers build one from their own render
target views.

## Pipeline Overview

The retained render path has three major stages.

```text
Document state
  -> prepare_frame()
       -> layout if needed
       -> collect dirty rects
       -> check active animations
       -> record DisplayList if document changed
       -> compare DisplayList hash against cached list
  -> root-layer raster
       -> full raster, partial raster, or reuse
  -> composite root layer into FrameTarget
```

The important distinction:

- display-list recording is CPU work from `Document::draw`;
- root-layer raster is GPU work through NanoVG into an offscreen texture;
- composition draws the retained texture into the actual frame target.

## Display List

`DisplayList` is the renderer's retained paint command stream.

`Document::draw()` does not call NanoVG directly in the retained path.
It paints through `DisplayListBuilder`, which records compact `PaintOp`
records plus a text pool.

The display list is designed to be:

- deterministic for identical document state;
- cheap to compare using `content_hash`, op count, and text-pool size;
- replayable into any `Painter`;
- independent of the swapchain.

`prepare_frame()` records a new display list only when something in the
document could affect paint:

- viewport changed;
- layout is dirty;
- full paint dirty flag is set;
- dirty rects are present;
- animations are active;
- the previous frame had active animations.

If none of those are true, the cached display list is reused without
calling `Document::draw()`.

If a new display list is recorded but its content hash and sizes match
the cached list, the renderer treats it as unchanged. This catches cases
where an event or dirty path ran but emitted pixels did not change.

## Root Layer

The root layer is an offscreen GPU texture containing the rasterized
document.

The root layer owns:

- color image;
- depth/stencil image;
- texture view for sampling the color image;
- color attachment view;
- depth/stencil attachment view;
- NanoVG image handle wrapping the color image;
- validity flag and dimensions.

The root layer is recreated when its dimensions change. Otherwise it is
retained across frames.

## Full Raster

Full raster is the safe path.

It happens when:

- there is no valid root layer;
- the viewport changed;
- layout changed;
- a full paint dirty fallback was requested;
- active animation currently requires paint/raster;
- the dirty region is unavailable;
- the display list changed for a reason that cannot be bounded.

Full raster clears the root layer to transparent, replays the whole
cached display list through NanoVG, and marks the layer valid.

## Partial Raster

Partial raster is the first dirty-rect optimization layer.

It is allowed only when all of these are true:

- a valid root layer already exists;
- the display list changed;
- dirty rects are present;
- the dirty union is valid;
- viewport did not change;
- layout did not change;
- no full paint dirty fallback is set;
- no active animation requires paint/raster.

The dirty rects are unioned, inflated slightly for antialiasing/shadow
fringes, and clipped to the frame.

The partial path is currently behind a correctness gate in the live renderer.
The retained root layer still gives zero-cost static frames, but any
display-list change is conservatively re-rasterized as a full root layer until
the partial damage contract is proven by interaction filmstrips. This avoids
two failure modes observed in demos: text accumulating into crunchy overdraw
and cleared cached pixels not being replayed until a later hover.

When the gate is enabled again, the partial path is:

1. opens the root-layer pass with color load;
2. clears only the dirty region to transparent using `NVG_COPY`;
3. applies a NanoVG scissor to the dirty region;
4. replays the cached display list through a conservative culling path;
5. leaves the rest of the root layer untouched.

The culling path rejects paint ops whose known bounds do not intersect
the dirty region. When replay is inside a transform stack, the op bounds
are transformed to a conservative axis-aligned target-space rect before
testing. It also skips clipped subtrees when a `PushClip` is completely
outside the dirty region, and it skips a whole balanced transform subtree
when the transformed union of its bounded paint ops misses the dirty
region.

Text, text boxes, unknown bounds, and transform nesting that exceeds the
fixed stack used by the hot replay path keep the optimization
conservative: those ranges fall back to normal replay/culling instead of
being skipped as a group. That keeps the optimization allocation-free
and correct.

Balanced transform range bounds and clip pop indices are prepared when
the display list is recorded. A partial raster frame can therefore
reject a transformed subtree or clipped subtree with one metadata lookup
instead of scanning the subtree again. Display lists built by tests or
older callers without metadata still fall back to the conservative scan.

## Composition

Current composition has two paths:

- preferred: a direct sokol textured quad samples the root-layer color
  image and draws it into the frame target;
- fallback: the root-layer texture is drawn over the frame target as a
  full-surface NanoVG image pattern.

The direct path is the first real compositor path. It removes NanoVG
from the retained root-layer composite and turns unchanged frames into a
single textured draw. The NanoVG fallback remains because shader or
pipeline creation can fail on a backend during bring-up; correctness
falls back to the known raster path.

Current behavior:

- if the adapter says no frame is needed, nothing is rendered;
- if the root layer did not change, composition reuses it;
- if the direct compositor is valid, composition does not enter NanoVG;
- if the perf HUD is disabled, the sokol adapter commits in the same
  render call;
- if the perf HUD is enabled, AffineUI renders without commit, then
  opens a load pass to draw the native HUD, then commits.

The current compositor is enough to prove retained rendering and dirty
root-layer raster. The next compositor step is to extend the same direct
texture-quad machinery from one root layer to multiple independently
invalidated layers.

## Scheduler

The standalone sokol adapter checks whether work is needed before
rendering:

- `Ui::needs_update()`
- viewport changed
- perf HUD enabled

When the UI is static and the perf HUD is disabled, the frame callback
returns without rendering. This is the zero-work static-page path.

`Ui::needs_update()` is driven by:

- explicit UI dirty state;
- immediate-mode dirty state;
- renderer-observed active animations.

It does not scan the whole document each frame to discover animations.
The document maintains an animation-candidate count so pages with no
animations can answer quickly.

## Invalidation

The document has two levels of invalidation:

- layout invalidation;
- paint invalidation.

Pseudo-state and live attribute changes re-resolve style and compare
computed layout fields. If layout fields changed, layout is scheduled.
If only paint fields changed, the old and new visual rects become dirty
rects and layout is skipped.

Dirty rects are visual bounds, not just border boxes. They include
known shadow extents so old pixels are cleared when a shadow or focus
ring changes.

If a precise dirty rect cannot be established, the document sets the
full paint dirty flag and the renderer falls back to full raster.

## Render Statistics

`RenderStats` exists to keep the architecture honest in demos.

Important counters:

- frames seen by the renderer;
- display-list records;
- display-list replays;
- display-list changes;
- display-list unchanged records;
- display-list ops culled;
- display-list ops culled in the current partial replay;
- root-layer rasterizations;
- root-layer partial rasterizations;
- root-layer composites;
- direct root-layer composites;
- cached op count;
- dirty rect count;
- dirty area percentage;
- per-frame flags for record, display-list change, partial raster,
  direct composite, layout dirty, paint dirty, and active animation.

The perf HUD is intentionally native renderer text, not HTML, so
instrumentation does not change document layout or force document paint.

## Current Render Paths

### Static, No HUD

```text
sokol frame callback
  -> Ui::needs_update() == false
  -> viewport unchanged
  -> return without rendering
```

This is the cheapest possible path.

### Static, HUD Enabled

```text
render_frame()
  -> prepare_frame()
       -> no display-list record
       -> no root raster
  -> composite retained root layer
  -> draw native HUD
  -> commit
```

HUD mode intentionally keeps rendering so the user can see live frame
timing.

### Paint-Only Hover Change

```text
MouseMove
  -> hover chain changes
  -> restyle affected blocks
  -> computed layout fields unchanged
  -> dirty rects recorded

render_frame()
  -> record display list
  -> display list changed
  -> partial root-layer raster
       -> clear dirty union
       -> replay clipped/cullable ops
  -> composite root layer
```

### Layout Change

```text
DOM/style mutation
  -> computed layout fields changed
  -> content_size reset
  -> dirty root queued

render_frame()
  -> layout
  -> record display list
  -> full root-layer raster
  -> composite root layer
```

### Animation Today

Animations are sampled during document draw and currently keep the
document-changing path alive when they affect paint or transform.

The current scheduler avoids unnecessary animation scans for pages with
no candidates, but active animations are still conservative from the
renderer's point of view.

## Next Compositor Layer

The next major step is real compositor layers.

Layer candidates:

- explicit `will-change`;
- transform;
- opacity below 1;
- fixed position;
- scroll containers;
- active transform/opacity animations;
- expensive stable subtrees such as skeuomorphic controls.

Each compositor layer should own:

- backing texture;
- local bounds;
- dirty state;
- transform;
- opacity;
- clip;
- z/order metadata;
- content hash;
- age/usage information for eviction.

The compositor should then:

- raster dirty layers only;
- skip clean layer raster;
- update transform/opacity uniforms for composite-only animations;
- draw layers in stacking order;
- cull layers outside the viewport;
- avoid issuing any draw when no swapchain image needs updating.

This is where static pages can become zero CPU and near-zero GPU, and
where transform/opacity animation becomes uniform updates rather than
document repaint.

## Direct Quad Compositor

The root-layer composite uses sokol_gfx directly when the backend can
create the required shader, sampler, texture view, and pipeline.

It owns:

- one immutable fullscreen-quad vertex buffer;
- one pipeline per backend format/sample-count tuple;
- one sampler;
- one texture view for the retained root-layer color image.

For the root layer this reduces composition to a tiny textured draw.
For multiple layers it becomes the same draw repeated or instanced. The
future multi-layer version will add uniforms or per-instance data for
transform, opacity, UVs, and clip.

NanoVG remains the rasterizer for vector paint into layer textures.
Composition should not require NanoVG.

If the direct compositor cannot be created, AffineUI falls back to the
NanoVG image-pattern composite for that frame.

## Dirty Rects and Clip Culling

Dirty rects currently bound raster work. They should also become input
to deeper culling:

- skip display-list ops outside the dirty rect;
- skip display-list subtrees outside the dirty rect;
- skip layer raster when dirty rect misses the layer;
- skip child traversal when subtree bounds miss the dirty rect;
- skip scroll-area children outside clip rects.

The current `replay_clipped()` implementation is the first conservative
step. It handles transformed op bounds, transformed clip rejection,
prepared clip-range jumps, and prepared balanced transformed-range skips
when all paint bounds are known. The next step is to generalize that
range metadata from transforms/clips to paint chunks and eventually
layout subtrees, so whole groups can be skipped without scanning the
range first.

## Failure Modes to Avoid

These are architectural failures:

- reparsing HTML/CSS per frame;
- rebuilding the DOM for hover or animation;
- relayout for paint-only state changes;
- rerasterizing the full panel for one hover ring;
- rasterizing SVG/icons from source every frame;
- allocating during steady-state animation;
- doing CPU document work when all visible layers are clean.

Whenever a new feature is added, it should be classified by the stages
it invalidates:

- style only;
- layout;
- paint;
- layer raster;
- composite state;
- no render work.

Features that cannot state their invalidation level usually become
performance bugs.

## Practical Debugging

Use the perf HUD when validating renderer behavior.

Healthy signs:

- static pages stop rendering when the HUD is off;
- static pages with HUD show composites without root raster;
- hover over paint-only controls increments dirty rects without layout
  dirty count; partial raster count should remain zero while the
  correctness gate is closed;
- dragging a composite-only animation should eventually show no display
  list records and no root raster;
- culled op count should rise for small dirty rects on large panels only
  when partial replay culling is explicitly enabled for validation.

Suspicious signs:

- layout dirty during simple hover color changes;
- full paint dirty during bounded widget state changes;
- root raster count rising while nothing visual changes;
- display-list records rising on static pages;
- dirty rect count always zero during interactions;
- active animation flag staying true after finite animation end.

## Compatibility Path

`Renderer::render(Document&, width, height, dpi)` is the compatibility
path for callers that already opened a sokol pass. It uses the same
display-list preparation but replays directly into the caller's pass.

It cannot use retained root-layer composition because it does not own
the pass. New adapters and examples should prefer `render_to()` /
`Ui::render(FrameTarget)` / `affineui::sokol::render_frame()` when they
want retained rendering.
