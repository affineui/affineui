# DENDER Web Sample — Complete Decode for C++ Port

Files decoded in full: `index.html` (1047 lines), `app.js` (797 lines), `viewport.js` (692 lines), `app.css` (341 lines). All four live in `c:\Users\benjcooley\projects\decius-css\samples\dender\`. Third-party deps: `../../dist/css/decius.bundle.min.css`, `../../dist/js/decius.min.js`, and three.js r0.170.0 (`THREE`, `OrbitControls`, `TransformControls`) loaded via importmap from jsDelivr CDN.

Global body attributes: `<body class="dcs" data-dcs-density="comfortable" data-dcs-accent="orange">`. Root app container: `<div class="dn-app" id="dender">`.

---

## 1. WINDOW / LAYOUT STRUCTURE

The `.dn-app` is a vertical flexbox (`display:flex; flex-direction:column; position:fixed; inset:0; background:var(--dcs-bg-app)`). Top-to-bottom the three direct children are: TOPBAR, WORK AREA, STATUS BAR.

### 1a. TOPBAR — `<header class="dcs-toolbar dn-topbar">`
Single horizontal toolbar. Children left-to-right:
- **Logo cell** `<div class="dn-logo" data-dcs-tip="DENDER — splash & about">`: `<i class="di di-decius dn-logo__mark">` + `<span class="dn-logo__name">DENDER</span>`.
- **Menubar** `<nav class="dcs-menubar">` with 5 `<button class="dcs-menubar__item" data-dcs-toggle="menu" data-dcs-target="#menu-...">`: File, Edit, Render, Window, Help.
- `<span class="dcs-toolbar__sep">`.
- **Workspace picker** `<div class="dcs-select dcs-select--btn dn-workspace-pick" data-dcs-toggle="menu" data-dcs-target="#menu-workspace" data-dcs-tip="Workspace">`: label "Layout" + caret (`di di-chevron-down`).
- `<span class="dcs-toolbar__spacer">` (flex-grow gap).
- **Scene picker** `dcs-select dcs-select--btn` → `#menu-scene`, `data-dcs-tip="Scene"`: icon `di di-cube` + label "Scene" + caret.
- **ViewLayer picker** `dcs-select dcs-select--btn` → `#menu-viewlayer`, `data-dcs-tip="View Layer"`: icon `di di-layers` + label "ViewLayer" + caret.
- `<span class="dcs-toolbar__sep">`.
- **Theme-tweaks cog** `<button class="dcs-btn dcs-btn--icon dcs-btn--ghost" data-dcs-toggle="popover" data-dcs-target="#dn-tweaks" data-dcs-placement="bottom-end" data-dcs-tip="Theme tweaks">` icon `di di-cog`.

(Note: there is no separate "workspaces bar" row; `.dn-workspaces` CSS exists but is unused in markup — the workspace picker is inline in the topbar.)

### 1b. WORK AREA — `<div class="dcs-dock dcs-dock--v dn-workarea">`
Nested flex dock with splitters (all resizing via stock decius `data-dcs-splitter`). Structure:

```
.dn-workarea (.dcs-dock.dcs-dock--v)
├─ .dcs-dock  (row, style="flex:1;min-height:0")
│   ├─ .dn-viewport (.dcs-dockpane.dcs-dockpane--center, style="flex:1")   ← CENTER
│   ├─ .dcs-splitter[data-dcs-splitter]                                     ← vertical drag
│   └─ .dn-right (.dcs-dock.dcs-dock--v, style="flex:0 0 340px")            ← RIGHT COLUMN
│        ├─ .dn-outliner (.dcs-dockpane, style="flex:0 0 240px")
│        ├─ .dcs-splitter.dcs-splitter--h[data-dcs-splitter="h"]
│        └─ .dn-props (.dcs-dockpane, style="flex:1")
├─ .dcs-splitter.dcs-splitter--h[data-dcs-splitter="h"]
└─ .dn-timeline (.dcs-dockpane, style="flex:0 0 140px")                     ← BOTTOM
```
Dock sizing: right column fixed 340px wide; outliner fixed 240px tall (top of right column), properties fill remainder; timeline fixed 140px tall at bottom; viewport fills all remaining center space.

### 1c. STATUS BAR — `<footer class="dcs-statusbar">`
Items left→right: `dcs-statusbar__item` with `di di-cube` + "Cube"; `dcs-statusbar__spacer`; then items: "Verts **8**", "Faces **6**", "Tris **12**", "Objects **3/3**", "Memory **42.6 MB**", "v1.0 · DENDER". (bold numbers via `<b>`.)

---

## 2. EVERY PANEL / REGION IN DETAIL

### 2a. VIEWPORT PANE — `.dn-viewport` (`.dcs-dockpane--center`)
**Tabbar** (`.dcs-dockpane__tabbar`):
- Tab: `.dcs-dockpane__tab` aria-selected="true", `data-dcs-target="#vp-body"`: icon `di di-cube` + "3D Viewport".
- **Header toolbar** (`.dcs-dockpane__toolbar` `data-dcs-tabtoolbar="#vp-body"`), left→right:
  1. Mode select `dcs-select--btn` → `#menu-mode`, `data-dcs-tip="Interaction Mode"`, label **"Object Mode"** + caret.
  2. `dcs-toolbar__sep`.
  3. Menubar buttons → menus: **View** (`#menu-view`), **Select** (`#menu-select`), **Add** (`#menu-add`), **Object** (`#menu-object`).
  4. `dcs-toolbar__sep`.
  5. Orientation select → `#menu-orient`, `data-dcs-tip="Transform Orientation"`, label **"Global"** + caret.
  6. Pivot button `dcs-btn--icon dcs-btn--ghost` → `#menu-pivot`, tip "Transform Pivot Point", icon `di di-pivot`.
  7. Snapping toggle button `aria-pressed="false"`, tip "Snapping", icon `di di-magnet`.
  8. Proportional-editing toggle `aria-pressed="false"`, tip "Proportional Editing", icon `di di-cross-target`.
  9. `dcs-toolbar__spacer`.
  10. Show Gizmo toggle `aria-pressed="true"`, tip "Show Gizmo", icon `di di-gizmo`.
  11. Show Overlays toggle `aria-pressed="true"`, tip "Show Overlays", icon `di di-view-bbox`.
  12. Toggle X-Ray `aria-pressed="false"`, tip "Toggle X-Ray", icon `di di-eye`.
  13. **Shading modes** `<div class="dcs-btn-group dn-shademodes" data-dcs-tip="Viewport Shading">` — 4 radio buttons `data-dcs-radio="shade"`: `di di-view-wire` (wireframe), `di di-view-solid` (**aria-pressed="true"**, default solid), `di di-view-tex` (texture), `di di-view-render` (render).
  14. Shading-options button → `#menu-vpoptions`, tip "Viewport Shading Options", icon `di di-chevron-down`.
  15. `dcs-toolbar__sep`.
  16. Label `<span>` "Render Engine" (muted, `--dcs-fs-xs`).
  17. Engine select → `#menu-engine`, tip "Render Engine", label **"Cycles"** + caret.

**Body** — `#vp-body` `data-dcs-tabpanel` contains `<div class="dn-vp-canvas" data-screen-label="3D Viewport" data-dcs-float-host>`:
- `<canvas id="vp-scene">` — the WebGL surface (three.js renders here).
- **Stats overlay** `.dn-vp-stats`: line 1 `<b>User Perspective</b>`, line 2 "(1) Collection | Cube".
- **Corner overlay** `.dn-vp-corner`: "Verts 8 | Faces 6 | Tris 12 | Objects 3/3".
- **Floating tool rail** `<div class="dcs-toolbar dcs-toolbar--v dcs-toolbar--sm dcs-toolbar--floating dn-toolrail" style="left:8px;top:8px" data-dcs-drag-bounds=".dn-vp-canvas">`:
  - Drag grip `<span class="dcs-grip dcs-grip--h" data-dcs-drag-handle>`.
  - Radio group `data-dcs-radio="tool"` icon buttons (`dcs-btn--icon dcs-btn--sm dcs-btn--ghost`): **Tweak** (`di di-cross-target`, aria-pressed="true"), **Cursor** (`di di-pen`); `dcs-divider`; **Move** (`di di-move`), **Rotate** (`di di-rotate`), **Scale** (`di di-scale`), **Transform** (`di di-axes`); `dcs-divider`; **Annotate** (`di di-pen`), **Measure** (`di di-graph`), **Add Cube** (`di di-plus`). Tips are the button labels.
- **Nav cluster** `.dn-navcluster` (positioned top-right, `right:232px`):
  - `<svg class="dn-gizmo" id="vp-gizmo" viewBox="0 0 100 100">` (axis nav ball, built by JS).
  - `.dn-navbtns` column of 4 icon buttons: **Zoom** (`di di-search`), **Move View** (`di di-move`), **Camera View** (`di di-camera`), **Toggle Perspective/Ortho** (`di di-grid`).
- **Floating N-panel** `<div class="dcs-panel dcs-panel--floating dn-npanel" style="right:8px;top:8px;bottom:8px;width:220px" data-dcs-drag-bounds=".dn-vp-canvas">` containing a `.dcs-dockpane` with tabbar (`data-dcs-drag-handle`) of 3 tabs:
  - **Item** tab (`di di-cube`, aria-selected="true") → `#npanel-item`:
    - Foldout **"Transform"**: field "Location" → `.dcs-vec` of 3 `data-dcs-combo` (label X/Y/Z, value 0, step 0.01, suffix " m"); field "Dimensions" → 3 combos (X/Y/Z, value **2**, step 0.01, suffix " m").
    - Foldout **"View"**: field "Focal" → combo (label "Lens", min 1, max 250, step 0.5, value **50**, suffix " mm"); field "Clip Start" → combo (value **0.1**, step 0.01, suffix " m"); checkbox "Lock to Object" (`.dcs-check`, unchecked); `dcs-divider`; field "Transform" → btn-group `data-dcs-radio="orient"`: "Local", "Global" (aria-pressed="true").
  - **Tool** tab (`di di-axes`) → `#npanel-tool` (empty, hidden).
  - **View** tab (`di di-camera`) → `#npanel-view` (empty, hidden).

### 2b. OUTLINER PANE — `.dn-outliner` (`.dcs-dockpane`, 240px tall)
**Tabbar tab**: aria-selected, `data-dcs-target="#outliner-body"`, icon `di di-folder-open` + "Outliner".
**Toolbar** (`data-dcs-tabtoolbar="#outliner-body"`):
- Display-mode select → `#menu-displaymode`, tip "Display Mode", label **"View Layer"** + caret.
- `<input class="dcs-input" type="text" placeholder="Search" style="max-width:140px">`.
- Filter button → `#menu-filter`, tip "Filter", icon `di di-eq`.
- New-collection button, tip "New Collection", icon `di di-folder` (cosmetic).

**Body** — `#outliner-body` → `<div class="dcs-tree" id="outliner-tree" data-dcs-select="multi">`. Static rows (JS injects object rows between Collection and World):
- Row `--depth:0`: chevron `--open` (`di di-chevron-right`), icon `di di-cube`, label **"Scene"**.
- Row `--depth:1`: chevron `--open`, icon `di di-folder`, label **"Collection"**, meta `<i class="di di-eye" data-dcs-tip="Hide in Viewport">`.
- (object rows injected here at depth 2 by `rebuildOutliner`)
- Row `--depth:0`: chevron (closed), icon `di di-globe`, label **"World"**.

### 2c. PROPERTIES / INSPECTOR PANE — `.dn-props` (`.dcs-dockpane`, flex:1)
**Tabbar tab**: aria-selected, `#props-body`, icon `di di-cog` + "Inspector".
**Toolbar** (`data-dcs-tabtoolbar="#props-body"`):
- Editor-type select → `#menu-editortype`, tip "Editor Type", icon `di di-cog` + caret.
- Layout btn-group `data-dcs-tip="Inspector Layout"`, radios `data-dcs-radio="proplayout"`: Row (`di di-eq`, tip "Row"), Stack (`di di-layers`, tip "Stack", **aria-pressed="true"**), Grid (`di di-grid`, tip "Grid").
- Search `<div class="dcs-search dn-props-search">`: icon `di di-search` + `<input class="dcs-input dcs-input--sm" type="search" placeholder="Search inspector…">`.

**Body** — `#props-body`: a left vertical icon rail + a sheet.
- **Icon rail** `<div class="dcs-tabs dcs-toolbar dcs-toolbar--v dcs-toolbar--sm" id="prop-tabs">` — 14 tab buttons (`dcs-tab dcs-btn dcs-btn--icon dcs-btn--sm`), each `data-dcs-target` + `data-dcs-tip`:
  1. `#prop-tool` — "Active Tool" — `di di-axes`
  2. `#prop-render` — "Render" — `di di-render`
  3. `#prop-output` — "Output" — `di di-export`
  4. `#prop-viewlayer` — "View Layer" — `di di-layers`
  5. `#prop-scene` — "Scene" — `di di-cube`
  6. `#prop-world` — "World" — `di di-globe`
  7. `#prop-object` — "Object" — `di di-cube` — **aria-selected="true"** (default)
  8. `#prop-modifiers` — "Modifiers" — `di di-cog`
  9. `#prop-particles` — "Particles" — `di di-cube`
  10. `#prop-physics` — "Physics" — `di di-bolt`
  11. `#prop-constraints` — "Constraints" — `di di-link`
  12. `#prop-data` — "Object Data" — `di di-mesh`
  13. `#prop-material` — "Material" — `di di-droplet`
  14. `#prop-texture` — "Texture" — `di di-texture`

- **Sheet** `.dn-props__sheet` — one `data-dcs-tabpanel` per tab:
  - **`#prop-object`** (visible default): datablock row `.dn-datablock` — icon `di di-cube` `id="dn-object-icon"` (accent-colored), `<input class="dcs-input" id="dn-object-name" value="Cube">`, browse button `di di-more-h` (tip "Browse Object"). Then foldouts:
    - **Transform** (open): Location vec 3 combos (X/Y/Z, value 0, step 0.01, suffix " m"); Rotation vec 3 combos (X/Y/Z, value 0, step 1, dec 1, suffix "°"); Scale vec 3 combos (X/Y/Z, value **1.000**, step 0.01, no suffix).
    - **Delta Transform** (`dcs-foldout--collapsed`, empty body).
    - **Relations** (open): field "Parent" → `<input placeholder="—">`; field "Collection" → select label "Collection" + caret.
    - **Viewport Display** (collapsed, empty).
  - **`#prop-modifiers`** (hidden): full-width `dcs-btn--lg` "Add Modifier" (`di di-plus`). Modifier card `.dn-modifier`: head with `di di-chevron-down`, `di di-cube`, title **"Bevel"**, buttons: Edit-mode display (`di di-mesh`), Realtime (`di di-view-solid`, aria-pressed="true"), menu (`di di-chevron-down` → `#menu-modifier`), Delete (`di di-close`). Body: btn-group radios `data-dcs-radio="bev"` "Width"(pressed)/"Angle"; field "Amount" combo (value 0.1, step 0.01, suffix " m"); field "Segments" `data-dcs-slider` (min 1, max 12, value **2**); checkbox "Clamp Overlap".
  - **`#prop-material`** (hidden): row — preview chip `.dn-mat-preview`, `<input value="Material">`, `+` (`di di-plus`), `−` (`di di-minus`). Foldout **Surface**: field "Surface" → select "Principled BSDF"; field "Base Color" → `.dcs-colorfield` chip `background:#cf6b3a`; field "Metallic" slider (min 0 max 1 value 0); field "Roughness" slider (value **0.45**); field "IOR" combo (value **1.450**, step 0.01).
  - **`#prop-tool`** (hidden): caption `.dn-cap` "Active Tool & Workspace settings."; foldout Transform → checkbox "Affect Only Origins".
  - **`#prop-render`** (hidden): foldout Sampling → field "Render" combo (value 4096), "Viewport" combo (value 1024), checkbox "Denoise".
  - **`#prop-output`** (hidden): foldout Format → "Resolution X" combo (value 1920, suffix " px"), "Resolution Y" (value 1080, suffix " px"), "Frame Rate" select "24 fps".
  - **`#prop-viewlayer`** (hidden): caption "View Layer passes & filters."
  - **`#prop-scene`** (hidden): foldout Units → field "Unit System" select "Metric".
  - **`#prop-world`** (hidden): foldout Surface → field "Color" colorfield chip `#3a3d45`; field "Strength" combo (value 1.000).
  - **`#prop-particles`** (hidden): caption "No particle systems. Press + to add."
  - **`#prop-physics`** (hidden): 4 `dcs-btn--lg` buttons "Rigid Body", "Collision", "Cloth", "Fluid".
  - **`#prop-constraints`** (hidden): full-width `dcs-btn--lg` "Add Object Constraint" (`di di-plus`).
  - **`#prop-data`** (hidden): foldout Normals → checkbox "Auto Smooth"; foldout UV Maps → single-select tree with one row aria-selected, icon `di di-image`, label "UVMap".
  - **`#prop-texture`** (hidden): caption "No textures in active slot."

### 2d. TIMELINE PANE — `.dn-timeline` (`.dcs-dockpane`, 140px tall)
**Tabbar tab**: aria-selected, `#timeline-body`, icon `di di-keyframe` + "Timeline".
**Toolbar** (`data-dcs-tabtoolbar="#timeline-body"`):
- Editor-type select → `#menu-editortype`, tip "Editor Type", icon `di di-keyframe` + caret.
- Menubar buttons: **View** (`#menu-tlview`), **Marker** (`#menu-tlmarker`).
- `dcs-toolbar__sep`.
- **Transport** btn-group `.dn-tl-controls`: Jump to Start (`di di-skip-back`), Previous Keyframe (`di di-keyframe`), Play Reverse (`di di-rewind`), **Play Animation** (`di di-play`, `id="tl-play"`, aria-pressed="true"), Next Keyframe (`di di-keyframe`), Jump to End (`di di-skip-fwd`).
- Frame combo `<div data-dcs-combo id="tl-frame" data-value="24" data-min="1" data-max="250" data-step="1" data-dec="0" style="width:72px" data-dcs-tip="Current Frame">`.
- `dcs-toolbar__sep`.
- Keying-set button → `#menu-keying`, tip "Keying Set", icon `di di-keyframe` + "Keying".
- Auto-keying toggle `aria-pressed="false"`, tip "Auto Keying", icon `di di-record`.
- `dcs-toolbar__spacer`.
- Field "Start" → combo `id="tl-start"` (value 1, min 0, max 9999, step 1, dec 0, w 72px).
- Field "End" → combo `id="tl-end"` (value 250, min 1, max 9999, step 1, dec 0, w 72px).

**Body** — `#timeline-body` → `<div class="dn-timeline-body" data-screen-label="Timeline Dopesheet">`:
- Ruler `<div class="dn-tl-ruler" id="tl-ruler">` (ticks built by JS).
- Tracks `<div class="dn-tl-tracks" id="tl-tracks">`: track "Summary" (`.dn-tl-track__label`), track "Cube" (label colored `var(--dcs-accent)`).
- Playhead `<div class="dn-playhead" id="tl-playhead">` with flag `<div class="dn-playhead__flag" id="tl-playhead-flag">24</div>`.

### 2e. THEME-TWEAKS POPOVER — `#dn-tweaks` (`.dcs-popover .dn-tweaks-popover`, width 260px)
Header: icon `di di-cog` + "Theme tweaks". Body `.dcs-form` with 3 fields:
- **Density** btn-group `id="density-row"`: "S" (`data-density-set="compact"`), "M" (`comfortable`, aria-pressed="true"), "L" (`spacious`).
- **Style** btn-group `id="style-row"`: "Flat" (`data-style-set="flat"`, aria-pressed="true"), "3D" (`data-style-set="3d"`).
- **Accent** `.dn-accent-row id="accent-row"`: 5 dots `.dn-accent-dot` `data-accent-set`: blue `#4f86d6`, orange `#e8843a`, green `#4e9e54`, purple `#8466cf`, teal `#2f9c93`.

---

## 3. MENUS (all `.dcs-menu`, hidden; items `.dcs-menu__item`; separators `.dcs-menu__sep`; section headers `.dcs-menu__label`; submenu marker class `.dcs-menu__item--has-sub` with caret `di di-chevron-right`; shortcuts in `.dcs-menu__shortcut`; leading icon in `.dcs-menu__icon`; check-mark items use trailing `.dcs-menu__icon` with `di di-check`)

**`#menu-file`**: New (`di di-image`, Ctrl N) · Open… (`di di-folder`, Ctrl O) · Open Recent ▸ · sep · Save (Ctrl S) · Save As… (Ctrl ⇧ S) · sep · Import ▸ · Export ▸ · sep · Quit (Ctrl Q).

**`#menu-edit`**: Undo (Ctrl Z) · Redo (Ctrl ⇧ Z) · Undo History · sep · Menu Search… (F3) · Rename Active Object… (F2) · sep · Preferences… (`di di-cog`).

**`#menu-render`**: Render Image (`di di-render`, F12) · Render Animation (Ctrl F12) · sep · View Render (F11) · View Animation (Ctrl F11).

**`#menu-window`**: New Window · New Main Window · sep · Toggle Window Fullscreen (`di di-fullscreen`) · Save Screenshot….

**`#menu-help`**: Manual · Tutorials · Report a Bug · sep · About DENDER.

**`#menu-add`** (static HTML — replaced at runtime by app.js, see §5): Mesh ▸ (`di di-mesh`) · Curve ▸ (`di di-spline`) · Surface ▸ · Metaball ▸ · Text · sep · Armature (`di di-arm`) · Empty ▸ (`di di-cross-target`) · Light ▸ (`di di-light`) · Camera (`di di-camera`).

**`#menu-view`**: Frame All (Home) · Frame Selected (NumPad .) · sep · Viewpoint ▸ · Navigation ▸ · Cameras ▸ (`di di-camera`).

**`#menu-select`**: All (A) · None (Alt A) · Invert (Ctrl I) · Box Select (B).

**`#menu-object`**: Set Origin ▸ · Shade Smooth · Shade Flat · sep · Duplicate Objects (⇧ D) · Join (Ctrl J) · Delete (X).

**`#menu-mode`**: Object Mode (`di di-cube`, shortcut Tab) · Edit Mode (`di di-mesh`) · Sculpt Mode · Vertex Paint · Weight Paint · Texture Paint.

**`#menu-workspace`** (items carry `data-dcs-value`): Layout (`layout`, trailing check `di di-check`) · Modeling (`modeling`) · Sculpting · UV Editing (`uv`) · Texture Paint (`texture`) · Shading · Animation · Rendering · Compositing · Geometry Nodes (`geometry`) · Scripting · sep · Add Workspace… (`add`, `di di-plus`).

**`#menu-engine`** (`data-dcs-value`): Eevee (`eevee`) · Workbench (`workbench`) · Cycles (`cycles`, trailing check `di di-check`).

**`#menu-scene`**: Scene (`di di-cube`, trailing check) · sep · New Scene (`di di-plus`).

**`#menu-viewlayer`**: ViewLayer (`di di-layers`, trailing check) · sep · Add View Layer (`di di-plus`).

**`#menu-editortype`**: label "General" → 3D Viewport (`di di-cube`), Image Editor (`di di-image`), Shader Editor (`di di-view-tex`); label "Animation" → Timeline (`di di-keyframe`), Dope Sheet (`di di-eq`), Graph Editor (`di di-spline`); label "Data" → Outliner (`di di-folder`), Inspector (`di di-cog`).

**`#menu-displaymode`**: Scene Collection · View Layer (trailing check) · Blender File · Data API · Orphan Data.

**`#menu-orient`**: Global (trailing check) · Local · Normal · Gimbal · View.

**`#menu-pivot`**: Median Point (`di di-pivot`, trailing check) · 3D Cursor · Individual Origins · Active Element · Bounding Box Center.

**`#menu-vpoptions`**: label "Lighting" → Studio (trailing check), MatCap, Flat; sep; Backface Culling · Cavity · Shadow (trailing check).

**`#menu-filter`**: label "Restriction Toggles" → Visibility (`di di-eye`), Disable in Renders (`di di-render`), Selectable (`di di-lock`); sep; Filter by Type.

**`#menu-modifier`**: Apply (Ctrl A) · Duplicate · Copy to Selected · sep · Move to First · Move to Last.

**`#menu-keying`**: Location · Rotation · Scale · Location, Rotation & Scale (trailing check).

**`#menu-tlview`**: Frame All · Frame Playback Range · Show Seconds (leading check `di di-check`).

**`#menu-tlmarker`**: Add Marker (M) · Rename Marker · Delete Marker.

---

## 4. DOCUMENT / SCENE MODEL (state lives in viewport.js, mirrored by app.js)

The authoritative model is inside `viewport.js`'s IIFE closure (published as `window.DenderVP`). There is **no separate document model in app.js** — app.js reads/writes `DenderVP` and DOM.

**Object registry** — `const objects = []`, each record:
```
{ id: "obj_N", name, type, root: THREE.Object3D, mesh?, helper?, lightObj?, parentObj: null }
```
- `id` from `newId()` → `obj_1`, `obj_2`, … (monotonic counter).
- `name` uniquified via `uniqueName(base)` → collisions get `.001`, `.002` suffixes (`base.NNN`, 3-digit zero-padded).
- `type` = one of the primitive library keys.
- `parentObj` = parent record or null (scene-root).

**Selection state**: `let selected = null` (the active object) + `const multiSel = new Set()` (additional shift-selected). `VP.selection` getter = `[selected, ...multiSel]`.

**Transform**: stored directly on `obj.root` (three.js `position`/`rotation`(radians)/`scale`). Inspector displays rotation in degrees (converts `× 180/π`).

**Mode**: transform-gizmo mode only — `translate` / `rotate` / `scale`, held by `TransformControls` (`VP.mode` getter). There is NO real object/edit-mode state machine; the "Object Mode" select and `#menu-mode` are cosmetic.

**Scene collections / hierarchy**: purely presentational in the Outliner — static rows Scene → Collection → World. Objects are all injected under "Collection" at depth 2 (or deeper if re-parented). Re-parenting only changes three.js parent + `parentObj` link.

**INITIAL SCENE (exact)** — built at boot (viewport.js §9), in order:
1. `VP.add('Cube', { name: 'Cube' })` → BoxGeometry 1.6³, `MeshStandardMaterial` color `0x9aa1ad` roughness 0.55 metalness 0.05, positioned `y=0.8`. Selected as active.
2. `VP.add('Point Light', { name: 'Light', position: [2.6, 2.4, 1.2] })`.
3. `VP.add('Camera', { name: 'Camera', position: [-3.4, 2.0, -3.2] })`.

So the initial scene has exactly **3 objects: Cube (active/selected), Light, Camera** — matching the "Objects 3/3" statusbar/overlay and the Outliner. (Note: static HTML Location combos show 0/0/0 but the actual Cube mesh sits at y=0.8; app.js overwrites the Inspector from the live object on select, reading `root.position` — for the Cube root that's y=0.8.)

**Timeline model** — `app.js` object `TL = { start: 1, end: 250, frame: 24, ppf: 4.2 }`. Keyframes hardcoded array `[1, 24, 48, 72, 96, 130, 175, 220]` placed on both Summary and Cube tracks.

---

## 5. BEHAVIORS / INTERACTIONS (app.js — what is REAL vs COSMETIC)

app.js `boot()` runs: `decius.init(document)`, `buildScene()` (no-op stub — returns immediately; legacy SVG code is dead), `buildGizmo()`, `buildTimeline()`, `wireAccent()`, `wirePlay()`, `wireFrameValidation()`, `wireOutlinerSelection()`, `wireViewport()`, and resize handlers (rebuild timeline on `resize`/`dn:resize`, debounced 120/60 ms).

**REAL interactions:**

- **Navigation gizmo** (`buildGizmo`): SVG axis ball. 6 nubs (+X/+Y/+Z filled+labelled, −X/−Y/−Z hollow rings, letters fade in on hover). Per-frame (`requestAnimationFrame`) it rotates the world axes by the inverse camera quaternion, depth-sorts nubs (painter's algorithm), dims back-facing (opacity 0.55 when depth<−0.05). **Clicking a nub** calls `snap(dir)`: positions camera `dist` along that axis from `controls.target` and calls `controls.update()` — snaps view down that axis. Only repaints when camera quaternion changes.

- **Timeline** (`buildTimeline`): computes `ppf = max(3.4, width/140)`, left gutter pad 60px. Draws ruler ticks (major every 10 frames with number label, minor every 5), a playback-range tint `.dn-tl-range` (`fx(start)`→`fx(end)`), diamond keyframes on Summary+Cube tracks. **Click/drag in body** = scrub: `setFromX` computes frame = round((x−pad)/ppf) clamped to [start,end], sets `TL.frame`, moves playhead, writes `#tl-frame` value. Uses pointer capture. `placePlayhead` sets playhead left = pad+frame·ppf and updates flag text.

- **Theme controls** (`wireAccent`):
  - Accent row → sets `body[data-dcs-accent]` to clicked dot's `data-accent-set`, marks aria-pressed. REAL.
  - Density row → sets `body[data-dcs-density]`, dispatches `dn:resize` (rebuilds timeline). REAL.
  - Style row → "flat" removes `data-dcs-style`; "3d" sets `data-dcs-style="3d"` (skeuomorphic). REAL.

- **Play button** (`wirePlay`): `#tl-play` toggles `.is-playing` class and swaps its icon between `di di-play` and `di di-pause`. Glyph-only toggle — does NOT actually animate/advance frames. (Cosmetic playback, real glyph swap.)

- **Frame validation** (`wireFrameValidation`): cross-validates `#tl-start` / `#tl-end` / `#tl-frame` combos on `input` so Start ≤ Frame ≤ End is enforced (nudging one past a sibling forces the sibling). REAL, DOM-only (does not drive TL object).

- **Outliner selection → Inspector** (`wireOutlinerSelection`): listens for `dcs:select` on `#outliner-tree`. On select: copies selected row's label into `#dn-object-name`, copies its `di-*` icon class into `#dn-object-icon`, force-activates the Object tab, and if `DenderVP` exists calls `DenderVP.select(name)` (name matches three.js object name). REAL.

- **Viewport bridge** (`wireViewport`) — waits for `dender:vp-ready` if `DenderVP` not yet published:
  - `ICON_FOR` maps types → icon names (all meshes → `cube`; lights → `light`; Camera → `camera`; Empty → `cross-target`).
  - **Outliner rebuild** (`rebuildOutliner`): removes prior `[data-vp-row]` rows, depth-first walks `VP.objects` inserting rows after the "Collection" row at depth 2+ (children indented under parents). Rows get `data-vp-row=<id>`, `draggable="true"`, aria-selected if active. REAL.
  - **Outliner click**: ignores the eye/Hide meta; else reads `data-vp-row` id → `VP.select(id)`. REAL. (Eye/Hide toggle is COSMETIC — click is ignored.)
  - **Outliner drag-reorder** (`dcs:tree-reorder`): computes new parent from drop zone ("into" → child of target or root if Collection; sibling → shares target's parent), calls `VP.reparent(draggedId, newParentId)` then rebuilds. Structural rows (no `data-vp-row`) refuse to move. REAL.
  - **Inspector name/icon**: `setObjectMeta` sets `#dn-object-name` value + `#dn-object-icon` class from selected object.
  - **Inspector Location/Rotation/Scale combos** (two-way bind): reads the three `.dcs-vec` blocks in `#prop-object` (vecs[0]=loc, [1]=rot, [2]=scl). `updateInspectorFromSelected` writes object transform into combos (rotation ×180/π), using `combo.dcsSet()` (framework external setter, fallback to `data-value` + `.dcs-combo__value` text). On combo `input` (via `dcs:change`/'input' detail.value), writes back to `obj.root.position/rotation/scale` (rotation ×π/180), then `VP.setTransform(obj,{})` to refresh helpers + emit. `__suppress` flag prevents feedback loops. REAL two-way binding.
  - **Add menu** (`#menu-add`): app.js **wipes the static HTML** and rebuilds it programmatically: section "Mesh" → items Cube, UV Sphere, Icosphere, Cylinder, Cone, Torus, Plane (all icon `cube`); sep; section "Light" → Point Light, Sun, Spot (icon `light`); sep; Camera (icon `camera`); Empty (icon `cross-target`). Each item gets `data-dcs-value="add:<type>"`. On `dcs:select` with such value → `VP.add(type)` then `VP.select(newObj.id)`. REAL.
  - **Shift+A** (`dender:add-menu` event): opens `#menu-add` anchored to the Add button via `decius.menu.open`. REAL.
  - **Shading modes** (`.dn-shademodes [data-dcs-radio='shade']`): index 0=wire, 1=solid, 2=texture, 3=render. Click sets `material.wireframe = (i===0)` across all scene meshes. So wireframe button toggles wireframe; texture/render are COSMETIC (same as solid visually).
  - **Tool rail → transform mode**: `MODE_FOR_TIP` maps Move→translate, Rotate→rotate, Scale→scale, Transform→translate. Clicking those calls `VP.setTransformMode(...)`. "Add Cube" button → `VP.add("Cube")` + select. `updateToolRail(mode)` reflects active mode via aria-pressed (also updated when G/R/S keys fire VP's `mode` event). Tweak/Cursor/Annotate/Measure buttons are COSMETIC.
  - **Nav buttons**: Zoom → `dolly(shift?1.25:0.8)` (dolly along view dir). Move View → toggles `controls.mouseButtons.LEFT` between PAN and ROTATE (aria-pressed reflects). Camera View → `VP.frameSelected()`. "Toggle Perspective/Ortho" (`di di-grid`) is COSMETIC (not wired). REAL for the first three.
  - **VP→UI event subscriptions**: `add`/`remove`/`rename`/`reparent` → rebuildOutliner; `select` → rebuildOutliner + updateInspectorFromSelected; `transform` → updateInspectorFromSelected; `mode` → updateToolRail. Initializes `updateToolRail(VP.mode)` + `updateInspectorFromSelected()`.

**COSMETIC / non-wired** (no JS handlers — decius provides only generic menu/toggle/tab/tooltip/foldout/splitter/drag behavior): all menubar menus except Add; Mode select and `#menu-mode`; Orientation/Pivot/Snapping/Proportional/Gizmo/Overlays/X-Ray toggles; Engine/Scene/ViewLayer/Workspace pickers; property inspector tabs beyond the Object binding; all Inspector fields except Object transform combos; modifier/material/render/output/world/etc. fields; timeline transport buttons other than Play glyph; keying/marker menus; outliner search/filter/new-collection; N-panel View/Focal/Clip fields; the eye "Hide" toggles.

---

## 6. VIEWPORT (viewport.js — three.js, r0.170.0)

Boots on `three:ready` (or immediately if THREE already present). Guards on presence of `THREE`, `OrbitControls`, `TransformControls`, `#vp-scene` canvas + parent.

**Renderer**: `WebGLRenderer({canvas:#vp-scene, antialias:true, alpha:true})`, pixelRatio min(2,dpr), shadowMap enabled `PCFSoftShadowMap`, `outputColorSpace = SRGBColorSpace`. Sized to host client size (ResizeObserver-driven `resize()`).

**Scene / camera**: `PerspectiveCamera(fov 38, near 0.1, far 200)`, position `(5.2, 3.4, 6.4)`, `lookAt(0,0.6,0)`. Y-up world (three.js default) — NOTE this differs from the dead SVG code which used Z-up iso.

**Lights**: HemisphereLight(sky `0xe6efff`, ground `0x1a1c20`, 0.55); DirectionalLight key (`0xffe2b5`, 1.55) at (5,7,4), castShadow, 2048² shadow map, ortho shadow cam ±10 near0.5 far30; DirectionalLight fill (`0x6090b8`, 0.45) at (−6,4,−3).

**Ground chrome**: `GridHelper(20,20, 0x40444c, 0x2a2d34)` transparent opacity 0.55; invisible shadow-catcher `PlaneGeometry(60,60)` with `ShadowMaterial(opacity 0.32)`, rotated flat, receiveShadow.

**World axes gnomon**: Group of 3 arrows (X `0xd8475a`, Y `0x6fb74a`, Z `0x3f7ad0`), each a cylinder shaft (len 1.06, r 0.022) + cone head (len 0.34, r 0.085), `MeshBasicMaterial` `depthTest:false depthWrite:false toneMapped:false`, `renderOrder 3` (floats over scene). Oriented +Y→axis-dir.

**Primitive library** (11 builders — geometry, material, initial y-offset):
- **Cube** BoxGeometry 1.6³, y=0.8. **UV Sphere** SphereGeometry(0.9,32,24) y=0.9. **Cylinder** (0.7,0.7,1.6,32) y=0.8. **Cone** (0.85,1.6,32) y=0.8. **Torus** (0.8,0.25,16,48) y=0.9, rotX π/2. **Plane** PlaneGeometry(2,2) flat y=0.001. **Icosphere** IcosahedronGeometry(0.9,1) y=0.9. All mesh primitives use `baseMat()` (MeshStandardMaterial 0x9aa1ad), cast+receive shadow.
- **Point Light**: Group of icosahedron edges wire (`0xffd060`), halo sphere (`0xfff1c4`), `PointLight(0xfff1c4,0.6,14,2)`; pos (2.6,2.4,1.2).
- **Sun**: 8 radial spokes + circle disk (LineLoop) wire, `DirectionalLight(0xffe2b5,0.7)`; pos (3.0,4.0,2.5).
- **Spot**: `SpotLight(0xfff0c0,1.2,10,π/6,0.4,1.5)` + target at (0,−1,0), cone-edges helper; pos (−2.4,3.0,1.8).
- **Camera**: wireframe body box + wireframe frustum lines (`0xb6bcc7`), oriented via `Matrix4.lookAt` so −Z faces (0,0.6,0); pos (−3.4,2.0,−3.2).
- **Empty**: 3 orthogonal LineSegments (len 0.5, `0xa0a4ad`); pos (0,1,0).

**Selection highlight**: `selBox` = unit-cube EdgesGeometry wireframe, `LineBasicMaterial 0xe8943c depthTest:false transparent`, renderOrder 2, `matrixAutoUpdate=false`. `updateSelectionBox()` (called every frame + on transform): computes object-space AABB by unioning each child geom bbox transformed by `rootInv·nodeWorld`, composes a matrix (center, identity quat, size clamped ≥1e-3) and multiplies by `root.matrixWorld` — so the box rotates/scales with the object (not world-axis-aligned). Hidden when nothing selected.

**Camera controls** (`OrbitControls`): target (0,0.6,0), damping 0.08, rotateSpeed 0.85, zoomSpeed 0.9, panSpeed 0.8, minDistance 1.8, maxDistance 50, maxPolarAngle π·0.495. `mouseButtons = { LEFT: ROTATE, MIDDLE: DOLLY, RIGHT: PAN }`. Disabled while a gizmo drag is active (`dragging-changed`).

**Transform gizmo** (`TransformControls`): size 0.9; helper added to scene. `dragging-changed` toggles `controls.enabled` and sets a 250ms `suppressClickUntil` to swallow the post-drag click. `objectChange` → updateSelectionBox + emit `transform`.

**Click-to-pick** (Raycaster): on `pointerdown` (button 0, not dragging) records down point; on `pointerup` discriminates click vs drag (ignore if moved >4px, held >600ms, or within suppress window). Converts to NDC, raycasts against all registry roots (recursive — helper lines count). No hit → `VP.deselect()` (unless shift). Hit → walk up to registry root → `VP.select`. **Shift+click** toggles membership in `multiSel` (active object stays gizmo target).

**Public API** `window.DenderVP`: exposes `THREE, scene, camera, renderer, controls, tc, on, PRIMITIVE_TYPES`, getters `objects` (slice copy), `selected`, `selection`, `mode`; methods `add(type,opts)`, `remove(idOrObj)` (re-homes children to grandparent, disposes GPU resources), `duplicate` (Shift+D, offsets x+0.5), `rename` (uniquified), `select`, `deselect`, `setTransformMode`, `reparent` (cycle-safe, preserves world transform via `attach`), `setTransform`, `frameSelected` (F — fits selection or whole scene, dist=max(2,size·1.4)), `_resolve`.

**Event bus**: `VP.on(event, fn)` for `add`/`remove`/`select`/`transform`/`rename`/`reparent`/`mode`.

**Keyboard shortcuts** (ignored while typing in inputs; ignored with meta/ctrl/alt): **G**=translate, **R**=rotate, **S**=scale (not shift), **X**/**Delete**=remove active, **Shift+D**=duplicate+select, **F**=frameSelected, **Shift+A**=dispatch `dender:add-menu`.

**Render loop** (`tick`): `controls.update()` → `updateSelectionBox()` → render → rAF.

**Overlay HUD elements** (DOM, not three.js — see §2a): axis nav gizmo SVG (`#vp-gizmo`, driven by app.js from `DenderVP.camera.quaternion`), stats/corner text overlays, floating tool rail, N-panel, nav buttons. The three-way sync: Outliner ↔ `DenderVP` ↔ Inspector, all bridged in app.js `wireViewport`.

---

## 7. app.css — APP-SPECIFIC STYLING (on top of decius.bundle)

Relies on decius CSS variables: `--dcs-bg-app`, `--dcs-bg`, `--dcs-surface-1/2`, `--dcs-line`, `--dcs-line-strong`, `--dcs-accent`, `--dcs-accent-dim`, `--dcs-text`, `--dcs-text-dim`, `--dcs-text-mute`, `--dcs-text-nudge`, spacing `--dcs-s-1..4`, `--dcs-h` (control height), radii `--dcs-r-1/3`, font-sizes `--dcs-fs-xs/sm`.

Key custom rules the C++ port must replicate:
- `html,body{height:100%;margin:0}`, `body.dcs{overflow:hidden}`, universal `box-sizing:border-box`.
- **`.dn-app`**: `position:fixed;inset:0;display:flex;flex-direction:column;background:var(--dcs-bg-app)`. `.dn-workarea{flex:1;min-height:0}`.
- **`.dn-topbar`**: `gap:var(--dcs-s-2);padding-left:var(--dcs-s-2);align-items:center`. Menubar inside is flattened (`height:auto;background:transparent;border-bottom:none;padding:0;line-height:1`; items `align-self:stretch`).
- **`.dn-logo`**: inline-flex, dark brand patch `background:#0d0f14` (hover `#14171e`), gap s-2, `padding:var(--dcs-text-nudge) var(--dcs-s-4) 0`, negative left margin, `line-height:1`. `.dn-logo__mark{font-size:14px;color:var(--dcs-accent)}`. `.dn-logo__name{font-weight:700;letter-spacing:.14em;font-size:var(--dcs-fs-sm);color:#e7e9ee}`.
- **Accent dots**: `.dn-accent-dot{14×14px;border-radius:50%;box-shadow:inset 0 0 0 1px rgba(255,255,255,.18)}`; selected `box-shadow:0 0 0 2px var(--dcs-bg-app),0 0 0 3px currentColor`.
- **Workarea min-0 fixes**: `.dn-workarea > .dcs-dock`, `.dn-right`, `.dn-viewport/.dn-outliner/.dn-props/.dn-timeline` all `min-width:0;min-height:0`.
- **`.dn-vp-canvas`**: `position:relative;flex:1 1 auto;overflow:hidden`, radial-gradient background `radial-gradient(120% 90% at 50% 8%, #50545d 0%, #3a3d45 38%, #292b31 78%, #202228 100%)`. `canvas#vp-scene{position:absolute;inset:0;width:100%;height:100%}`.
- **`.dn-vp-stats`**: absolute top s-3 left 56px, z4, `--dcs-fs-xs`, color `#d7dae1`, text-shadow `0 1px 2px rgba(0,0,0,.8)`, pointer-events none, line-height 1.5.
- **`.dn-vp-corner`**: absolute bottom-left s-3, z4, `--dcs-fs-xs`, `--dcs-text-dim`.
- **`.dn-navcluster`**: absolute top s-3 **right:232px**, z5, flex gap s-3. `.dn-gizmo{72×72px;pointer-events:none}` (SVG click-through); `.dn-gizmo__nub{pointer-events:auto;cursor:pointer}`, hover → circle stroke #fff; `.dn-gizmo__neglabel{opacity:0}` → hover opacity 1. `.dn-navbtns{flex column;gap:2px}`.
- **`.dn-toolrail`**: gap 2px; `.dcs-divider` → 60% width 1px `var(--dcs-line)` centered.
- **Inspector layout** (anchored to `#props-body`, NOT `.dn-props`, so it survives tear-off): `.dcs-dockpane__body:has(> #props-body){padding:0;display:flex;overflow:hidden}`. `#props-body{flex:1;display:flex;flex-direction:row}` (always horizontal rail+sheet). `#props-body > .dcs-tabs` = vertical rail, `background:var(--dcs-surface-1)`, padding, gap 1px. Tab buttons 28×28px, transparent, radius r-1, `color:var(--dcs-text-dim)`; hover → surface-2 + text; selected → `background:var(--dcs-accent-dim);color:var(--dcs-accent)` (and `::after{display:none}`). `.dn-props__sheet{flex:1;overflow:auto}`, foldouts padding s-3.
- **`.dn-modifier`** card: margin s-3, radius r-3, border `1px var(--dcs-line)`, `background:var(--dcs-surface-1)`. `.dn-modifier__head`: flex, height `--dcs-h`, `background:var(--dcs-surface-2)`, bottom border. `__title{flex:1;font-weight:500;--dcs-fs-sm}`. `__body{padding:s-3;display:grid;gap:s-3}`.
- **`.dn-mat-preview`**: 18×18 circle, `conic-gradient(from 200deg,#cf6b3a,#e8943c 40%,#8a4a26)`, inset shadows.
- **`.dn-cap`**: `--dcs-fs-xs`, `color:var(--dcs-text-mute)`, padding.
- **Timeline**: `.dn-timeline{background:var(--dcs-bg)}`. `.dn-timeline-body{position:relative;flex:1;background:var(--dcs-bg-app);overflow:hidden}`. `.dn-tl-ruler{height:22px;background:var(--dcs-surface-1);border-bottom}`. `.dn-tl-tick{position:absolute;width:1px;background:var(--dcs-line)}`, span label `--dcs-fs-xs` `--dcs-text-mute` tabular-nums; `.dn-tl-tick--major{background:var(--dcs-line-strong)}`. `.dn-tl-tracks{flex:1}`. `.dn-tl-track{height:20px;border-bottom}`. `.dn-tl-track__label` absolute left s-3 `--dcs-fs-xs` `--dcs-text-dim`. `.dn-key{9×9px;translateX(-50%) rotate(45deg);background:var(--dcs-accent);border:1px solid #2a1c08;border-radius:1.5px}` (diamond); `.dn-key--sel{background:#fff}`. `.dn-playhead{width:0;z6;pointer-events:none}`, `::before` = 2px accent bar; `.dn-playhead__flag{top:0;left:-14px;28×15px;background:var(--dcs-accent);color:#0a1220;font-size:10px;font-weight:700;border-radius:0 0 2px 2px;tabular-nums}`. `.dn-tl-range{background:color-mix(in srgb,var(--dcs-accent) 9%,transparent)}`.

Fixed axis/gizmo colors used in JS (not from CSS vars): X `#d8475a`, Y `#6fb74a`, Z `#3f7ad0`; selection accent box `#e8943c`; light wire `#ffd060`/`0xffd060`; the app default accent orange is `#e8843a`.

---

## 8. ICON INVENTORY (complete list of `di di-*` names used)

Complete de-duplicated set: `arm, axes, bolt, camera, check, chevron-down, chevron-right, close, cog, cross-target, cube, decius, droplet, eq, export, eye, folder, folder-open, fullscreen, gizmo, globe, graph, grid, image, keyframe, layers, light, link, lock, magnet, mesh, minus, more-h, move, pen, pivot, play, pause, plus, record, render, rewind, rotate, scale, search, skip-back, skip-fwd, spline, texture, view-bbox, view-render, view-solid, view-tex, view-wire`.

Non-`di` icon/glyph elements: `.dcs-grip.dcs-grip--h` (drag handle), `.dcs-divider` (tool-rail separators), `.dcs-colorfield__chip`, `.dn-mat-preview`, SVG gizmo nubs (drawn programmatically), timeline diamonds (`.dn-key`, CSS).

---

**Load-bearing gotchas for the port:**
- `buildScene()` in app.js is a **dead no-op** (returns immediately); the entire SVG scene-drawing code below the `return` is unreachable. The real 3D is three.js in viewport.js. Don't port the SVG scene.
- The static Location combos in `#prop-object` show 0,0,0 but the live Cube root is at y=0.8; on select, app.js overwrites Inspector from `obj.root` values.
- `#menu-add` static HTML is discarded and rebuilt by app.js — port the runtime version (Cube/UV Sphere/Icosphere/Cylinder/Cone/Torus/Plane · Point Light/Sun/Spot · Camera · Empty), not the static Blender-style one.
- Rotation is stored in radians on the object, displayed in degrees in the Inspector (×180/π both ways).
- Selection box is object-oriented (rotates/scales with object), not world-AABB.
- Many toolbar toggles/menus are cosmetic (framework-only behavior); only the interactions in §5 marked REAL have app logic.
