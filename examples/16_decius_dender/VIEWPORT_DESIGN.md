# DENDER Phase B — Real vector 3D viewport (design spec)

Goal: replace the CSS-transform placeholder in examples/16_decius_dender with a real, interactive, software-projected 3D viewport that mirrors the web version's three.js viewport (see dender_web_reference.md §6), rendered as inline SVG.

## Mechanism (proven in-tree — mirror it)
`extras/skeuo/affineui_skeuo.cpp` PatchBay::cables_layer (line ~602) is the pattern:
- The engine renders inline `<svg>` natively. An svg is NOT a block — it paints as part of its parent block, so the overlay needs a positioned, z-indexed wrapper div; the svg then fills the wrapper rect.
- Declare with `v.element("div", ...)` wrapper + `v.element("svg", ...)` + `v.html(<svg markup>, key)` children. Regenerate markup on each view rebuild (reload()).
- Geometry: `app.document().find_element_rect(name)` for the canvas-host rect (may be empty before first layout — set a needs-layout flag and re-render from a frame tick, as PatchBay does).
- Interaction: `App::on_event` (with hover chain) for pointer drags over the canvas host; `App::on_frame(dt)` for damping/retry ticks; `request_render = reload` callback.

## Module
New `dender_viewport.{h,cpp}` — class `DenderViewport` (PatchBay-shaped):
- Declaration side: `canvas(View&)` opens the canvas-host (keeps the CSS radial-gradient background from styles), emits the scene SVG layer, then the existing DOM overlays (stats, corner, tool rail, nav cluster, N-panel) stack above it.
- Runtime side: `attach(App&)`, `request_render`, reads/writes the Phase-A `DenderDocument` scene model (objects, selection, transform mode).

## Camera (match web viewport.js)
Orbit model: target (0, 0.6, 0); initial eye (5.2, 3.4, 6.4) → derive yaw/pitch/dist; perspective fov 38°, near 0.1; minDist 1.8, maxDist 50, maxPolar π·0.495; rotateSpeed 0.85, panSpeed 0.8, zoomSpeed 0.9. Y-up.
- LMB drag = orbit; RMB drag = pan (target moves in view plane); wheel = dolly (×1.1 / ÷1.1 per notch toward view dir); "Move View" nav button toggles LMB between orbit and pan (aria-pressed).
- `frame_selected()`: fit selection (or whole scene) — dist = max(2, size·1.4). Bound to F key + "Camera View" nav button.
- Nav gizmo (72×72 SVG in the nav cluster): 6 axis nubs (+X/+Y/+Z filled + letter, −X/−Y/−Z hollow), rotated by inverse camera orientation, depth-sorted, back-facing dimmed (opacity .55 at depth < −0.05); click snaps camera along that axis (keep dist). Regenerate with the viewport SVG.

## Scene rendering (SVG, painter's algorithm)
- Mesh tables (compile-time constants, keep tessellation SVG-friendly): Cube 1.6³ (12 tris); UV Sphere r .9 (16×12); Icosphere r .9 subdiv 1 (80 tris); Cylinder r .7 h 1.6 (16 seg); Cone r .85 h 1.6 (16); Torus R .8 r .25 (12×20, rotated flat); Plane 2×2. Spawn y-offsets per web (§6 primitive library).
- Flat shading: base color 0x9aa1ad, roughness-ish modulation by key light dir normalize(5,7,4) (diffuse ~1.0 weight) + hemisphere ambient (sky 0xe6efff·0.25 up / ground 0x1a1c20 down); clamp; backface-cull; sort faces by view-space centroid depth across ALL objects; emit `<polygon>` per face (fill only, tiny 0.25px stroke same color to hide seams).
- Wireframe shading mode: stroke edges 0x9aa1ad, no fills. Texture/Render modes = same as solid (web is cosmetic there too).
- Non-mesh objects as stroked polylines (`<polyline>/<path>`, screen-constant ~1.2px): Point Light = ico wire + halo circle (#ffd060); Sun = 8 spokes + circle (#ffe2b5-ish per web); Spot = cone edge lines; Camera = wire box + frustum lines (#b6bcc7); Empty = 3 axis segments (#a0a4ad).
- Ground grid: 20×20 GridHelper lines, center lines 0x40444c, others 0x2a2d34, opacity .55 (single `<path>` batches for perf). No shadows (skip the shadow catcher).
- World-axes gnomon: 3 arrows (X #d8475a, Y #6fb74a, Z #3f7ad0) drawn LAST (render-order on top), origin-anchored, length ~1.06 + head.
- Selection box: object-oriented AABB (union child geo bounds in object space, transform by object matrix) drawn as 12 edges #e8943c on top, only when something selected.
- Multi-select: active object gets the box; extra multiSel members get a dimmer outline (web ties gizmo to active only).

## Picking
On click (not drag: moved ≤4px, held ≤600ms, not within 250ms post-drag suppression): test screen point against projected faces (front-to-back) of each object incl. gizmo polylines (use a few px tolerance for lines); hit → document select (shift = toggle multi); miss → deselect (unless shift). Same discrimination rules as web §6.

## Transform gizmo (stretch — only after the rest lands)
Translate-mode axis arrows at the active object origin (3 screen-projected axis handles); drag moves along the axis via closest-point-on-axis math; rotate/scale modes can fall back to the Inspector fields initially. If skipped, keep G/R/S setting the mode state + rail sync only, as Phase A.

## Live overlays sync
- Stats overlay line 2 "(1) Collection | <ActiveName>"; corner overlay + statusbar "Verts N | Faces N | Tris N | Objects sel/total" computed from mesh tables of scene contents (web shows the Cube's 8/6/12 initially — compute for the active mesh, fall back to scene totals like web's static text where ambiguous).

## Perf rules
- Rebuild the SVG string only when camera or scene is dirty; reuse cached string otherwise (reload() calls build every time — memoize inside DenderViewport).
- Keep total polygon count modest (< ~2500 faces with default scene); tessellation constants above chosen for that.
- Pointer-drag orbits call request_render per move (PatchBay precedent); if that visibly lags, throttle to on_frame cadence.

## Verify
Build decius_dender clean; then hand to the USER for windowed testing (orbit/pan/dolly feel, click-pick, add/delete objects reflected, selection box, nav gizmo snaps, wireframe toggle). Do not claim done before that (verify-in-window rule).
