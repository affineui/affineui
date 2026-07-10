# Embedding & resource provisioning — design

Scope of what AffineUI's embedding layer **wants to support**, the user
stories behind each capability, and the proposed interfaces. Status:
**now** = shipped, **easy** = small, do soon, **planned** = wanted, larger.
The shipped basics live in [`embed.h`](../include/affineui/embed.h) /
[`EMBEDDING.md`](EMBEDDING.md); this doc is the forward-looking spec.

## Guiding principles

1. **Simplicity favors *our* code, not the embedder's.** Interfaces are
   small, narrow, and cheap for AffineUI to implement and maintain. We are
   explicitly **not** trying to be all things to all people — when an
   embedder wants more ergonomics or engine-specific behavior, they write a
   wrapper around our primitive. We do not absorb that complexity.
2. **We own our resources; we borrow only the core API objects.** The host
   lends us the device + context (init) and render-target views (per frame)
   — that's it. For everything else (images, atlases, glyph atlases) the
   host hands us **bytes/pixels** (encoded, raw, or compressed DXT/BCn) and
   AffineUI uploads into its **own** textures.
   We **never store an engine object across frames** — the device + context
   are the one persistent exception (they define the session). An engine
   handle may be passed as a **transient parameter** to a call that uses it
   and returns (per-frame render-target views; or a *live* engine texture
   sampled that frame — §3.2); we use it during the call and retain nothing.
   sokol_gfx, NanoVG, the painter, display lists, and the compositor stay
   internal either way.
   *Rationale: stability* — not holding engine handles across frames means
   no dangling references and unambiguous lifetime. It can force some copies
   (e.g. duplicating an atlas into our own texture); that's an accepted tradeoff.
   **Allocator corollary (§6):** since we free with our *own* allocator, we
   must never take *ownership* of host-allocated memory or objects (freeing
   them with a different allocator would corrupt) — we copy host-provided
   bytes into our own allocations and only ever *borrow* host objects
   transiently.
3. **No new dependencies in the amalgamated `.cpp`.** No JSON/format
   parsers, no engine SDKs. POD / C-ABI-friendly structs the embedder fills.
4. **AffineUI owns its own world** (text/fonts, layout, paint). We don't try
   to share or interop with the engine's equivalents.
5. **Never crash on engine input.** Every host-provided pointer, handle,
   byte buffer, and callback result is validated at the boundary; null /
   empty / stale / mismatched input degrades gracefully (placeholder, skip,
   no-op + optional log) — AffineUI must never null-deref or dangling-deref
   on engine code or objects. Reinforced by principle 2 (we don't retain
   engine handles across frames, so they can't dangle later).
6. **No implicit I/O.** AffineUI never touches the filesystem or network on
   its own — every resource arrives through a host hook (VFS / asset
   resolver). That keeps it deterministic and **console / sandbox / cert
   friendly** (no direct disk or net access to audit).
7. **Main-thread only.** All AffineUI calls run on the host's UI/render
   thread unless a method is *explicitly* documented as thread-safe (only the
   async asset-fulfillment enqueue, §3.0). We spawn no internal threads.

When a feature would make the embedder's life easier but our code bigger or
more coupled, it goes in the embedder's wrapper, not in AffineUI.

**API shape:** each capability is a small, orthogonal piece — a free
function (e.g. `panel::dispatch_at`) or a `Ui` method — kept simple, with a
C ABI mirror. We expose simple calls and let embedders compose/wrap them; we
do not build a grand framework.

**Hooks vs data — the boundary convention:**
- Host **hooks** (allocator, VFS / asset resolver, clipboard, damage / stats
  callbacks) are **callbacks**: a `std::function` in C++, a function pointer
  `+ void* user` in the C ABI. They're registered at init and held for the
  session (like the device + context).
- Host **data** (bytes, strings, the POD structs / arrays — `AtlasInfo`,
  per-frame `ExternalTexture[]`, …) is **always copied into our own storage
  on receipt**. We never retain a host-owned data pointer past the call.

---

## 1. Ownership model — *now*

Standalone (AffineUI owns device + window + swapchain) vs embedded (host
owns them, all-or-nothing). See EMBEDDING.md.

---

## 2. Surfaces — "draw an HTML panel here"

### User stories
- **S1 screen-space HUD** — health/ammo/minimap, a rect over the 3D scene, every frame.
- **S2 full-screen menu** — pause/settings, opaque, whole target. *(now)*
- **S3 worldspace / diegetic panel** — UI on an in-world monitor/tablet/hologram, into a texture mapped on geometry, re-rendered when dirty.
- **S4 in-engine tools** — dockable inspector/console, frequent updates, keyboard focus.
- **S5 labels / nameplates / tooltips** — many small dynamic bits over world objects.
- **S6 engine content in UI** — a scene RT / video / icon atlas shown *inside* the HTML.

### 2.1 Full-surface render — *now*
`Ui::render(FrameTarget)` fills a target with the document. Covers S2.

### 2.2 Sub-rect overlay — *easy*
Add a viewport to `FrameTarget` so the document lays out at the panel size
and draws into a sub-region of the host's target. Covers S1/S4.

```cpp
struct FrameTarget {
    // ... existing ...
    struct { int x, y, w, h; } viewport{0,0,0,0};  // 0,0,0,0 = whole target
};
```
Implementation: after `sg_begin_pass`, `sg_apply_viewport(x,y,w,h)` and lay
out at `w×h` (NanoVG's `renderViewport` only sets the projection, so the
sokol viewport positions it). The pass clear is whole-RTV, so a sub-rect
overlay uses `clear = false` (draw over host content) and lets the panel's
own `body` background paint its backdrop.

### 2.3 Host-owned render-to-texture — *now (host-driven)*
Because `FrameTarget` takes render-target *views*, an offscreen panel needs
no new API: the host creates a texture (`RENDER_TARGET | SHADER_RESOURCE`),
passes its **RTV** in `FrameTarget`, renders, then samples its own **SRV**
as a quad anywhere (S3/S5). Transparent compositing today: set the Ui clear
color to alpha 0 (or `clear=false`). *Easy* follow-ups: confirm
premultiplied-alpha output and add a worked example + a resize note.

### 2.4 Input mapping — *easy*
Hosts already call `Ui::dispatch(Event)`. Add helpers that turn a
host-space coordinate into a panel-local event so each panel routes
correctly:
```cpp
namespace affineui::panel {
    bool dispatch_at(Ui&, const Event& e, Rect panel_px);     // screen-space
    bool dispatch_uv(Ui&, const Event& e, float u, float v);  // worldspace hit → UV
}
```

### Multiple panels
A "panel" is just a `Ui` instance (own document, state, target). The model
is N cheap `Ui`s, not one Ui with regions. (Confirm Ui instancing cost.)

### Color space & alpha
AffineUI's internal color system is **sRGB** — **not HDR**. The host's
target should be an sRGB (or sRGB-typed) format; AffineUI doesn't tone-map
or emit HDR/scRGB values. Output uses premultiplied alpha for over-the-scene
compositing (§2.2/§2.3). If a host needs AffineUI to composite in a specific
mode, that's a small explicit option; otherwise sRGB + premultiplied is the
contract.

---

## 3. Resource provisioning

### 3.0 Asset resolution — what does `"thing.png"` actually mean? — *planned (core)*
An embedder's `<img src="ui/thing.png">` rarely maps to literal PNG bytes
on disk. The **host** decides what the logical name resolves to:
- VFS bytes — an encoded file (PNG/JPG) that AffineUI decodes itself;
- raw or **compressed pixel data** (BCn/DXT/ASTC) the host hands over and
  AffineUI uploads into its **own** texture (no decode, but we own it);
- a **sprite** — a sub-rect of an atlas the host registered earlier
  (AffineUI owns that atlas texture too).

We hand back **data, not engine texture handles** (principle 2 — own our
resources for lifetime stability; an extra upload/copy is fine).

So the central hook is one **asset resolver** the host installs. It is
**asynchronous**: AffineUI hands the host a request + handle when it first
needs a URL; the host fulfills it now *or later* from its streaming / cook
pipeline. While pending, the element shows a placeholder; when fulfilled,
AffineUI repaints the affected nodes.

```cpp
struct AssetHandle { uint64_t id; };

// Installed by the host; called the first time AffineUI needs `url`.
// Fulfill synchronously inside the call, or keep the handle and fulfill
// later when the asset is resident.
using AssetResolver = std::function<void(std::string_view url, AssetHandle)>;

// Fulfillment — call exactly once per handle, on the UI/render thread.
// AffineUI copies/uploads what it needs; the caller keeps nothing alive.
void Ui::asset_bytes (AssetHandle, const void* data, size_t size);          // encoded → AffineUI decodes (stb_image)
void Ui::asset_pixels(AssetHandle, const void* data, size_t size,
                      int w, int h, PixelFormat, bool premultiplied);       // raw/compressed → AffineUI uploads to its own texture
void Ui::asset_sprite(AssetHandle, AtlasId, const char* sprite_name);       // sub-rect of an AffineUI-owned atlas (§3.3)
void Ui::asset_failed(AssetHandle);                                         // → broken-image placeholder
```

Semantics:
- **Cached by URL** — a second reference reuses the result; the resolver is
  called once per URL until invalidated.
- `Ui::invalidate_asset(url)` drops the entry and re-requests (hot-reload,
  streamed-LOD swap).
- **Pending** elements render a placeholder (default transparent, configurable).
- **Threading** — fulfillment runs on the UI thread; a worker that finishes
  off-thread marshals via a thread-safe enqueue AffineUI drains at frame
  start. *(exact enqueue API: open)*

This is the abstraction engines need: cooked assets, not loose PNGs, while
AffineUI still owns every GPU resource. **Dependency:** the `pixels` shape
with a *compressed* format needs AffineUI to create an image from
compressed pixel data — a small addition in the **affineui_nanovg** fork
(create a NanoVG image over an AffineUI-owned sokol image of any pixel
format). The encoded `bytes` (RGBA) path works without it.

### 3.1 Virtual file system / resource provider — *partially now*
Already: `ResourceLoader = std::function<std::string(url)>` (set via
`InitDesc::resource_loader` or `Ui::load`). A pull-model VFS: AffineUI asks
for a URL, the host returns the bytes. **All** document/resource fetches
route through it — HTML, `<link>`/`@import` CSS, `<img>`, and the asset URL
schemes below. (Fonts are **not** host-provided — see §3.4.)

*Easy follow-ups:*
- C ABI callback: `bytes = cb(url, user, &len)` + a `free` convention.
- Documented scheme conventions (`app://`, `asset://`, `data:`…).
- Optional: an `exists(url)` probe and a content-type hint for ambiguous bytes.

### 3.4 Fonts — AffineUI-owned, *not shared* (decided)
AffineUI keeps its **own** text/font stack (fontstash + stb_truetype, with
an embedded default — see [[embedded-fonts]]). We deliberately **do not**
share fonts or the font system with the host engine, and we do not consume
the engine's font atlases — cross-engine font/shaping interop is more pain
than value. An app may register additional font *files* through AffineUI's
own font API (a core feature, byte-loadable via the VFS), but that is
AffineUI loading its own fonts, **not** an embedding/interop surface. The
host's text renderer and AffineUI's stay separate by design.

### 3.2 Live engine texture — the narrow, transient exception — *planned*
The one case that can't be a copy: showing a *live* engine texture inside
the UI (S6 — scene RT, video). Per principle 2 we don't **store** the
handle; instead the host supplies a **per-frame binding** in `FrameTarget`
that AffineUI uses *during that render and forgets*:
```cpp
struct ExternalTexture { const char* name; const void* native_tex;
                         int w, int h; PixelFormat fmt; bool premultiplied; };
// FrameTarget gains, for this frame only:
//   const ExternalTexture* external; int external_count;
// Markup references it by name: <img src="live://scene-rt">.
```
The handle is read inside `render()` and never retained; if a name isn't
bound this frame (or the handle is null) the element shows its placeholder
(principle 5). No registration, nothing stored across frames.

### 3.3 Atlas textures + atlas info — *planned*
Most engines ship icons/sprites as **one sheet + a table of named
sub-regions**. The host hands over the sheet's **pixel data** (raw or
compressed) once; AffineUI uploads it into its **own** texture and draws
sprites as sub-rects via a NanoVG image pattern (no per-sprite texture).
```cpp
struct AtlasEntry {
    const char* name;          // "icons/sword"
    Rect        rect;          // pixel rect within the sheet
    // optional: nine-slice insets (l,t,r,b), rotated flag
};
struct AtlasInfo {
    const AtlasEntry* entries; int count;
    int atlas_width, atlas_height;
};
// Pixel data, not a handle → AffineUI owns the resulting texture.
AtlasId Ui::register_atlas(const void* pixels, size_t size,
                           PixelFormat fmt, const AtlasInfo&);
void    Ui::unregister_atlas(AtlasId);
// Referenced as <img src="atlas://sheet/icons/sword"> or
//   background-image: url(atlas://sheet/icons/sword);
```
**Decided:** `AtlasEntry` / `AtlasInfo` are **POD** the embedder fills in;
AffineUI only *reads* them and never parses a file format. How the embedder
populates them is entirely their concern — their own JSON/TOML parser, an
asset-cook step, codegen, or plain struct literals. This keeps any parser
dependency out of the amalgamated `.cpp` and out of AffineUI's scope; the
engine already has the sprite table (it built the atlas) and just hands the
POD over (lifetime: AffineUI copies what it needs at `register_atlas`).
Nine-slice insets map naturally onto CSS `border-image`.

---

## 4. Input & platform services

Embedded input is **different in kind** from the sokol/SDL adapters. Those
are *complete platforms* — window + event loop + GPU + cursor + clipboard +
IME. An engine host is **only an input source**: it forwards input and,
optionally, services a few UI→host intents. It is *not* a platform and
shouldn't be made to act like one.

The core is already neutral (`affineui::Event` + `Ui::dispatch`), so the
right model is three tiers over one core:

| Tier | Provides | Used by |
|---|---|---|
| **Core (neutral)** | `Event` in via `dispatch`; intent outputs (cursor, text-input/caret, clipboard requests) | everyone |
| **Input source (embedded)** | translate raw input → neutral `Event` (panel-local); optionally service intents | game engines |
| **Platform adapter (sokol/SDL)** | the input-source tier **plus** full intent servicing *via the platform* | standalone / simple hosts |

So the adapters are a **superset** — embedding reuses the same core and the
host wears only the "input source" hat. We should factor the back-channel
so the adapters and an embedder implement the *same* optional hooks; the
adapters just happen to back them with a real platform.

### In — events
- Host builds an `affineui::Event` and calls `Ui::dispatch(e)` (or panel
  helpers `panel::dispatch_at` / `dispatch_uv`). The return says whether the
  UI consumed it, so the host can gate its own game input.
- **Coordinates are panel-local CSS points** (with the panel's `dpi_scale`),
  not window pixels — there may be no window at all (worldspace panel). The
  screen-rect / UV helpers do the mapping.
- Keyboard/text go to the **focused panel**; *which* panel has focus is the
  host's call (cross-panel). Focus *within* a panel is AffineUI's.
- **Gamepad / controller** is an input *source* too (console UIs). Preferred
  shape: the host maps sticks/dpad/buttons to neutral **navigation intents**
  (move focus ↑↓←→, activate, back) and dispatches those; raw button/axis
  forwarding is a fallback. It drives the same focus model as keyboard Tab.

> **Out of scope here:** *scrolling, focus, and tab-order* are part of the
> **standard AffineUI API**, not embedding. Embedding only *delivers* the
> input (wheel, Tab, gamepad nav) that drives them; their behavior lives in
> core. This section is just about getting that input in (and intents out).

### Out — intents (UI → host), all optional
The host wires only what it cares about; unset = that feature is simply off.
- **Cursor** — `Ui::hovered_cursor()` (exists) → host sets its OS cursor.
- **Text input / IME** — `Ui::text_input_active()` + `Ui::caret_rect()`
  (**implemented**) so the host enables the platform IME, positions the
  candidate window, or raises the mobile soft keyboard. AffineUI can't
  open IME itself.
- **Clipboard** — copy/paste need the host: `clipboard_get` / `clipboard_set`
  hooks the host supplies; AffineUI calls them on Ctrl+C/V. Adapters wire
  these to sapp/SDL; embedders wire them to the engine (or leave unset).

### IME — the careful bit
**Implemented** (see `docs/IME_ARCHITECTURE.md` for the full design and
per-platform status): committed text arrives as `EventType::TextInput`,
preedit as `EventType::Composition` (rendered inline at the caret with
the conventional underlines, never entering the control's value), and the
text-input-active + caret-rect intents let the host steer the platform
IME. The SDL adapter is the reference host wiring.

### Queries — hit-test / click-through
The host needs to gate its own input *without* dispatching: "is this screen
point over opaque UI?" A pure query (no side effects) answers it —
`bool Ui::hit_test(Point panel_px)` (plus `panel::hit_test_at` / `_uv` for
screen / worldspace mapping). Lets the host decide click-through, cursor
ownership, and whether to forward an event to the game at all.

### Diagnostics / logging — host hook
AffineUI's warnings/errors **and** HTML/CSS parse diagnostics route to an
optional host **log callback** (severity + message), not `stderr`. Unset =
silent (or a built-in stderr default in dev). This is how a "never crash"
degrade (principle 5) surfaces to the developer, and it keeps engine logs in
one place. C ABI: fn-ptr `+ void* user`.

### Why not just reuse the SDL/sokol input path?
Those translate a *specific platform's* events and assume a window plus the
ability to call platform services directly. An engine has neither — its
"platform" *is* the engine. A neutral `Event` + optional intents lets one
core serve a sub-rect HUD, a worldspace panel (UV input, no window), and a
full SDL app, without any of them knowing about the others.

---

## 5. Update scheduling & damage

AffineUI already tracks **dirty rectangles internally** (layout/paint
invalidation is region-tracked). Embedding turns that into a host-facing
contract, two ways:

1. **Tell the host when a panel needs repaint.** A cached / worldspace RTT
   panel must not re-render every frame — the host repaints it only when the
   UI changed. Expose:
   - `bool Ui::needs_update() const;` — true if anything changed since the
     last render (the host's "should I call render?" check).
   - `void Ui::mark_dirty();` — force `needs_update()` true, e.g. when host
     state the UI depends on changed outside AffineUI's knowledge.
   - `Rect Ui::damage() const;` — union of dirty regions (panel-local),
     empty when clean. (List-of-rects is a possible refinement.)
   - optional `Ui::on_damage(cb)` — fired when a *clean* panel goes dirty, so
     an idle host can wake / schedule a repaint instead of polling.
2. **Repaint only what changed.** AffineUI can scissor its paint to the
   damage rect; for an RTT panel the host can then do a partial texture
   update of just that region. (`render(FrameTarget)` clears+repaints the
   whole target today; the damage-aware path needs `clear=false` + scissor
   so the rest of the texture is preserved.)

Sources of dirt: DOM/content mutation, imm re-eval, style/animation ticks,
hover/focus changes, an async asset arriving (§3.0), viewport resize.

**Who ticks:** the host either calls `render` every frame, or only when
`needs_update()`. An in-flight animation marks the Ui dirty each tick so an
"only when dirty" host keeps rendering until it settles, then goes idle.

**Host-driven time.** Animations, transitions, and timers advance off a
**host-supplied delta/clock** (passed to `render`/`tick`), never the wall
clock. The engine owns time — so pause, slow-mo, and fixed-timestep
determinism all work, and a paused game freezes the UI's animations too.

**Cheap no-op frames.** The host may call `render` unconditionally every
frame; when there's nothing to do — clean state, or (in the future
compositing-layer mode) nothing to recomposite — we **early-return** without
touching the GPU. Calling us with no work is free, so the host never has to
guard the call with its own dirty check (though it can use `needs_update()`
to skip even that).

Open: damage granularity (single union rect vs list); panel-local px vs points.

---

## 6. Memory, allocators & accounting — *allocator: must-have capability*

Supporting a host allocator is a **must-have capability** — serious engine
use requires it and most engines will set one (budget tracking, alignment,
no global `malloc`/`new`). **Setting it is optional**, though: small projects
and most casual use leave it null and get the default `malloc`/`new`. When a
host *does* supply one, coverage must be **total** — every byte AffineUI
allocates routes through it. AffineUI takes one allocator at init:

```cpp
struct Allocator {
    void* (*alloc)  (size_t size, size_t align, void* user);
    void* (*realloc)(void* p, size_t old_sz, size_t new_sz, size_t align, void* user);
    void  (*free)   (void* p, void* user);
    void* user;
};
// InitDesc::allocator   (null = default malloc/new)
```

**The amalgamation helps a lot here.** We ship a single `.cpp` TU and own
the impl TUs (`stb_impl.c`, `fontstash_impl.c`, `sokol_impl.c`,
`nanovg_sokol.c`) plus the nanovg/lexbor forks — so the C-library hooks are
just a block of `#define`s / setup calls placed **once** at the top of the
amalgamated TU (and mirrored in the modular impl TUs), not a scatter of
per-dependency forks:
- **sokol_gfx** — `sg_desc.allocator` (runtime). ✓
- **stb_image / stb_truetype** — `STBI_MALLOC` / `STBTT_malloc` macros. ✓
- **fontstash** — `FONS_MALLOC` macros. ✓
- **lexbor** — `lexbor_memory_setup(...)` (runtime). ✓
- **NanoVG core** — a couple of direct `malloc`s → tiny hook in the
  affineui_nanovg fork (we own it). ✓

The residual is AffineUI's own `std` (`string`/`vector`/`function`) and
**Yoga** (C++ `new`) — not macro-hookable. Because coverage must be
**total**, we commit to one of:
- **Primary — replace global `operator new`/`delete`** in the amalgamated TU,
  routed to the host allocator. In a single-TU SDK compiled into the engine
  this is the reliable blanket guarantee — it captures std, Yoga, and
  everything else with no per-type work.
- **Alternative — `std::pmr`** for AffineUI's own containers over a host-pool
  `memory_resource`, plus a Yoga allocator shim — no global replacement, but
  a broader refactor — for hosts that forbid replacing global operators.

Default = global override (simplest total coverage); pmr as the escape hatch.

Open: one allocator vs persistent + per-frame-scratch split; alignment
guarantees; global-override default vs pmr default.

### Resource accounting & callbacks — *wanted*
Engines want to see what AffineUI consumes (budget, leak-hunting, HUD).
Because we own our resources, the numbers are exact:
- **Stats query** — `Ui::stats()`, cheap to poll:
```cpp
struct Stats {
    int    textures, images, shaders, buffers;
    size_t cpu_bytes;   // live bytes through the host allocator
    size_t gpu_bytes;   // estimated GPU memory (textures + buffers)
};
```
- **Callbacks** (optional) — `on_gpu_resource(create|destroy, kind, bytes)`
  so the engine tracks GPU resources in real time / attributes them to its
  budget. CPU allocation the engine already sees (it *is* the allocator).

---

## 7. Reset & lifecycle — *wanted*

Engines reuse objects across level loads / hot-reloads and want a
deterministic way to wipe a Ui without tearing down the embedding session.

- `Ui::reset()` — **drop all state and start over**: clear the DOM, styles,
  layout, imm state, click handlers, the asset cache + registered atlases,
  and all GPU resources we created — back to the just-constructed state, but
  keeping the device/context binding and the allocator so the Ui is
  immediately reusable. (Equivalent to destroy+recreate minus the GPU
  bring-up.) All released memory returns to the host pool.
- Relationship to existing teardown: `~Ui` / `renderer().shutdown()` release
  everything *including* sokol_gfx; `reset()` keeps the session live.
- Lifecycle edges to define: device-lost / context-recreate (re-init GPU
  resources against a new device) and swapchain/target resize.

## 8. What internals to expose

| Object | Expose? | Notes |
|---|---|---|
| Device handles | **in, at init** | host → AffineUI (all-or-nothing) |
| Render-target views | **in, per frame** | host → AffineUI |
| Texture handles (panel SRV out; host texture/atlas in) | **yes, both ways** | §2.3, §3.2, §3.3 |
| Resource bytes (VFS) | **in, on demand** | §3.1 |
| Font system / fonts | **no (by design)** | AffineUI owns text; not shared with host (§3.4) |
| `sg_*` sokol context | **no** | keep the backend abstraction sealed ([[gpu-via-sokol-gfx-only]]) |
| `NVGcontext*` | **opt-in escape hatch** | advanced custom vector drawing into the same canvas; not stable API |
| Painter / DisplayList / Layer / Compositor | **no** | private; rasterizer must stay swappable |

---

## 9. Capability summary — *what we want to support*

| Capability | Story | Status | Interface |
|---|---|---|---|
| Standalone app | — | now | App / run |
| Embedded init (host device) | all | now | `Ui::init(InitDesc{gpu})` |
| Full-surface render | S2 | now | `Ui::render(FrameTarget)` |
| Host-owned render-to-texture | S3, S5 | now (host-driven) | `FrameTarget` → offscreen RTV |
| Sub-rect overlay | S1, S4 | easy | `FrameTarget::viewport` |
| Transparent / premultiplied compositing | S1, S3 | easy | clear alpha / blend |
| Panel input mapping | S1, S3, S4 | easy | `panel::dispatch_at` / `dispatch_uv` |
| Event dispatch (pointer/key/text) | input | now | `Ui::dispatch(Event)` |
| Gamepad / navigation intents | console UIs | planned | nav events → focus model |
| Hit-test / click-through query | input gating | easy | `Ui::hit_test` / `panel::hit_test_*` |
| Cursor intent (out) | input | now | `Ui::hovered_cursor()` |
| Text-input + caret intent (out) | IME | now | `text_input_active()` + `caret_rect()` + `Composition` event |
| Clipboard hooks | copy/paste | planned | `clipboard_get` / `clipboard_set` |
| Host-driven time/clock | animation/determinism | planned | `dt` to `render`/`tick` |
| Logging / diagnostics hook | dev / engine logs | easy | host log callback |
| Color space (sRGB, premultiplied) | compositing | now (sRGB only) | target format contract |
| Update/dirty query + force | S3, S5 | easy (expose) | `Ui::needs_update()` / `mark_dirty()` / `damage()` |
| Damage callback (idle hosts) | S3 | easy | `Ui::on_damage(cb)` |
| Partial / scissored repaint | S3 | planned | `clear=false` + scissor |
| Asset resolver (`url` → bytes/texture/sprite) | all images | planned (core) | `ImageResolver` |
| Virtual file system (bytes shape) | all | partial now | `ResourceLoader` (+ C ABI) |
| External texture in UI (texture shape) | S6 | planned | `register_texture` / resolver + `tex://` |
| Atlas texture + info (sprite shape) | S5, S6 | planned | `register_atlas(tex, AtlasInfo)` (C/C++ struct, no file parsing) + `atlas://` |
| Wrap external GPU texture as NanoVG image | S6 | planned (fork dep) | affineui_nanovg `nvgCreateImageFromHandle` |
| Custom allocator / host pool | serious engine use | must-have capability (optional to set) | `InitDesc::allocator` — total routing when set (§6) |
| Resource accounting + callbacks | budget / HUD | wanted | `Ui::stats()` + `on_gpu_resource` |
| Full reset (drop all state) | level load / hot-reload | wanted | `Ui::reset()` |
| Device-lost / resize lifecycle | robustness | planned | re-init GPU on new device; target resize |
| C ABI for all of the above | all | partial | `c_api.h` |
| `NVGcontext` escape hatch | advanced | planned (opt-in) | accessor on `Renderer` |

---

## 10. Open questions
- Premultiplied vs straight alpha for RTT panels and resolved textures — pick one, document the host blend state.
- ~~Atlas info format~~ — decided: plain C++/C struct only, no file parsing / no parser dependency (§3.3).
- `ImageId`/`AtlasId`/resolved-texture lifetime + thread/frame rules when the host frees a texture mid-flight (resolver hands out a borrow — when may it be revoked?).
- Async resolver decided: request/fulfill with handles, cached by URL, `invalidate_asset` to refresh. Remaining: exact **thread-safe fulfill/enqueue** API for worker threads; configurable **placeholder** (transparent vs spinner vs last-good).
- Do **text resources** (HTML/CSS, `@import`, `<link>`) also go through the async resolver, or stay on the synchronous byte loader? (Async images, sync text in v1?) — fonts excluded (§3.4).
- affineui_nanovg: add `nvgCreateImageFromHandle` (external sokol image / native texture, incl. compressed formats) — gates the texture/sprite shapes.
- Should sub-rect clear support a scissor-limited clear, or is `clear=false` enough?
- Hot-reload via the VFS/resolver (invalidate + re-fetch on host signal)?
