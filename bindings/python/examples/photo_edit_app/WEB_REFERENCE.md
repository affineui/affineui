# Decius Photo — Complete Feature Inventory (web reference decode)

Photoshop-clone demo on decius.css. App namespace `window.PS`. Source: c:\Users\benjcooley\projects\decius-css\samples\decius-photo\ (index.html, ui.js, engine.js, tools.js, paint.js, panels.js, dialogs.js, tweaks.jsx, tweaks-panel.jsx, app.css).

---

## 1. WINDOW / LAYOUT STRUCTURE

### Root shell
- `<body class="dcs" data-dcs-density="comfortable" data-dcs-accent="blue">`
- `<div class="ps-app" data-screen-label="Decius Photo">` — CSS grid, `grid-template-rows: auto auto 1fr auto` (menubar / options bar / body / statusbar), `height:100vh; width:100vw`.

### Four top-level regions (top→bottom)
1. **Menubar** — `<div class="dcs-menubar ps-menubar">`
2. **Options bar** — `<div class="ps-options" id="ps-options">`
3. **Body** — `<div class="ps-body" id="ps-body" data-dcs-float-host>` (`position:relative; overflow:hidden`) — float-host for all floating panels
4. **Statusbar** — `<div class="dcs-statusbar ps-statusbar">`

### Menubar contents (left→right)
- Brand patch `.ps-brand`: `<i class="di di-decius ps-brand__mark">` + `<span class="ps-brand__name">Decius&nbsp;Photo</span>` (dark `#0d0f14` patch, accent mark)
- 9 menu items `.dcs-menubar__item` each `data-dcs-toggle="menu" data-dcs-target="#m-..."`: **File, Edit, Image, Layer, Select, Filter, View, Window, Help**
- `.dcs-menubar__spacer`
- `.dcs-menubar__meta.ps-doc-name` → `<span id="ps-titlebar">Untitled-1 @ 67% (RGB/8)</span>` (mono, muted)
- `<span class="dcs-divider dcs-divider--v">`
- Settings cog `.dcs-btn.dcs-btn--icon.dcs-btn--ghost.ps-settings` `data-dcs-toggle="popover" data-dcs-target="#ps-tweaks" data-dcs-placement="bottom-end" data-dcs-tip="Theme tweaks"`, icon `di-cog`

### Options bar contents
- `<span class="ps-tool-glyph" id="ps-opt-glyph">` (26×26, accent glyph in `--dcs-well`)
- `<span class="ps-tool-name" id="ps-opt-name">Brush Tool</span>`
- `.dcs-divider--v`
- `<div class="ps-grow" id="ps-opt-slot">` — per-tool options injected here
- `.dcs-divider--v`
- `<button class="dcs-btn dcs-btn--ghost dcs-btn--sm" id="ps-btn-reset-view">` → `di-fit` + "Fit"
- Height `var(--dcs-h-xl)`, background `--dcs-surface-1`, bottom border.

### Body / dock arrangement
- `<div class="dcs-dock ps-doc-dock" id="ps-doc-dock">` with a single center document dockpane:
  - `<div class="dcs-dockpane dcs-dockpane--center" data-dcs-dock-kind="documents" data-dcs-dock-tearoff="false" data-dcs-tab-menu="false">`
  - `.dcs-dockpane__tabbar` → `.dcs-dockpane__tabs#ps-doc-tabs` with one tab (`data-dcs-target="#ps-doc-untitled1" aria-selected="true"`): `di-image` + "Untitled-1 @ 67% (RGB/8)" + close `.dcs-dockpane__tab-close` (×)
- **Stage anatomy** inside tabpanel: `.ps-stagewrap` (grid `18px 1fr / 18px 1fr`, background `--dcs-stage`):
  - `.ps-ruler-corner` (18×18); `.ps-ruler--h` → `<canvas id="ps-ruler-h">`; `.ps-ruler--v` → `<canvas id="ps-ruler-v">`
  - `.ps-stage#ps-stage` → `.ps-doc#ps-doc` (layer canvases + composite) → `.ps-marquee.ps-hidden#ps-marquee`
  - Overlays: bottom-left badge `#ps-ov-cursor` ("0, 0 px"); top-right badge `#ps-ov-doc` ("1280 × 800")

### Floating surfaces (children of `#ps-body`, absolute)
- **Tools toolbar** `.dcs-toolbar--v.dcs-toolbar--floating.ps-toolstrip#ps-toolstrip` at `left:12px;top:12px`. Grip `data-dcs-drag-handle`, `#ps-tools` host, `.ps-toolsep`, color chips block, quick-mask button.
- **Navigator** `.dcs-panel--floating.ps-navigator#ps-navigator` at `right:12px;top:12px;width:214px`.
- **Color / Swatches** `#panel-color` at `right:12px;top:226px;width:268px` (tabbed dockpane).
- **Layers / Channels / Paths / Comps** `#panel-layers` at `right:12px;top:486px;bottom:12px;width:300px` (tabbed dockpane).
- **Adjustments** `#panel-adjust` at `right:324px;top:12px;width:236px`.
- **History** `#panel-history` at `right:324px;top:152px;bottom:12px;width:236px`.
- **Quick floating toolbar** `.ps-floatbar#ps-floatbar` at `left:50%;bottom:18px` (undo/redo/zoom).

### Statusbar (left→right)
- `<input class="dcs-input ps-zoom-field" id="ps-zoom-field" value="67%">` (62px, centered)
- sep · "Doc: `<span id="ps-st-doc">3.91M / 11.7M</span>`" · sep · `<span id="ps-st-tip">` per-tool tip · spacer · `--ok` item `di-check-circle` + "Ready" · sep · "RGB/8 · 72 ppi"

### Other containers
`<div id="ps-menus">` (menus built here), `<div id="ps-modals">` (dialogs), `#ps-color-picker` popover, `#ps-tweaks` popover, `#ps-tweaks-root` (React tweaks).

---

## 2. TOOL PALETTE

`PS.TOOLS` rendered by `buildTools()` into `#ps-tools` (2-column grid, 34×34 cells). Each `.ps-tool[data-tool=id]`, `aria-pressed`, `title="Name  (Key)"`. `group:1` → corner triangle (`data-group="1"::after`). `sep:true` inserts `.ps-toolsep`. Icon = `di-<icon>` or raw inline SVG (`PS.SVG`: `type` T-glyph, `shape` rounded rect, `gradient` gradient rect).

| # | id | Name | Key | Icon | group | paint | cursor | Status tip |
|---|-----|------|-----|------|-------|-------|--------|------------|
| 1 | move | Move Tool | V | move | — | — | move | Move: drag to reposition the active layer. |
| 2 | marquee | Rectangular Marquee | M | marquee | ✓ | — | crosshair | Marquee: drag to make a rectangular selection. |
| 3 | lasso | Lasso Tool | L | lasso | ✓ | — | crosshair | Lasso: drag to draw a freehand selection. |
| 4 | wand | Object Selection | W | select | ✓ | — | crosshair | Magic Wand: click to select a similar-color region. |
| — sep |
| 5 | crop | Crop Tool | C | clip | — | — | crosshair | Crop: drag to trim the document to a region. |
| 6 | eyedropper | Eyedropper | I | eyedropper | — | — | crosshair | Eyedropper: click to sample a color as the foreground. |
| — sep |
| 7 | brush | Brush Tool | B | brush | ✓ | ✓ | crosshair | Brush: drag on the canvas to paint with the foreground color. |
| 8 | pencil | Pencil Tool | N | pencil | — | ✓ | crosshair | Pencil: hard-edged freehand strokes. |
| 9 | clone | Clone Stamp | S | stamp | ✓ | ✓ | crosshair | Clone Stamp: Alt-click to set a source, then paint to copy pixels. |
| 10 | history | History Brush | Y | history-brush | — | ✓ | crosshair | History Brush: paint to restore from the history source state. |
| 11 | eraser | Eraser Tool | E | eraser | ✓ | ✓ | crosshair | Eraser: drag to erase pixels on the active layer. |
| 12 | fill | Paint Bucket | G | fill | ✓ | — | crosshair | Paint Bucket: click to flood-fill a region with the foreground color. |
| 13 | gradient | Gradient Tool | G | svg:gradient | — | — | crosshair | Gradient: drag to draw a foreground→transparent gradient. |
| — sep |
| 14 | dodge | Dodge Tool | O | dodge | ✓ | ✓ | crosshair | Dodge: paint to lighten pixels. |
| 15 | burn | Burn Tool | O | burn | — | ✓ | crosshair | Burn: paint to darken pixels. |
| 16 | smudge | Smudge Tool | R | smudge | — | ✓ | crosshair | Smudge: drag to push pixels around. |
| 17 | blur | Blur Tool | R | blur | — | ✓ | crosshair | Blur: paint to soften detail. |
| — sep |
| 18 | pen | Pen Tool | P | pen | — | — | crosshair | Pen: click to add anchor points and build a path. |
| 19 | type | Horizontal Type | T | svg:type | — | — | text | Type: click to add a text layer. |
| 20 | shape | Rectangle Tool | U | svg:shape | — | — | crosshair | Shape: drag to draw a filled rectangle on a new layer. |
| — sep |
| 21 | hand | Hand Tool | H | pan | — | — | grab | Hand: drag to pan the document. |
| 22 | zoom | Zoom Tool | Z | zoom-in | — | — | zoom-in | Zoom: click to zoom in, Alt-click to zoom out. |

**Default tool at boot: brush** (`boot()` calls `PS.selectTool('brush')`).

### Color chips block (below tools)
`.ps-colorchips` (38×38 overlap): `.ps-reset#ps-color-reset` (⬚, "Default colors (D)"), `.ps-swap#ps-color-swap` (⇄, "Swap colors (X)"), `.ps-colorchip--bg#ps-chip-bg` (white, opens picker popover placement="right"), `.ps-colorchip--fg#ps-chip-fg` (black, opens picker). Then `.ps-tool#ps-quickmask` — "Edit in Quick Mask Mode (Q)", icon `di-layer-mask` (visual only).

### Per-tool OPTIONS BAR (`PS.OPTIONS[id]`)
Helpers: `sld(id,min,max,val,w=120)` slider; `num(id,val,w=58)` number input; `opt(label,inner)` labeled group; vsep divider.
`modeSelect` = blend `<select id="ps-opt-mode">`: Normal(source-over), Multiply, Screen, Overlay, Darken, Lighten, Dodge(color-dodge), Burn(color-burn), Difference.

| Tool | Controls |
|------|----------|
| brush | Size slider [1–400, **24**] + number `#ps-o-size-n`; Hardness `#ps-o-hard` [0–100, **70**]; Opacity `#ps-o-op` [1–100, **100**]; Flow `#ps-o-flow` [1–100, **100**]; Mode select |
| pencil | Size [1–200, **4**]+num; Opacity [1–100, **100**]; Mode |
| eraser | Size [1–400, **30**]+num; Hardness [0–100, **50**]; Opacity [1–100, **100**] |
| clone | Size [1–400, **40**]+num; Hardness [0–100, **60**]; Opacity [1–100, **100**]; badge `#ps-clone-state` "Alt-click to set source" |
| history | Size [1–400, **40**]+num; Opacity [1–100, **100**] |
| dodge | Size [1–400, **60**]+num; Exposure `#ps-o-op` [1–100, **40**] |
| burn | Size [1–400, **60**]+num; Exposure [1–100, **40**] |
| smudge | Size [1–300, **30**]+num; Strength `#ps-o-op` [1–100, **50**] |
| blur | Size [1–300, **40**]+num; Strength [1–100, **50**] |
| fill | Tolerance `#ps-o-tol` [0–128, **32**]; Opacity [1–100, **100**]; checkbox `#ps-o-contig` "Contiguous" (checked) |
| gradient | Opacity [1–100, **100**]; badge "Foreground → Transparent" |
| marquee | Button-group `#ps-marquee-mode`: new (`di-marquee`, pressed) / add (`di-plus`) / sub (`di-minus`); Feather num `#ps-o-feather` (0) + "px" |
| lasso | Feather num (0)+"px"; checkbox `#ps-o-aa` "Anti-alias" (checked) |
| wand | Tolerance [0–128, **32**]; checkbox "Contiguous" (checked) |
| move | checkbox `#ps-o-autosel` "Auto-Select" (checked); badge "Drag to move layer pixels" |
| crop | Ratio select: Free / 1:1 / 4:3 / 16:9; button `#ps-crop-apply` (`di-check` "Apply", primary) |
| type | Font select `#ps-type-font`: IBM Plex Sans / JetBrains Mono / Georgia; Size num `#ps-type-size` (64); Text input `#ps-type-text` value "Decius" |
| shape | Fill colorfield chip `#ps-shape-chip`; Radius num `#ps-shape-radius` (8) |
| pen | badge "Click to place anchor points · demo path preview" |
| zoom | button-group `#ps-zoom-in`/`#ps-zoom-out`; button `#ps-zoom-100` "100%"; button `#ps-zoom-fit` "Fit Screen" |
| hand | badge "Drag the document to pan · scroll to pan · Space + drag anytime" |

Options wiring: size slider ↔ number stay in sync both ways.

---

## 3. MENUS

Built by `buildMenus()` from `PS.MENUS` into `#ps-menus`; each `.dcs-menu#m-...` fires `dcs:select` → `PS.menuAction(value)`. Checked items class `dcs-menu__item--checked`. Menu labels sentence-case (app.css override). No enable/disable logic — all items always active. menuAction fallback: unhandled value → `PS.toast(v, 'info')`.

**File (m-file)**: New… ⌘N `di-file` → dlgNewDoc · Open Sample… ⌘O `di-folder-open` → regen sample scene, clear history, fit, toast · Place Embedded… `di-import` → dlgPlace · sep · Save ⌘S `di-save` → toast "Saved Untitled-1.psd" · Export As… ⇧⌘E `di-export` → dlgExportAs · sep · Close ⌘W `di-close` (toast).

**Edit (m-edit)**: Undo ⌘Z `di-undo` → PS.undo · Redo ⇧⌘Z `di-redo` → PS.redo · sep · Cut ⌘X `di-cut` · Copy ⌘C `di-copy` · Paste ⌘V `di-paste` (all toast) · sep · Fill with Foreground ⌥⌫ `di-fill` → dlgFill · Stroke… `di-edit` → dlgStroke · Free Transform ⌘T `di-scale-corners` → toast.

**Image (m-image)**: Brightness/Contrast… `di-light` → adjust('bc') · Hue/Saturation… ⌘U `di-color-grade` → adjust('hsl') · Levels… ⌘L `di-graph` → adjust('levels') · Invert ⌘I `di-mirror` → adjust('invert') · Desaturate ⇧⌘U `di-mono` → adjust('desat') · sep · Image Size… ⌥⌘I `di-aspect` → dlgImageSize · Canvas Size… ⌥⌘C `di-fit` → dlgCanvasSize · Flatten Image `di-layers` → flatten.

**Layer (m-layer)**: New Layer ⇧⌘N `di-plus` → dlgNewLayer · Duplicate Layer ⌘J `di-duplicate` → duplicateLayer · Delete Layer `di-trash` → deleteLayer · sep · Group Layers ⌘G `di-folder` → toast · Add Layer Mask `di-layer-mask` → toast · sep · Bring Forward ⌘] `di-chevron-up` → moveActive(1) · Send Backward ⌘[ `di-chevron-down` → moveActive(-1) · Merge Down ⌘E `di-compress` → mergeDown.

**Select (m-select)**: All ⌘A `di-marquee` → select whole doc · Deselect ⌘D `di-close` → clear · Inverse ⇧⌘I `di-mirror` → toast · sep · Feather… ⇧F6 `di-blur` → dlgFeather · Grow `di-plus` (toast).

**Filter (m-filter)**: Gaussian Blur… `di-blur` → filter('fblur') · Sharpen `di-sharpen` → filter('fsharp') · Add Noise… `di-wave-noise` → filter('fnoise') · Pixelate `di-grid` → filter('fpix') · sep · Emboss `di-extrude` → filter('femboss') · Find Edges `di-cross-target` → filter('ffind').

**View (m-view)**: Zoom In ⌘+ `di-zoom-in` → z×1.4 · Zoom Out ⌘- `di-zoom-out` → z/1.4 · Fit on Screen ⌘0 `di-fit` → fitToScreen · 100% ⌘1 `di-cross-target` → setZoom(1) · sep · Rulers ⌘R `di-aspect` **checked** · Show Grid ⌘' `di-grid` · Snap `di-magnet` **checked**.

**Window (m-window)**: Layers F7 `di-layers` **checked** · Color F6 `di-palette` **checked** · History `di-history-brush` · Adjustments `di-color-grade` · sep · Reset Workspace `di-grid`.

**Help (m-help)**: About Decius PhotoEditor `di-info` → aboutModal() · About decius.css `di-decius` → aboutModal(true) · Keyboard Shortcuts `di-keys` → dlgShortcuts.

---

## 4. PANELS

### Navigator (`#ps-navigator`)
- Header (drag-handle): `di-globe` "Navigator".
- Body: `.ps-nav-thumb#ps-nav-thumb` (116px tall, checkerboard, cursor:move) with `<canvas id="ps-nav-canvas">` + `.ps-nav-view#ps-nav-view` (red viewport rect, `--dcs-danger` border).
- Row: `di-zoom-out` + zoom slider `#ps-nav-zoom` (min 5, max 800, value 80) + `di-zoom-in` + `#ps-nav-pct` "80%".
- Behavior: click/drag thumb pans doc (`centerOnDoc`); slider → `setZoom(v/100)`; rAF-debounced live repaint on render/applyView; draws scaled composite + visible-region rect.

### Color / Swatches (`#panel-color`, tabbed dockpane)
Tabs: **Color** (`di-palette`, active) / **Swatches** (`di-grid`).
- Color tab `#p-color` → `.ps-colorpanel`: `.dcs-color-square.ps-sv#ps-sv` (SV square, 120px, `--hue` var) + dot `#ps-sv-dot`; `.dcs-hue-bar.ps-hue#ps-hue` (14px) + dot; Hex input `#ps-hex` (mono, `#000000`); RGB row: R `#ps-r` / G `#ps-g` / B `#ps-b` (0).
- Swatches tab `#p-swatches`: `.ps-swatches#ps-swatches` — 10-col grid of 30 chips from `PS.SWATCHES`. Left-click → setForeground; right-click → setBackground.

### Layers / Channels / Paths / Comps (`#panel-layers`, tabbed dockpane)
Tabs: **Layers** (`di-layers`, active) / **Channels** (`di-eq`) / **Paths** (`di-spline`) / **Comps** (`di-filmstrip`).

**Layers tab (`#p-layers`)**:
1. Filter bar: `<select id="ps-layer-kind">` Kind/Name/Effect/Mode/Attribute/Color (62px); icon buttons: `di-image`, `di-color-grade`, "T" cap span, `di-poly`, `di-cube`; spacer; filter toggle `#ps-filter-toggle` (`di-bolt`).
2. Blend + Opacity row: Blend `<select id="ps-blend">` — **15 options**: Normal(source-over), Multiply, Screen, Overlay, Darken, Lighten, Color Dodge, Color Burn, Hard Light, Soft Light, Difference, Hue, Saturation, Color, Luminosity. "Opacity:" + `.ps-amt#ps-op-amt[data-amt="opacity"]` "100%" (drag-to-scrub).
3. Lock + Fill row: "Lock:" + 4 icon buttons `data-lock`: transparency (`di-grid`), image (`di-brush`), position (`di-move`), all (`di-lock`), each aria-pressed. spacer, "Fill:" + `.ps-amt#ps-fill-amt` "100%" (scrub).
4. Layer list `#ps-layer-list` — `PS.refreshLayers()`, top layer first.
5. Footer: Link `#ps-l-link` (`di-link`) toast · fx `#ps-l-fx` (italic Georgia "fx") toast · Mask `#ps-l-mask` (`di-layer-mask`) toast · Adjust `#ps-l-adjust` (`di-color-grade`) → adjust('hsl') · Group `#ps-l-group` (`di-folder`) toast · spacer · New `#ps-l-new` (`di-plus`) · Duplicate `#ps-l-dup` (`di-duplicate`) · Delete `#ps-l-del` (`di-trash`).

**Layer ROW anatomy** (`.ps-layer[data-idx]`, draggable): `.ps-layer-eye` (+`.is-off`): `di-eye`/`di-eye-off`, click toggles visible + snapshot · `.ps-layer-thumb` 34×34 canvas (checkerboard, live-updated while painting) · `.ps-layer-name` (dbl-click → inline input rename, Enter/Escape) · `.ps-layer-lock` shows `di-lock` if locked · row click → active (`.is-active` accent-dim + inset accent bar) · drag-and-drop reorder with `.is-drop-above/below` indicator lines, splices array + snapshot "Reorder Layers".

**Channels tab**: list rows RGB(⌘2, selected), Red(⌘3), Green(⌘4), Blue(⌘5), icon `di-eq`.
**Paths tab**: static note "No paths. Use the Pen tool to create a work path."
**Comps tab**: static rows "Last Document State" (`di-cross-target`, active), "Hero — sunrise" (`di-filmstrip`, meta "V P A"), "Hero — dusk" ("V P A"), "No title text" ("V · ·"); footer Apply (`di-check`), Update (`di-redo`), spacer, New (`di-plus`), Delete (`di-trash`).

### Adjustments (`#panel-adjust`)
Header `di-color-grade` "Adjustments". Body: 6-col grid `#ps-adjust-grid`, 12 icon buttons (icon — label — action): light—Brightness/Contrast—bc · color-grade—Hue/Saturation—hsl · graph—Levels—levels · curve—Curves—curves(fallback) · mono—Black & White—desat · palette—Color Balance—balance(fallback) · mirror—Invert—invert · eq—Channel Mixer—mixer(fallback) · droplet—Photo Filter—photo(fallback) · wave-sine—Vibrance—vibrance · filter-lp—Threshold—threshold · gain—Exposure—exposure(fallback).

### History (`#panel-history`)
Header `di-history-brush` "History". Body `#ps-history-list`: each `.ps-history-item` = `di-<icon>` + name; `.is-current` (current) / `.is-future` (dimmed .4). Click → `PS.jumpTo(i)`. Auto-scrolls to bottom.

---

## 5. ENGINE / DOCUMENT MODEL (engine.js)

### Document
`PS.doc = { w:1280, h:800, layers:[], active:0, sel:null }`; `PS.view = { z:0.67, px:0, py:0, min:0.05, max:16 }`; `PS.fg='#1f6feb'`, `PS.bg='#ffffff'`; `PS.hsv={h,s,v}`.

### Layer model (`PS.mkLayer(name, opts)`)
`{ id, name, canvas (full-doc), ctx, thumb (34×34), thumbCtx, visible(true), opacity(1), fill(1), blend('source-over'), locked(bool), kind('pixel'|'text') }`. Each layer owns a full-size canvas. `PS.active()` = layers[active].

### Compositing (`PS.render`)
Composite target `PS.viewCanvas` (`id="ps-view" class="ps-composite"`). For each layer bottom→top: skip if !visible or opacity≤0; `globalAlpha = opacity*fill`, `globalCompositeOperation = blend`, drawImage. Thumbs: letterbox-fit into 34×34.

### View transform (`applyView`)
CSS props on `#ps-doc`: `--z`,`--px`,`--py`. `.is-pixelated` when z≥4. `docOrigin()`, `screenToDoc()`, `stageRect()`. `setZoom(z, cx?, cy?)` clamps [0.05,16], zooms toward cursor. `fitToScreen()` 48px margin. Menu/key zoom factor ×1.4.

### Rulers
Two canvases, DPR-aware. Colors `--dcs-text-mute`/`--dcs-line-soft`. Tick step from [1,2,5,10,20,25,50,100,200,250,500,1000] (first with t*z ≥ 48). Mono 8px labels; vertical rotated -90°.

### Selection
`doc.sel = {x,y,w,h}` int rect or null. Rect only. `setSelection` rounds, needs w>0&h>0. `updateMarquee()` positions `.ps-marquee` (marching ants CSS). `clipToSel(ctx,fn)` clips ops to selection. `PS.selFeather` stored but never applied.

### History (`PS.history`)
`{ list:[], index:-1, max:40, sourceIndex:0 }`. `snapshot(name, icon)` pushes `{name, icon, active, layers:[{id,name,visible,opacity,fill,blend,locked,kind, data:canvas.toDataURL()}]}` — FULL pixel snapshot per step; truncates redo tail; caps 40. `restoreState(rec)` rebuilds layers from dataURLs (async). `jumpTo(i)`, `undo()`, `redo()`. `historySourceFor(layer)` = layer image at sourceIndex (History Brush source; sourceIndex=0 after boot "Open" snapshot).

### Layer ops
`addLayer(name,opts,atTop)` inserts at top, active. `deleteLayer()` guarded (never last), snapshot. `duplicateLayer()` copies canvas, "name copy", above. `moveActive(dir)` swap+snapshot. `mergeDown()` composites active onto below respecting opacity/fill/blend. `flatten()` → single white "Background" layer with composite.

### Sample scene (`generateSampleScene`) — 7 layers bottom→top
1. **Background** (locked) — vertical gradient sky #0b1437→#1d2b66→#6a4a86→#d98a5a→#f2c277 + 90 random stars top half
2. **Sun** (blend screen) — radial glow at (W·0.5, H·0.62) + sun disc r=70
3. **Mountains · Far** (opacity 0.85) — ridge at H·0.66, #34305a
4. **Mountains · Near** — ridge at H·0.76, #1a1730
5. **Water** (blend overlay, opacity 0.55) — reflection band bottom 18%
6. **"DECIUS"** (kind text) — "DECIUS" 700 120px IBM Plex Sans + "a decius.css showcase" 500 30px JetBrains Mono
7. **Paint** — empty top layer (active)

---

## 6. TOOLS IMPLEMENTATION (paint.js)

### Pointer routing
`initPointer()`: stage pointerdown → onDown; window pointermove/up. Stage wheel: ctrl/cmd → zoom (×1.12/÷1.12 toward cursor), else pan. onDown ignores middle button; Space or hand tool → pan drag; pointer capture.

### Paint family (brush/pencil/eraser/clone/history/dodge/burn)
- `beginStroke`: radius = size/2 (min 0.5); hardness (pencil forced 1, default 70%); opacity; flow (default 100%). Snapshots base canvas + fresh buffer.
- Spacing: `sp = max(0.6, r * (pencil ? 0.5 : 0.16))`; stamps interpolated along segment.
- `stamp()`: hardness ≥ .985 → solid circle; else radial gradient color→(inner stop r*hardness)→transparent at r. Flow alpha.
- `commitLive()`: layer = base + buffer, clipped to selection. Composite: eraser destination-out; dodge screen; burn multiply; else source-over. Stroke alpha = opacity.
- Colors: brush/pencil fg; eraser (dest-out); dodge white; burn black.
- Clone: needs `PS.cloneSource` (Alt-click sets {x,y,layer}; badge "Source set ✓"); circle-clip draws source layer offset by dx/dy.
- History Brush: source = historySourceFor image; circle-clip draw.
- `endStroke()`: snapshot with tool name/icon.

### Smudge / Blur (`directSmear`, unbuffered)
Strength = opt/100. Smudge: circle-clip, alpha strength*0.5, draw own layer offset (-dx*0.6,-dy*0.6). Blur: alpha strength*0.6, `ctx.filter = blur(max(1,r/6)px)`, redraw base.

### Fill / Wand (`floodMask`)
Seed color match = squared RGBA dist ≤ tol²·4. Contiguous = 4-connected stack flood; else whole-image scan. Fill: mask → fg-colored canvas at opacity, clipped to sel; snapshot "Paint Bucket". Wand: floods composite, bbox → rectangular selection, toast "Selection: W × H px".

### Others
- **Eyedropper**: 1px from composite → hex → setForeground.
- **Gradient**: linear a→b, fg opaque → fg transparent, full-doc at opacity, clipped; snapshot.
- **Move**: snapshot base on down; drag redraws layer offset; snapshot "Move Layer".
- **Marquee/Lasso/Crop**: drag rect (lasso = bbox of freehand pts); live `previewRect`; up: crop → applyCrop if w>4; else setSelection.
- **applyCrop**: crops every layer canvas (min 4×4), resizes doc, clears sel, re-fits; snapshot "Crop".
- **Type**: new layer "T  <txt>" (kind text), fills text at click in fg, `600 <size>px "<font>"`; snapshot.
- **Shape**: rounded rect (radius opt, default 8) in fg on active layer; snapshot "Rectangle".
- **Pen**: clicks add anchors to `PS.penPts`; overlay polyline #4d9fff + square handles; cleared on tool switch. Preview only.
- **Zoom**: click ×1.4, Alt ÷1.4 toward cursor. **Hand**: drag pan; Space+drag anytime.

---

## 7. DIALOGS (dialogs.js)

Generic `PS.dialog({title,icon,width,fields,okLabel,onOk,onField})` → `.dcs-modal-backdrop#ps-dlg`, header (icon+title+close), body `.ps-dlg-body`, footer (Cancel + primary OK). Field types: section, static, text, number, select, slider, check, color, custom. Auto-focus first field.
`blendOptions()` = 13 modes (Normal, Multiply, Screen, Overlay, Darken, Lighten, Color Dodge, Color Burn, Difference, Hue, Saturation, Color, Luminosity).

- **New Document** (file, 420, "Create"): Name "Untitled-1"; Preset select Custom/Web 1920×1080/Square 2048/Postcard 1748×1240/Phone 1170×2532 (updates W/H); section "Size & resolution"; Width 1280; Height 800; Resolution 72; Color Mode RGB/8|RGB/16|Grayscale|CMYK/8; Background White|Black|Transparent|Foreground. onOk: fresh single Background layer (filled unless Transparent), reset history, titlebar "<name> @ 100% (<mode>)", fit, toast.
- **Image Size** (aspect, 400, "Resize"): static current "W × Hpx · <MB>M"; section Dimensions; W/H/Resolution; check Constrain (true); check Resample (true). onField: aspect-locked W↔H. onOk: resizeDoc(w,h,'scale') scales pixels; snapshot.
- **Canvas Size** (fit, 400, "Resize"): static current; W/H; custom 3×3 anchor grid (tl…br, mc default). onOk: resizeDoc(w,h,'canvas',anchor) — no scaling, anchor placement.
- **New Layer** (plus, 380, "Create"): Name "Layer <n>"; Mode blend select; Opacity slider [0–100, 100]. onOk: addLayer with blend+opacity.
- **Fill** (fill, 380, "Fill"): Contents Foreground|Background|Black|White|50% Gray; section Blending; Mode; Opacity slider. onOk: fill active layer (clipped) with mapped color at opacity+blend.
- **Stroke** (edit, 380, "Stroke"): Width px 4; Color (fg); Location Inside|Center|Outside (center); Opacity. onOk: strokes selection rect (or 1px-inset full doc).
- **Feather** (blur, 340, "OK"): Radius px 8. onOk: stores PS.selFeather, toast (not applied).
- **Export As** (export, 400, "Export"): Format PNG|JPG|WEBP; Quality slider [10–100, 92]; Scale 1×|2×|0.5×; static "Exports a flattened image of the document." onOk: composite → scaled canvas → white bg for non-PNG → download `decius-photoeditor.<ext>`.
- **Keyboard Shortcuts** (keys, 460, "Done"): 2-col kbd list — V Move, M Marquee, L Lasso, B Brush, E Eraser, G Bucket, S Clone, I Eyedropper, Z Zoom, H Hand, ⌘Z Undo, ⇧⌘Z Redo, ⌘A Select All, ⌘D Deselect, ⌘0 Fit, ⌘1 100%, X Swap colors, D Default colors, [ / ] Layer down/up, ⌫ Clear selection.
- **Place Embedded** (import, 380, "Place"): static; Asset select Sun flare|Gradient map|Noise texture|Vignette. onOk: new layer with generated content — Sun flare radial glow blend screen; Gradient map #1f6feb→#ff7ab8 soft-light .6; Noise gray noise alpha40 overlay; Vignette radial black multiply.
- **About** (`aboutModal(fw)`): "Decius PhotoEditor" or "About decius.css"; badges "decius.css 0.5.3", "IBM Plex Sans", "225 icons", "flat · comfortable".

---

## 8. ADJUSTMENTS & FILTERS

### Native pixel adjustments (`PS.adjust(key)`) — live-preview modals (`adjustModal`): captures base ImageData of selection region; slider input re-applies live; Cancel restores; OK snapshots.
- **bc**: Brightness [-100..100, 0], Contrast [-100..100, 0]. `d=(d-128)*(c/100+1)+128+b*1.2`.
- **hsl**: Hue [-180..180], Saturation [-100..100], Lightness [-100..100]. HSV per-pixel: h+=dh, s*=(1+ds), v*=(1+dl).
- **levels**: Black [0..254, 0], White [1..255, 255], Gamma [10..300, 100]. `t=((d-lo)/(hi-lo))^(100/gamma)*255`.
- **vibrance**: single [-100..100, 0] → sat op.
- **invert** (no modal): 255-d. **desat**: luma gray. **threshold**: luma>128?255:0.
- **fallback** (curves/balance/mixer/photo/exposure): generic Amount [-100..100] modal adds to RGB; toast "'<key>' — adjustment preview applied".

### Filters (`PS.filter`)
- fblur — Gaussian Blur modal: Amount [0..40, 4] → canvas `filter: blur(Npx)`.
- fsharp — `contrast(1.4) saturate(1.1)`. fnoise — random ±45/channel. fpix — 10px block pixelate. femboss — `grayscale(1) contrast(2)`. ffind — `grayscale(1) invert(1) contrast(2.5)`.
- Infra: `runPixels(fn)` getImageData/putImageData over `region()` (selection or whole doc); `runFilterCanvas(filterStr)`.

### Theme tweaks (two UIs, same effect)
- Static popover `#ps-tweaks` (menubar cog): Density S/M/L → compact/comfortable/spacious; Style Flat/3D; Accent dots blue/orange/green/purple/teal. Sets body `data-dcs-*`.
- React overlay (tweaks.jsx/tweaks-panel.jsx): dev edit-mode harness (accent/density/corners/depth/rulers); port only needs the net effect (data-dcs-* attrs + ruler visibility). Defaults: accent #4d9fff, density comfortable, corners default, depth flat, rulers true.

---

## 9. STATUS BAR + MISC UI (ui.js)

- Color math: hsvToRgb, rgbToHsv, toHex, hexToRgb.
- setForeground/setBackground sync chips + panel; syncColorPanel updates hex/RGB/SV/hue dots.
- Two pickers: color panel (SV+hue+RGB+hex) and popover `#ps-color-picker` (`#ps-cp-sv/hue/hex`, target fg/bg chip).
- `refreshDocMeta`: `#ps-ov-doc` "W × H"; `#ps-st-doc` "<MB>M / <MB×layers>M" (MB=w·h·4/1048576); `updateTitle` "<name> @ <z%>% (RGB/8)".
- `refreshZoomUI` updates zoom field (unless focused) + title. `updateCursorReadout` "X, Y px".
- wireZoom: zoom field change → setZoom(v/100); Fit button → fitToScreen.
- Chrome: rulers (canvases), NO scrollbars (pan only), checkerboard `.ps-doc` bg, marching ants CSS.
- **Keys** (`wireKeys`, ignored in inputs): Space hold = pan; ⌘Z undo / ⇧⌘Z redo / ⌘Y redo; ⌘A all; ⌘D deselect; ⌘= in; ⌘- out; ⌘0 fit; ⌘1 100%; ⌘S toast. X swap; D default (#000/#fff); [ layer down; ] layer up; Delete/Backspace clear selection region + snapshot "Clear". Else single-key tool shortcut (first match — G→fill, O→dodge, R→smudge).
- **Boot**: buildMenus/Tools/Swatches/Adjust/Channels → initCanvas → generateSampleScene → fg #1f6feb bg #ffffff → selectTool('brush') → pickers → wireLayerControls/Chips/Zoom/Keys/Tweaks → initPointer → decius.init → refreshLayers → snapshot "Open" (sourceIndex 0) → fitToScreen retry → refreshDocMeta → resize listener → welcome toast. panels.js initDetached: floating panels resizable, navigator/floatbar wired, render/applyView wrapped for live navigator.

---

## 10. APP.CSS keys

- `.ps-app` grid `auto auto 1fr auto` / 100vh×100vw. `.ps-body` relative overflow hidden.
- `.ps-brand` dark patch #0d0f14 (hover #14171e), mark accent 14px, name #e7e9ee 600.
- `.ps-options` flex, height `--dcs-h-xl`, surface-1 bg, bottom border, nowrap. `.ps-tool-glyph` 26×26 well accent 16px. `.ps-tool-name` 600.
- `.ps-toolstrip` column 2px gap. `#ps-tools` grid `repeat(2, 34px)` 1px gap. Separators span 2 cols, 28px.
- `.ps-tool` 34×34, r-2, dim, 17px; hover surface-2; `[aria-pressed=true]` accent-dim bg + accent-hi color + accent-lo border; `[data-group="1"]::after` corner triangle.
- `.ps-colorchips` 38×38 overlap: chips 24×24 line-strong border; fg top-left z2 (#000), bg bottom-right z1 (#fff); swap top-right; reset bottom-left.
- `.ps-stagewrap` grid `18px 1fr / 18px 1fr`, `--dcs-stage` bg. Rulers surface-1, mono 8px. `.ps-stage` crosshair (.is-pan grab, .is-panning grabbing).
- `.ps-doc` absolute, `transform: translate(-50%,-50%) translate(var(--px),var(--py)) scale(var(--z))`; shadow `0 0 0 1px #0008, --dcs-shadow-3`; checkerboard `repeating-conic-gradient(#cfcfcf 0 90deg,#fff 90deg 180deg)` 16px; `.is-pixelated` image-rendering:pixelated.
- `.ps-marquee` outline 1px dashed #fff + 1px #000 shadow; `@keyframes ps-ants` outline-offset to -4px 0.6s.
- Layers rows: bottom border, hover surface-1, `.is-active` accent-dim + 2px inset accent bar; eye 18px (.is-off dim); thumb 34×34 line-strong + checkerboard 8px; name ellipsis (input variant accent border); drop lines 2px accent.
- History: `.is-current` accent-dim, `.is-future` opacity .4.
- `.ps-amt` mono, cursor:ew-resize, hover well. Lock group pressed accent-dim. `.ps-fx` italic Georgia. `.ps-tcap` bold 11px.
- `.ps-nav-thumb` 116px checkerboard 10px cursor move; `.ps-nav-view` `--dcs-danger` 1.5px border.
- Overrides: menu labels sentence-case; `.dcs-btn--icon` transparent until hover.
- `.ps-rz` (e/s/se) resize handles for floating panels.
- Dialogs: `.ps-dlg-row` grid `116px 1fr`; `.ps-dlg-sec` uppercase; `.ps-dlg-slider` grid `1fr 46px`; `.ps-anchor-grid` 3×3 of 22px; `.ps-kbd-list` 2-col.
- `.ps-sv` 120px; `.ps-hue` 14px; `.ps-swatches` 10-col; `.ps-swatch-chip` aspect-1.

---

## 11. ICON INVENTORY (`di-*`)

Menubar/chrome: decius, cog, image, check-circle, close, globe, palette, grid, layers, eq, spline, filmstrip, color-grade, history-brush, fit, undo, redo, zoom-in, zoom-out.
Tools: move, marquee, lasso, select, clip, eyedropper, brush, pencil, stamp, eraser, fill, dodge, burn, smudge, blur, pen, pan, layer-mask.
Options/layers: plus, minus, check, poly, cube, bolt, link, duplicate, trash, folder, lock.
Adjustments: light, graph, curve, mono, mirror, droplet, wave-sine, filter-lp, gain.
Menus: file, folder-open, import, save, export, cut, copy, paste, edit, scale-corners, aspect, chevron-up, chevron-down, compress, magnet, keys, info, sharpen, wave-noise, extrude, cross-target, chevron-left, chevron-right.
History icons: eye, eye-off, delete, opacity, sharpen.
Custom inline SVGs (not font icons): type (T glyph), shape (rounded rect), gradient (gradient rect).
Non-icon glyphs: ⬚ (reset), ⇄ (swap), × (tab close), fx (Georgia italic), T (.ps-tcap).

---

## Stubbed in web (parity awareness — do NOT over-build)
- Lasso = bbox rect selection only. Wand = bbox of flood mask. Feather stored, never applied. Pen preview-only; Paths tab static. Channels/Comps static. Quick Mask, layer filter buttons, link/fx/mask/group footer buttons, Cut/Copy/Paste/Close/Grow/Show Grid/Snap/Window menu items = toasts/no-ops. Curves/Balance/Mixer/Photo/Exposure = generic Amount fallback modal. Two theme-tweak UIs both just set body data-dcs-*.
