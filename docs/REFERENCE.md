# AffineUI API Reference

The complete public C++ API surface, organized by layer. Task-oriented
introduction: [USERS_GUIDE.md](USERS_GUIDE.md).

AffineUI is primarily a **UI framework** — a peer of Qt and Dear ImGui —
built on an efficient, conformant **HTML5 renderer** that is independently
usable. The two layers sit over a shared base, and this reference keeps
them separate:

- **[The application shell](#part-1--application-shell)** — entry points,
  `App`, windowing. Hosts either layer.
- **[The component framework](#part-2--the-component-framework)** — `View`,
  widgets, docking, themes. Emits DOM into the renderer; you build with
  typed widgets, mixing in raw HTML fragments where convenient.
- **[The HTML5 renderer](#part-3--the-html5-renderer)** — `.html` documents,
  CSS, the `Document`, painting. Web content in, pixels out; no widgets
  here.
- **[Core facilities](#part-4--core-facilities)** — value types, crash-safe
  callbacks, reflection, logging, memory. Used by both layers.
- **[Embedding & integration](#part-5--embedding--integration)** — host-owned
  GPU, adapters, the C ABI, devtools/telemetry.

Conventions:

- Everything lives in `namespace affineui` unless noted.
- **Single-threaded.** Construct, build, dispatch, and render on the thread
  that owns the graphics context.
- **Hard to crash.** Invalidating handle types (`WidgetRef`, `AppHandle`,
  `ImageHandle`, `DomHandle`, `WeakRef`) resolve to defaults on read and no-op
  on write after their target dies. Retained callbacks receive the same
  guarantee when created with `bind()` or `guard()`; an arbitrary capturing
  `std::function` still carries the lifetime contract chosen by its author.
- `LOC` in signatures stands for the trailing
  `std::source_location here = std::source_location::current()` parameter
  every `View` builder takes (records the creating call site for devtools).
- Status is **alpha**; items marked *(experimental)* or *(stub)* are called
  out inline.

---

# Part 1 — Application shell

## Entry points (`affineui/affineui.h`)

The umbrella header; includes the whole public API.

| Signature | Description |
|---|---|
| `int run(std::string_view html)` | Open a window, render an HTML document, run to exit. The simplest renderer-layer program. |
| `int run(std::function<void()> view_fn)` | Run an immediate-mode app (see [imm](#immediate-mode-affineuiimm)). |
| `int run(App::Config cfg)` / `run(App::Config, std::function<void()>)` | As above with explicit config. |

Opt-in/opt-out macros, defined before including: `AFFINEUI_NO_IMM`,
`AFFINEUI_NO_C_API`, `AFFINEUI_WITH_SOKOL`, `AFFINEUI_WITH_SDL`.

## `App` and `App::Config` (`affineui/app.h`)

`App` owns the window, the main loop, and a retained `Document` (which lives
as long as the App). Non-copyable, movable.

### `App::Config`

| Field | Default | Notes |
|---|---|---|
| `std::string title` | `"AffineUI"` | Window title. |
| `int width`, `int height` | `1024`, `768` | Logical window size. |
| `Color clear_color` | `{30, 30, 46, 255}` | Background behind the document. |
| `bool high_dpi`, `bool vsync` | `true`, `true` | |
| `std::string default_font_family` | `"sans-serif"` | |
| `int default_font_size` | `16` | |
| `std::vector<std::string> asset_folders` | `{"."}` | Directories probed for relative resource URLs. |
| `ResourceLoader resource_loader` | `{}` | Custom URL → bytes hook. |
| `bool perf_overlay` | `false` | Native perf overlay (see Part 5). |
| `DebugOverlayCorner perf_overlay_corner` | `top_right` | |
| `bool devtools_hotkey` | `true` | F12 / Ctrl+Shift+I opens the affinetools viewer. Compiled out with `AFFINEUI_PERF=0`. |
| `bool native_menus` | `true` | The menu set with `App::set_menu` becomes the macOS system menu bar, and the drawn menubar hides its triggers. ON by default: with no menu bar there is no Quit item, so Cmd-Q could not work. Set false to keep the drawn bar. |
| `TitleBarStyle titlebar` | `Default` | `Hidden` / `HiddenInset` remove the system title bar (OS window buttons stay); `Frameless` removes the buttons too and the app draws its own. Named as in Electron. The app must mark its drag region — see `docs/WINDOW_CHROME_AND_MENUS.md`. |
| `Point traffic_light_position` | `{0,0}` | macOS: where the window buttons sit, from the window's top-left. Zero = the style's default. |
| `std::function<void()> on_layout_changed` | `{}` | Fired when an interaction changed the dock layout — read and persist it. |
| `bool no_bundle_decius` | `false` | The embedded Decius bundle is ON by default; `true` disables it at runtime. |

### Content

| Signature | Description |
|---|---|
| `void load_html(std::string_view html)` | Load an HTML string (renderer layer). |
| `bool load_html_file(std::string_view path)` | Load an `.html` file; linked resources resolve relative to it. |
| `void set_stylesheet(std::string_view css)` | Install/replace the user stylesheet (above document styles in the cascade). |
| `void set_stylesheet(std::string_view css, std::string_view base_url)` | Same; `base_url` makes the sheet's relative `url()`s resolve like a `<link>`ed sheet. |
| `void load_view(const View& view)` | Inflate a component-framework view once (serialize + parse; fine for one-shot loads). |
| `void set_view(std::function<void(View&)> builder)` | Install a persistent view builder; each `rebuild_view()` reconciles into the live document — the fast path, no HTML reparse. |
| `void rebuild_view()` | Re-run the installed builder and reconcile. |
| `void mount(std::function<void()> view_fn)` | Install an immediate-mode view function. |

`App::handle()` returns an `AppHandle`: a copyable, non-owning capability for
retained controllers. `is_valid()` becomes false after App destruction;
custom-paint, repaint, image creation, geometry lookup, and pointer-capture
operations then safely no-op. It never exposes an App, Document, Renderer, or
Painter pointer.

### Loop, events, frames

| Signature | Description |
|---|---|
| `int run()` / `int run(std::function<void()> view_fn)` | Start the main loop; returns the OS exit code. |
| `void quit(int code = 0)` | Clean exit after the current frame. Does NOT run the close-request handler — quit means quit. |
| `void on_close_request(std::function<bool()> cb)` | The veto point for an inbound close (window button, Cmd-Q, menu Quit, `close()`). Return false to cancel. This is what makes save-on-exit possible. |
| `void set_menu(Menu menu)` | The application menu, platform-neutral. On macOS becomes the system menu bar. Re-set it as its checked/enabled state changes. See `docs/WINDOW_CHROME_AND_MENUS.md`. |
| `void close()` | Ask to close — runs the close-request handler, so it is cancellable. |
| `void minimize()` / `void toggle_maximize()` / `bool is_maximized() const` | What an app-drawn title bar's buttons call. |
| `void set_fullscreen(bool)` / `bool is_fullscreen() const` | |
| `bool dispatch(const Event& ev)` | Route a translated input event; true when consumed. |
| `void on_event_capture(EventHandler cb)` | Capture phase before document/widgets for every dispatched mouse, key, text, or composition event. Return true to consume, e.g. force Ctrl/Cmd+Z into the app-global undo stack or let a modal host tool own pointer input. The supplied hover chain is the current pre-dispatch chain. |
| `void on_event(EventHandler cb)` | Post-document low-level handler; receives the refreshed hit-tested hover chain, deepest first. Focused-editor commands consumed by the document do not reach it. `EventHandler = std::function<bool(const Event&, const std::vector<Document::HoverInfo>&)>`. |
| `void on_frame(std::function<void(double dt_seconds)> cb)` | Per-frame tick (requestAnimationFrame analog). Call `invalidate()` inside to schedule a repaint; do nothing to stay idle. |
| `void invalidate()` | Force re-evaluation/repaint before the next frame. |
| `void capture_pointer()` / `release_pointer()` / `bool pointer_captured() const` | While captured, MouseMove reaches `on_event` handlers before DOM hover hit-testing. |

### Custom paint

| Signature | Description |
|---|---|
| `void set_custom_paint(std::string_view name, Document::CustomPaintFn fn)` | Register a paint handler for elements with `data-aui-paint="name"`. Empty fn removes it. |
| `void request_custom_repaint(std::string_view name)` | Repaint matching elements next frame — no restyle, no layout, no reconcile. |

### Host-pulse embedding

For hosts that own the frame loop: dispatch **all** input each pulse, then
`if (app.should_render()) app.render();`.

| Signature | Description |
|---|---|
| `void set_min_frame_time(double ms)` / `double min_frame_time() const` | Render throttle; default `0` (unthrottled). |
| `bool should_render()` | True iff dirty-or-animating AND the throttle elapsed. Does **not** detect host swapchain resizes — dispatch a `Resize` event yourself. |
| `void render()` | Paint one frame. *(stub — currently being extracted; standalone `run()` is unaffected)* |

### Introspection

| Signature | Description |
|---|---|
| `Document& document()` | The retained document (lifetime = the App). |
| `Size window_size() const` / `Size framebuffer_size() const` | Logical vs physical pixels. |
| `float dpi_scale() const` | 1.0 = standard, 2.0 = Retina-class. |
| `const FrameTelemetry& frame_telemetry() const noexcept` | Last presented frame's telemetry (zeroed with `AFFINEUI_PERF=0`). |
| `bool perf_overlay_enabled() const` / `void set_perf_overlay_enabled(bool)` / `void set_perf_overlay_corner(DebugOverlayCorner)` | Perf overlay control. |

---

# Part 2 — The component framework

Headers: `affineui/view.h`, `affineui/components.h`, `affineui/inspector.h`,
`affineui/decius_bundle.h`.

## The model

`View` is a **reconciler**: re-run a build pass (`begin()` → builders →
`end()`) and it diffs against the previous pass, emitting minimal DOM
operations through a `ViewSink`. Attribute writes inside a pass are
coalesced — only the net end-of-build change emits; an unchanged tree emits
zero operations. Nodes match across passes by **key** (the trailing `key`
parameter on every builder).

### Types

| Type | Description |
|---|---|
| `enum class ViewTheme { Plain, Bootstrap, Decius }` | The CSS-framework *personality* mapping abstract builder intent to concrete class names. **Decius is the default** (its CSS + fonts are embedded in the library). Some widgets degrade per personality: `toggle()` → checkbox off-Decius, `colorfield()` → native color input off-Decius. |
| `struct FrameworkVersion` | Parsed, comparable framework version; `static FrameworkVersion parse(std::string_view) noexcept` (lenient). |
| `enum class WidgetKind` | `Root, Container, Text, RawHtml, Heading, Panel, Button, Checkbox, Slider, Knob, TextInput, TextArea, Dropdown, ButtonGroup, VirtualList, Card` — the shape recorded on each node. |
| `struct StableId` | Stable node identity across reconciliation. |
| `struct WidgetNode` | One tree node (tag, name, attrs, children, source location). Reconcile fields (`cursor`, `attrs_before`, …) are public for debug inspectors only — treat as internal. |
| `enum class ComponentValidity { Valid, WrongType, NotPresent }` | Typed-wrapper validity (see components). |
| `struct VirtualListOptions` | `item_count, first_item, visible_items{16}, overscan{2}, item_size{24.0}, item_sizes` — windowed-list config. |
| `struct TreeRowOptions` | `depth, selected, icon, expandable, expanded{true}, draggable{true}, meta_icon` — tree-row presentation; the component generates the canonical chevron/icon/label/meta substructure. |
| `struct FloatingToolbarOptions` | `vertical{true}, small{true}, drag_bounds, position` — floating tool rail. |

Framework-constant namespaces: `decius::` and `bootstrap::` carry
`default_version`, selector-attribute names, token values, and emitted
class names. The main knobs across all selectors are **`density`**
(`compact`/`comfortable`/`spacious`) and **`accent`** (the key color);
`size`, `style`, `radius`, and `dark` refine from there. Free functions
`framework_bundle_href(theme, version = {})` and
`framework_default_version(theme)` expose the CSS bundle a theme targets.

## `View` — lifecycle & configuration

| Signature | Description |
|---|---|
| `explicit View(ViewTheme theme = ViewTheme::Decius)` | A bare `View` + `load_view` is fully styled — zero configuration. |
| `void begin(ViewSink* sink = nullptr)` / `void begin(RemotePatchQueue*)` | Start a build/reconcile pass (local or remote sink). |
| `void end()` | Finish the pass; flushes the coalesced attribute diff. |
| `void clear()` | Reset the tree. |
| `ViewTheme theme() const` / `void set_theme(ViewTheme)` | Personality get/set. |
| `View& selector(std::string_view name, std::string_view value)` | Document-level personality selector (`"density"`, `"dark"`, …). |
| `void set_framework_version(std::string_view)` / `std::string_view framework_version() const` | Pin the CSS framework version this view targets (stamped as `data-aui-framework-version`). |
| `WidgetRef find_widget(std::string_view name)` | Look up a live widget by key. |
| `template <typename T> T component(std::string_view name)` | Typed lookup — always safe (see [Typed components](#typed-components-affineuicomponentsh)). |
| `std::vector<WidgetClickBinding> click_bindings() const` / `change_bindings()` | All registered handlers by name. |
| `std::string to_html_fragment() const` / `to_html_document() const` | Serialize the tree as HTML. |
| `std::string to_html_shell() const` | Document with an **empty** `<main>` — the reconcile bootstrap (load shell once, replay the tree through the sink; no HTML reparse ever again). |
| `const std::vector<std::string>& diagnostics() const` / `void clear_diagnostics()` | Build diagnostics (e.g. component type mismatches). |

## Builders

All builders take a trailing `LOC` parameter (omitted below) and an
optional `key` for identity/lookup.

### Text & content

| Signature |
|---|
| `WidgetRef heading(int level, std::string_view text, std::string_view classes = {}, std::string_view key = {})` |
| `WidgetRef paragraph(std::string_view text, std::string_view classes = {}, std::string_view key = {})` |
| `WidgetRef text(std::string_view text, std::string_view key = {})` |
| `WidgetRef html(std::string_view markup, std::string_view key = {})` — trusted raw-HTML fragment inside a component tree |

### Buttons & inputs

| Signature | Notes |
|---|---|
| `WidgetRef button(std::string_view label, bool primary = false, std::string_view key = {})` | |
| `WidgetRef icon_button(std::string_view icon, std::string_view key = {})` | Icon-only ghost button; `icon` is a framework glyph name (`"save"`, `"bolt"`). |
| `WidgetRef checkbox(std::string_view label, bool checked, std::string_view key = {})` | |
| `WidgetRef toggle(std::string_view label, bool on, std::string_view key = {})` | Switch; checkbox semantics (`aria-checked`). |
| `WidgetRef input(std::string_view label, std::string_view value, std::string_view type = "text", std::string_view key = {})` | |
| `WidgetRef password(std::string_view label, std::string_view value, std::string_view key = {})` | |
| `WidgetRef textarea(std::string_view label, std::string_view value, int rows = 3, std::string_view key = {})` | |
| `WidgetRef dropdown(std::string_view label, const std::vector<std::string>& options, std::string_view selected, std::string_view key = {})` | |
| `WidgetRef button_group(std::string_view label, const std::vector<std::string>& options, std::string_view selected, std::string_view key = {})` | Segmented options. |
| `WidgetRef slider(std::string_view label, double value, double min = 0.0, double max = 1.0, std::string_view key = {})` | |
| `WidgetRef knob(std::string_view label, double value, double min = 0.0, double max = 1.0, bool bipolar = false, std::string_view key = {})` | Rotary; `bipolar` = centered zero. |
| `WidgetRef combo(std::string_view label, double value, double step = 0.01, std::string_view key = {})` | Bare drag-scrub numeric (no field wrapper); `label` is a short axis tag. |
| `WidgetRef vec(std::string_view label, const std::vector<std::string>& channels, const std::vector<double>& values = {}, std::string_view key = {})` | Row of 2–4 numeric channels (`{"X","Y","Z"}`). |
| `WidgetRef colorfield(std::string_view label, std::string_view value, std::string_view key = {})` | Chip + editable hex + picker popover (chip drag-scrubs H/S/V). |
| `WidgetRef color_field(std::string_view label, std::string_view value, const std::vector<std::string>& swatches, std::string_view key = {})` | Swatch-popup variant. |

### Containers (return `View::Scope`)

| Signature | Notes |
|---|---|
| `Scope container(std::string_view classes = {}, std::string_view key = {})` | Generic div. |
| `Scope element(std::string_view tag, std::string_view classes = {}, std::string_view key = {})` | Arbitrary tag. |
| `Scope panel(std::string_view key = {})` / `Scope card(std::string_view title, std::string_view classes = {}, std::string_view key = {})` | Themed panel / titled card. |
| `Scope foldout(std::string_view title, bool expanded = true, std::string_view key = {})` | Collapsible section; header click toggles. |
| `Scope tree(std::string_view key = {})` | **Deprecated** — use `virtual_tree`. |
| `WidgetRef tree_row(std::string_view label, const TreeRowOptions&, std::string_view key = {})` | **Deprecated** — use `virtual_tree`. |
| `WidgetRef virtual_list(std::string_view key, const VirtualListOptions&, const std::function<void(View&, std::size_t)>& build_item, std::string_view classes = {})` | **Deprecated** eager form — use the provider overload below. |
| `WidgetRef virtual_list(std::string_view key, VirtualListProvider&, Axis axis = Axis::Vertical, std::string_view classes = {})` | Recycling virtual list driven by a provider (held weakly). The list element is itself the scroll box. |
| `WidgetRef virtual_tree(std::string_view key, VirtualTreeProvider&, std::string_view classes = {})` | Recycling virtual tree over the flattened-expanded rows. |
| `WidgetRef virtual_list(std::string_view key, const std::vector<std::string>& items)` / `(…, const StringListOptions&)` | All-virtual list of strings; options carry `item_size`, `axis`, optional `IndexSelection* selection` / `* checked` (checkbox column). |
| `void set_scroll_provider(std::function<ScrollGeometry(std::string_view, Axis)>)` | Feeds virtual widgets their live scroll geometry; `App::set_view` wires it automatically. |
| `WidgetRef container_ref(...)` / `element_ref(...)` / `panel_ref(...)` | Leaf variants returned as refs (no open scope). |
| `Scope status_bar(std::string_view key = {})` | |
| `WidgetRef splitter(bool horizontal = false, std::string_view key = {})` | Drag splitter between docked regions. |
| `WidgetRef canvas(std::string_view paint_name, std::string_view classes = {}, std::string_view key = {})` | Custom-paint surface — emits `<div data-aui-paint="name">`; draw via `App::set_custom_paint`. |

### Menus & toolbars

| Signature | Notes |
|---|---|
| `Scope menu_bar(std::string_view key = {})` | |
| `WidgetRef menu_brand(std::string_view title, std::string_view icon = {}, std::string_view key = {})` | App brand at the start of a menubar. |
| `WidgetRef menu_button(std::string_view label, const std::function<void(View&)>& build, std::string_view key = {})` | Owns its dropdown, declared inline. (Also an id-targeting overload.) |
| `WidgetRef menu(std::string_view id, const std::function<void(View&)>& build)` | Standalone popup menu targeted by id. |
| `WidgetRef menu_item(std::string_view label, std::string_view icon = {}, std::string_view shortcut = {}, std::string_view key = {})` | Icon + label + accelerator text; wire `on_click`. |
| `Scope menu_item_custom(std::string_view key = {})` | Caller-composed row (swatches etc.); activates like `menu_item`. |
| `WidgetRef submenu(std::string_view label, const std::function<void(View&)>& build, std::string_view icon = {}, std::string_view key = {})` | Nested menu, any depth. |
| `WidgetRef menu_separator(...)`, `menu_spacer(...)`, `menu_meta(std::string_view text, ...)` | Separator; flexible spacer; right-aligned status text. |
| `WidgetRef document_title(std::string_view text, std::string_view key = {})` | The edited document's name, centered on the WINDOW (not between its neighbours). What a title bar shows. |
| `Scope toolbar(std::string_view key = {})` / `Scope floating_toolbar(const FloatingToolbarOptions& = {}, std::string_view key = {})` | Toolbar row / floating draggable rail. |
| `WidgetRef toolbar_separator(std::string_view key = {})` | |

### Docking

Declarative model: inside a `document_view` build, declare the center
`document()` and any number of `dockpanel()`s — **flat, any order** — each
with a `DockLocation`. The engine resolves them into a split tree and
auto-inserts splitters. The declaration is a **seed**; saved workspace state
supplied through the providers wins on rebuild.

| Signature | Description |
|---|---|
| `WidgetRef document_view(std::string_view key, const std::function<void(View&)>& build)` | Declare a dock container. |
| `DockHandle document(const std::function<void(View&)>& content, std::string_view title = "Document", std::string_view icon = {})` | The center pane (only valid inside a `document_view` build). |
| `DockHandle dockpanel(std::string_view title, const DockLocation& where, const std::function<void(View&)>& content, std::string_view icon = {}, std::string_view key = {})` | A dockable panel. |
| `DockHandle::toolbar(const std::function<void(View&)>& build)` | The pane's tab-strip toolbar; call right after `document()`/`dockpanel()`. |
| `Scope dock_panel(std::string_view title, std::string_view tabpanel_id, std::string_view classes = {}, std::string_view key = {})` | Lower-level: a single dockable panel scope. |

`DockLocation` — one value type for placement: implicit from `Dock::Left`
etc.; factories `docked(side, px)`, `tab()`, `floating(corner, pos, size)`,
`tearoff(corner, pos, size)`; fluent `in(parent)`, `sized(px)`,
`tearout_size({w, h})`, `dragging_with(handle)`. Enums:
`Dock { Left, Right, Top, Bottom, Tab }`,
`DockState { Docked, Detached, Tearoff }`, `DockCorner`.

Workspace persistence providers (saved state overrides the declared seed):

| Signature | Supplies |
|---|---|
| `void set_dock_size_provider(std::function<int(std::string_view pane_id)>)` | Saved per-pane px sizes (≤ 0 = fall back to declared). |
| `void set_dock_placement_provider(std::function<Document::DockPlacement(std::string_view panel_id)>)` | Saved structural overrides (side/parent/float rect). |
| `void set_dock_layout_provider(std::function<Document::DockLayout()>)` | The full current arrangement; when it covers every declared panel it is re-emitted instead of the seed (stale layouts fall back). |
| `void set_dock_active_tab_provider(std::function<std::string(std::string_view pane_id)>)` | Active tab per leaf; inactive tab bodies build lazily. |

## `View::Scope`

RAII guard returned by container builders; keeps the node open for child
builder calls and closes it on destruction. It records the stack **depth**
to restore, so compound builders (a pane + its body) close as one unit.
Move-only.

| Signature | Description |
|---|---|
| `WidgetRef ref() const` | Handle to the scope's node. |
| `Scope& named(std::string_view)` / `attr(name, value)` / `selector(name, value)` / `cls(classes)` / `text(value)` | Fluent setters on the open node. |
| `WidgetRef find_widget(std::string_view name) const` | Find a named descendant. |

## `WidgetRef`

Id-addressed widget handle. It weakly tracks its View (never keeps it alive),
survives reconciliation by re-finding its node by key, and degrades gracefully
when either the View or node is gone: reads return the fallback and **every
mutator no-ops**. `Scope` and `DockHandle` use the same View identity.

| Signature | Description |
|---|---|
| `explicit operator bool() const` | Does the ref currently resolve? |
| `std::string_view name() const` / `StableId id() const` / `const WidgetNode* node() const` | Identity. |
| `std::string_view attr_value(std::string_view name, std::string_view fallback = {}) const` / `text_value()` / `bool has_attr(name)` | Graceful reads. |
| `WidgetRef& text(std::string_view)` / `attr(name, value)` / `remove_attr(name)` / `selector(name, value)` / `named(name)` / `clear()` | Mutators. |
| `WidgetRef& cls(std::string_view classes)` | **Replaces the whole class list** — including framework classes the builder set. |
| `WidgetRef& add_class(std::string_view token)` | Appends one class token (idempotent) — the right way to add modifiers. |
| `WidgetRef& on_click(std::function<void()>)` / `on_change(std::function<void(std::string_view)>)` | Handlers. On an attached `set_view` View, replacement refreshes the live App binding immediately. |
| `WidgetRef& append(const std::function<void(View&)>&)` / `replace(...)` | Build children into the node. On an attached View, structural changes enter one scoped live-document mutation transaction. |
| `WidgetRef find_widget(std::string_view name) const` | Named descendant. |

## Virtual lists & trees (`affineui/virtual_list.h`)

Recycling virtual widgets: the DOM holds only the visible rows + overscan
(slot-keyed, structurally uniform, recycled in place), while spacers carry
the honest scroll extent. Requires the `App::set_view` retained-view loop.

| Type / member | Notes |
|---|---|
| `enum class SelectMod { Replace, Toggle, Range }` | Modifier intent of a row activation (plain / Ctrl / Shift). |
| `enum class Axis { Vertical, Horizontal }` | List orientation (trees are vertical). |
| `enum class DropPos { Before, Into, After }` | Row drag-and-drop target position (reserved; DnD lands in a follow-up). |
| `class IndexSelection` | Replace/toggle/range selection with an INDEX anchor: `apply(i, mod, count)`, `contains(i)`, `clear()`, `size()`, `anchor()`, `indices()`, `on_change(fn)`. For flat lists whose indices are stable identities; trees key by handle via `TreeFlattener`. |
| `class VirtualListProvider` | Stateless bridge of callbacks (Trackable; the widget holds a `WeakRef`). Fluent setters return the derived type: `on_item_count`, `on_item_size` (variable heights), `on_item_text`, `on_build_item(View&, i)` (rows must stay structurally uniform), `on_is_selected`, `on_activate(i, SelectMod)`, `on_is_checked`, `on_set_checked(i, bool)`, `checkboxes(bool)`, `default_item_size(px)`, `on_drop` (reserved). |
| `class VirtualTreeProvider` | Everything above plus the tree questions over the flattened-expanded rows: `on_depth`, `on_is_expandable`, `on_is_expanded`, `on_toggle`. |
| `template <class Data, class Item = void, class Handle = std::uintptr_t> class TreeFlattener` | Flattens a weak-ref'd data source into visible rows by opaque HANDLE (uint64 id / stable pointer / map key — unique to the item for its lifetime, never recycled). `on_roots` / `on_children` / `on_label` / `on_has_children` / optional `on_resolve` + `on_render` (handle → live item at render; null draws empty) / `on_changed`. `wire(provider)` answers every provider question. Owns the expanded set plus HANDLE-keyed selection (`activate(i, mod)`, `row_selected(i)`, `set_selected(h, on)`, `selected()`, `clear_selection()`, `index_of(h)`, `handle_at(i)`) and HANDLE-keyed checked state (`set_checked(h, on)`, `checked_contains(h)`, `checked()`), all of which survive expand/collapse renumbering. |
| `struct View::StringListOptions` | `item_size`, `axis`, `IndexSelection* selection`, `IndexSelection* checked`, `classes` — for the strings-only `virtual_list` overload. |
| Windowing math (`compute_window`, `virtual_offset`, `virtual_item_at`) | Header-level helpers used by the builders; unit-testable. |

**C ABI / bindings:** the full surface ships as
`affineui_vlist_provider_*`, `affineui_vtree_provider_*`,
`affineui_index_selection_*`, `affineui_tree_flattener_*`,
`affineui_view_virtual_*`, and `affineui_app_set_view` /
`affineui_app_rebuild_view` in `c_api_app.h`, with idiomatic wrappers in
Python (`ui.VirtualListProvider`…), Rust
(`affineui::VirtualListProvider`, `TreeFlattener` + `TreeSource`), and C#
(`AffineUI.VirtualListProvider`, `TreeFlattener` + `TreeSource`).

## Typed components (`affineui/components.h`)

Strongly-typed wrappers over a `WidgetRef` with semantic accessors. Get one
from a builder (`Button b = v.button("OK");`) or by query
(`view.component<Dropdown>("blend")`). **Never crash:** reads return typed
zeros/empties and writes no-op when the target is gone; wrong-type queries
stay attached (introspectable via `attr`/`text`/`kind()`) but typed
accessors go inert and a diagnostic is logged.

Common base `Component`: `valid()`, `explicit operator bool()`,
`validity()`, `attached()`, `id()`, `kind()`, generic
`attr`/`set_attr`/`text`/`set_text`/`visible`/`set_visible`, and `ref()` as
the escape hatch.

| Component | Typed surface |
|---|---|
| `Button` | `label()` / `set_label()`, `enabled()` / `set_enabled()`, `on_click(ClickCallback)` |
| `Checkbox` | `checked()` / `set_checked()`, `on_change(ChangeCallback)` |
| `TextField` | `value()` / `set_value()`, `on_change` |
| `Dropdown` | `selected()` / `set_selected()`, `on_change` |
| `Slider` | `value(double fallback = 0.0)`, `on_change` |
| `ColorField` | `color()` / `set_color(css_color)`, `on_change` |
| `DockPanel` | `active_tab()` / `set_active_tab(tab_id)` |
| `Foldout` | `open()` / `set_open(bool)` |

`ClickCallback` / `ChangeCallback` are the crash-safe `Callback` types from
Part 4 — plain lambdas work, `bind(&obj, &T::method)` adds a lifetime guard.

## Property inspector (`affineui/inspector.h`)

Bridges reflection (Part 4) to the component layer.

| Signature | Description |
|---|---|
| `WidgetRef property_field(View& v, const PropertyInfo& prop, const PropertyValue& current, std::function<void(const PropertyValue&)> on_change, std::string_view key = {})` | One editor field; widget chosen from attributes (`attr::Color` → color field, `attr::Slider`+`Range` → slider) then value type (bool → checkbox, double → number, string → text). |
| `template <Reflectable T> void inspect(View& v, const T& obj, std::function<void(std::string_view, const PropertyValue&)> on_edit)` | Fields for every property of `obj`; route `on_edit` through your command/undo system and write back with `set_property`. |

## Embedded Decius bundle (`affineui/decius_bundle.h`)

Namespace `affineui::decius`. The framework's default look, compiled into
the library (disable at build time with `AFFINEUI_NO_BUNDLE_DECIUS` /
CMake `AFFINEUI_BUNDLE_DECIUS=OFF`; at runtime with
`Config::no_bundle_decius`).

| Signature | Description |
|---|---|
| `bool available()` | Was the bundle compiled in? |
| `std::string css_bundle()` | The Decius CSS, decompressed on demand. |
| `std::string load(std::string_view url_suffix)` | Bytes of a bundled asset (e.g. the icon font) by URL suffix. |
| `void apply(App& app)` | Wire the bundled CSS into an App (done automatically by default). |

---

# Part 3 — The HTML5 renderer

Headers: `affineui/document.h`, `affineui/ui.h`, `affineui/imm.h`,
`affineui/painter.h`, `affineui/style.h`, `affineui/computed_style.h`,
`affineui/themes.h`, `affineui/automation.h`.

This layer speaks web content: HTML documents, CSS stylesheets, elements,
selectors, computed styles. No widgets, no View.

## `Document` (`affineui/document.h`)

A parsed HTML document with its CSS, layout, and event state. Owned by an
App (`app.document()`) or created standalone for **headless** layout/tests.
Non-copyable, movable.

### Loading & stylesheets

| Signature | Description |
|---|---|
| `void set_html(std::string_view html)` | Parse and replace the document. |
| `void set_user_stylesheet(std::string_view css)` / `(css, base_url)` | User stylesheet above the document's own styles; `base_url` resolves the sheet's relative `url()`s. |
| `void reload_stylesheets()` | Reapply CSS without re-parsing the DOM (hot reload). |
| `void set_resource_loader(ResourceLoader loader)` | URL → bytes hook for `<img>`, `<link>`, `url()`. |

### Layout & drawing

| Signature | Description |
|---|---|
| `void layout(int viewport_w, int viewport_h = 0, Painter* measurer = nullptr)` | Layout pass (automatic in an App; explicit for headless use). `measurer` supplies real text metrics. |
| `Size content_size() const` | Document size after the last layout. |
| `Rect find_element_rect(std::string_view target) const` | Border-box rect of `"#id"`, `"[name=value]"`, or a widget key; `w <= 0` when not found. |
| `void draw(Painter& painter)` | Paint the document. |
| `std::vector<Rect> take_dirty_rects()` / `bool take_paint_dirty()` | Drain damage from live mutations. |
| `bool has_active_animations() const` | CSS animations need another frame. |
| `void set_animation_time_for_testing(double seconds)` | Deterministic animation clock *(test harness only)*. |

### Events & behavior

| Signature | Description |
|---|---|
| `DispatchResult dispatch(const Event& ev)` | Route input: hover, focus, `:active`, form controls. |
| `void attach_script(DocumentScript)` / `detach_script(...)` / `clear_scripts()` | Optional native behavior scripts — the C++ equivalent of page script. `DocumentScript::UiControls` = form controls + framework widget interaction. Without one, a document is paint-only. |
| `std::vector<std::string> take_activated_widgets()` | Drain named activations from behavior scripts. |
| `std::vector<WidgetChange> take_widget_changes()` | Drain named value changes (`{name, value}` strings). |
| `void set_clipboard(ClipboardGet get, ClipboardSet set)` | Clipboard bridge; a deterministic in-document fallback exists without it. |
| `bool text_input_active() const` / `Rect caret_rect() const` | Platform IME intent and candidate-window anchor for the focused editor. |
| `void set_caret_blink_interval(double ms)` / `double caret_blink_interval() const` | Caret visibility half-cycle; default 500 ms, zero keeps it visible. |
| `bool tick_caret_blink()` | Advance caret timing for custom/headless Document drivers; App and Ui call it automatically. |
| `HoverInfo hovered_info() const` / `std::vector<HoverInfo> hovered_info_chain() const` | Hovered-element identity (chain is deepest-first, for bubbling-style routing). |
| `int hovered_cursor() const` | Cursor id: 0 default, 1 pointer, 2 text, 3 crosshair, 4 move, 5 not-allowed, 6 ew-resize, 7 ns-resize, 8 nwse-resize. |

### Live DOM mutation

The retained-mode analog of small JS updates — mutate one element, dirty
only its subtree. Each returns `true` only when the document actually
changed.

| Signature |
|---|
| `bool set_attribute_by_id(std::string_view elem_id, std::string_view name, std::string_view value)` |
| `bool remove_attribute_by_id(std::string_view elem_id, std::string_view name)` |
| `bool set_text_by_id(std::string_view elem_id, std::string_view text)` |
| `DomHandle weak_handle_for_id(std::string_view elem_id)` / `bool weak_handle_valid(DomHandle) const` — versioned weak handles to DOM nodes (never extend lifetime; invalid once the node or document goes away) |

### Custom paint

| Signature | Description |
|---|---|
| `void set_custom_paint(std::string_view name, CustomPaintFn fn)` | Handler for `data-aui-paint="name"` elements; receives `(Painter&, const Rect&)` (border box, document space). **Must not mutate the Document.** |
| `bool request_custom_repaint(std::string_view name)` | Dirty matching rects for next frame — geometry only. |

### Dock state readback

Used by the framework layer's persistence providers; listed here because
the data is read from the live document.

| Signature | Description |
|---|---|
| `DockLayout dock_layout() const` | The current arrangement (splits, tabs, floats), read live from the DOM. |
| `std::vector<std::pair<std::string, int>> dock_pane_sizes() const` | Current px size per sized pane. |
| `DockPlacement dock_override(std::string_view panel_id) const` / `dock_overrides()` | Placement overrides from drag-to-dock / tearoff. |
| `std::string dock_active_tab(std::string_view pane_id) const` | Active tab per leaf. |

## `Ui` — the one-type facade (`affineui/ui.h`)

Composes a `Document` and a `Renderer` behind one type — the surface game
integrations and the C ABI wrap. Single-threaded, non-copyable, and non-movable
so retained host wiring cannot follow a relocated facade by accident.

| Signature | Description |
|---|---|
| `void html(std::string_view source)` | Replace the document (full reparse — call infrequently). |
| `void css(std::string_view source)` | Set the user stylesheet. |
| `bool load(std::string_view path)` | Load an `.html` file; linked resources resolve relative to it. |
| `void render(int fb_w, int fb_h, float dpi_scale)` | Render into the current framebuffer; first call lazily creates GPU resources. |
| `void render(const FrameTarget& target)` | Embedded mode (see Part 5); requires `init()` first. |
| `void init(const InitDesc& desc)` | Embedded-mode init against host GPU objects. |
| `bool dispatch(const Event& e)` | True when the UI consumed the event. |
| `bool dispatch(const Event&, Painter&)` | Exact headless/custom-host dispatch; borrows the Painter for this call and never retains it. |
| `void on_click(std::string_view selector, std::function<void()> cb)` | Click handler for a selector. *(experimental grammar: `#id`, `.cls`, `tag`, comma lists — compound selectors later)* |
| `void on_event_capture(EventHandler)` / `on_event(EventHandler)` | Pre-widget interception for mouse, keyboard, text, and composition events / post-document low-level event phase. Returning true from capture prevents AffineUI handling (but does not cancel the underlying OS event). |
| `on_frame(std::function<void(double)>)` / `run_frame_callbacks(double)` | Frame tick. |
| `bool needs_update() const` / `void mark_dirty()` | Render-on-demand support (advisory; render is always safe). |
| `bool set_attr(elem_id, name, value)` / `remove_attr(...)` / `set_text(...)` | Live DOM mutation (as on Document). |
| `void mount(std::function<void()> view_fn)` / `void invalidate()` | Immediate-mode entry points. |
| `capture_pointer()` / `release_pointer()` / `pointer_captured()` / `hovered_info_chain()` / `hovered_cursor()` | Input helpers (as on App/Document). |
| `set_clear_color(Color)` / `clear_color()` / `content_size()` / `take_dirty_rects()` / `html_epoch()` / `render_epoch()` | Misc. |
| `Document& document()` / `Renderer& renderer()` | Escape hatches. |
| `void reset()` | Back to a clean reusable state, keeping the GPU binding. *(experimental: cached GPU/asset resources not yet released)* |

## Immediate mode (`affineui::imm`)

A Dear-ImGui-shaped layer over the renderer (exclude with
`AFFINEUI_NO_IMM`). The view function re-runs only when state plausibly
changed — never per frame; painting always runs off the retained DOM.
Identity is the call site (`std::source_location`); reordering lists need
an explicit `.key(...)`.

- **Containers** (return RAII `imm::Scope`): `div`, `span`, `section`,
  `header`, `footer`, `nav`, `main_`, `ul`, `ol`, `li`, `form`, `label`,
  `a(href, classes)`, headings `h1`–`h4`, `p`, `button(label, classes)`,
  `input(type, value)`, `checkbox(checked)`, `textarea(value)`,
  `img(src, alt)`.
- **Leaves:** `text(t)`; `raw_html(html)` (static graft, replaced wholesale
  each pass).
- **`imm::Scope` fluent API:** `key(k)`, `id(v)`, `cls(classes)`,
  `style(css)`, `attr(name, value)`, `text(t)`, and handlers `on_click`,
  `on_input`, `on_change`, `on_hover` — **handler closures live until the
  next reconciliation; re-register each pass.**
- **State:** `use_state<T>(initial)` returns a `StateRef<T>`; assignment or
  `update(f)` marks the view dirty automatically. `use_effect(effect,
  deps_hash)` mirrors React's useEffect. Slots are keyed by call-site path
  and freed after one full pass of absence.
- **Control:** `invalidate()` (safe from anywhere on the thread),
  `is_dirty()`, `key(value)` (keys the next opened element).
- **Binding:** `bind(Document&, view_fn)` / `unbind(Document&)` — most
  hosts use `App::mount` / `Ui::mount` instead.

## `Painter` (`affineui/painter.h`)

The abstract vector-drawing interface — what the engine paints with, and
the API available inside custom-paint handlers. The default implementation
wraps NanoVG on sokol_gfx; embedders can supply their own. Stateful within
a frame (clip/alpha/transform stacks must balance).

Selected surface (full list in the header):

- **Shapes:** `fill_rect`, `stroke_rect`, `stroke_line`, `fill_circle`,
  `stroke_arc` (degrees, clockwise from 12 o'clock),
  `fill_rounded_rect[_varying]`, `stroke_rounded_rect[_varying]`,
  `fill_rounded_rect_ring`, `fill_box_shadow`.
- **Gradients:** `fill_linear_gradient_rect(angle_deg, c0, c1, radii…)`
  (CSS angle convention), `fill_radial_gradient_rect(...)`.
- **Paths:** `fill_path(cmds, count, paint)` / `stroke_path(cmds, count,
  paint, width, cap, join)` — a flat float command stream (`kPathMove x y`,
  `kPathLine x y`, `kPathCubic c1x c1y c2x c2y x y`, `kPathClose`), nonzero
  winding. `PathPaint` is solid or an up-to-8-stop linear/radial gradient
  (`PathPaint::solid/linear/radial` + `add_stop`). `LineCap { Butt, Round,
  Square }`, `LineJoin { Miter, Round, Bevel }`. |
- **Text:** `resolve_font(family, size_px, weight, italic)` → handle (0 =
  fallback), `register_font_face(...)`, `measure_text`, `text_metrics`
  (ascender/descender/line height), `draw_text`, `measure_text_box` /
  `draw_text_box` (wrapped text; pass identical spacing params to both).
- **Images:** `load_image(url)` → handle (0 = miss), `image_size`,
  `draw_image(handle, dst, src)`.
- **State:** `push_clip(rect)` / `pop_clip`, `push_alpha(a)` / `pop_alpha`,
  `push_transform(Mat2x3)` / `pop_transform`.

## CSS model types (`affineui/style.h`, `affineui/computed_style.h`)

Public so tools, custom animators, and the imm layer can speak the engine's
CSS vocabulary.

- `struct Length` — 8-byte CSS length: `value` + `LengthUnit { Px, Em, Rem,
  Percent, Vw, Vh, Vmin, Vmax, Auto, None, Calc, Number }`; factories
  `Length::px/em/rem/percent/number/auto_/none`; predicates
  `is_auto/is_none/is_absolute/is_percent`. Aggregates `LengthEdges`
  (margin/padding/border/inset), `LengthCorners` (radii),
  `BorderStyleEdges`, `ColorEdges`.
- Property enums (all sized for the computed-style struct): `Display`
  (`Grid`/`InlineGrid` are *(experimental)*), `Position`, `Overflow`,
  `Visibility`, `FlexDirection`, `FlexWrap`, `JustifyContent`,
  `AlignItems` (+ `AlignContent`/`AlignSelf` aliases), `BorderStyle`,
  `TextAlign`, `TextDecorationLine` (bitmask), `FontStyle`, `WhiteSpace`,
  `WordBreak`, `Cursor`, `WillChange` (bitmask). `FontWeight` is a plain
  `std::uint16_t` (100–900) so weight animates as a lerp.
- `enum class PropertyId` — one value per supported longhand. **Order is
  unstable; never persist.**
- `struct ComputedStyle` — the per-element post-cascade snapshot (box
  model, visual, flex, text, insets), reference-counted, ~256 B with rare
  data behind an `extras` pointer. Read-only interest for tools:
  `has_borders()`, `has_background()`, `is_block_flow()`,
  `is_flex_container()`, `participates_in_layout()`,
  `requires_own_layer()`.

## Bundled stylesheets (`affineui/themes.h`)

Namespace `affineui::theme` — tested CSS you can pass to
`set_stylesheet()`: `ua_default()` (the user-agent sheet, always applied),
`material_dark()`, `material_light()`, `bootstrap_dark()`,
`bootstrap_light()`.

## UI automation (`affineui/automation.h`)

`UiScript` drives a Document with scripted input and validates results —
headless, deterministic.

- Construction: `UiScript(Document&, viewport_w, viewport_h, Painter*
  measurer = nullptr)`; `set_step_hook(fn)` runs after every dispatch (so a
  script can emulate the host's rebuild-on-layout-change loop).
- Pointer: `point_of(target, anchor)`, `move_to(...)`, `mouse_down/up`,
  `click(target, anchor)`, `drag(from, to, anchor, steps = 6)` (the docking
  gesture), `drag_to(point, steps)`. Targets are `"#id"`,
  `"[attr=value]"`, or a widget key; `Anchor { Center, Left, Right, Top,
  Bottom, TopLeft }`.
- Keyboard: `key(Key, shift, ctrl, alt)`, `type_text(text)`.
- Validation: `log()`, `log_contains(needle)`, `clear_log()`, and
  `static validate_dock_layout(layout, expected_panels)` → human-readable
  violations (empty = valid).

---

# Part 4 — Core facilities

Headers: `affineui/types.h`, `affineui/geom.h`, `affineui/callback.h`,
`affineui/weak_ref.h`, `affineui/object.h`, `affineui/keymap.h`,
`affineui/log.h`, `affineui/memory.h`, `affineui/version.h`.

The most heavily used facility here is the **versioned weak-reference
idiom**. `WeakRef<T>` carries `{registry, slot, generation}` for object
references; `DomHandle` uses the same slot-and-generation principle for DOM
nodes; safe callbacks and framework-level `WidgetRef` build on those handles.
It is why, everywhere in AffineUI, a reference to something that has been
destroyed degrades to a no-op instead of dangling.

## Value types (`affineui/types.h`)

| Type | Description |
|---|---|
| `struct Color { r, g, b, a{255} }` | 8-bit RGBA; `Color::rgb(r,g,b)`, `Color::rgba(r,g,b,a)`. |
| `struct Size`, `Point`, `Rect` | Integer geometry. |
| `struct DomHandle` | `{document_id, node_slot, generation}` versioned weak handle to a DOM node. |
| `enum class Key` | Platform-independent key codes: `Escape, Tab, Enter, Backspace, Delete`, arrows, `Home/End`, `A`–`Z`, `Digit0`–`Digit9`, `Space`, `Minus`, `Equal`, `BracketLeft`, `BracketRight`. Printable glyphs arrive as `TextInput`; these physical-key values exist for shortcut routing. |
| `enum class EventType` | `MouseMove, MouseDown, MouseUp, MouseWheel, KeyDown, KeyUp, TextInput, Composition, Resize, FocusLost, FocusGained`. |
| `struct Event` | Translated input event: `type, pos, button, wheel_dx/dy, key, key_code` (native scancode), `text` (`TextInput` commit or `Composition` preedit), UTF-8-byte composition cursor/clause offsets, and modifier flags (`super` = Cmd on macOS). |
| `struct DispatchResult` | `redraw_requested`, `invalidate_view`, `defer_widget_changes` (a native gesture is mid-flight), `layout_changed` (persist the dock layout), `event_consumed` (the focused document target handled the event). |
| `enum class MouseButton { Left, Right, Middle }` | |
| `using ResourceLoader = std::function<std::string(std::string_view url)>` | URL → raw bytes; empty = miss. |

## Float geometry (`affineui/geom.h`)

`Vec2`, `SizeF`, `RectF` (origin + extent; `right()`, `bottom()`,
`contains`, `inset`, `translate`, `intersects`, `from_min_max`), `EdgesF`
(per-side amounts; `inset`/`outset` a rect), `CornerRadii`, and `Mat2x3`
(2D affine transform: `identity/translate/scale/rotate`, `apply(Vec2)`,
`then(m)`). Conversions `to_int(RectF)` / `to_float(Rect)`.

## Crash-safe callbacks (`affineui/callback.h`, `affineui/weak_ref.h`)

The classic retained-UI crash — an event firing into a destroyed object — is
avoided by `bind()` and `guard()`. Their retained closures store weak identity,
resolve only for the call, and silently no-op once the target dies. Plain
capturing lambdas remain available for self-contained or externally
lifetime-safe handlers.

| Signature | Description |
|---|---|
| `template <typename... Args> class Callback` | Callable with optional guard; implicit from any lambda (unguarded); converts to `std::function` (guard still honored). Aliases: `ClickCallback = Callback<>`, `ChangeCallback = Callback<std::string_view>`. |
| `bind(T* obj, R (T::*method)(Args...))` | Safe (object, method) callback — no-ops after `obj` is destroyed; null obj → empty callback. Const overloads exist. |
| `bind(T* obj, method, bound_args...)` | Binds arguments by value into a zero-arg callback (per-item handlers: `row.on_click(bind(this, &C::select, id))`). |
| `guard(T* owner, callable)` | Guards an arbitrary void/non-void callable. The callable receives the live `T&` first, so it need not capture a raw `this`. Dead non-void calls return a default value. |

Opting into weak tracking (required for `bind`'s guard, structural — the
`WeaklyTrackable` concept):

| Mechanism | Use |
|---|---|
| `class Trackable` (base) | The default for long-lived controllers/models. Non-copyable/movable; virtual dtor. |
| `AFFINEUI_WEAK_TRACKABLE()` (macro) | One-line retrofit inside any class body; adds a registry identity and one 32-bit slot. |
| `WeakRef<T>` | Typed copyable weak reference: `get()` → a non-owning `T*` or null, `alive()`, `bound()`. `to_weak_ref(obj)` from a pointer. The borrow does not pin the target; resolve again after re-entrant work that could destroy it. `lock()` is a deprecated compatibility alias. |

The mechanism is a versioned slot table (the game-engine handle pattern) — the
same idiom `DomHandle` uses for DOM nodes. A statically linked library or
extension module may own its own table; each slot owner and weak reference
carries the issuing table's identity so a handle continues to resolve across
module boundaries. Only make trackable what genuinely receives callbacks that
outlive a stack frame; plain value types stay plain.

## Reflection (`affineui/object.h`)

Non-intrusive runtime reflection: a type opts in by providing an ADL
`get_class()` overload returning an `ObjectClass` — no base class, no
macro, no change to the type. Components (like the inspector) talk only to
the class object. Reflection is independent of lifetime tracking; objects
bound into live UI should *also* be `Trackable`.

| Item | Description |
|---|---|
| `using PropertyValue = std::variant<bool, double, std::string>` | Deliberately small; maps to editor field kinds. |
| `namespace attr` | Property attributes: `Label{text}`, `Tooltip{text}`, `Range{min, max, step}`, `Slider{}`, `Color{}`, `ReadOnly{}`. |
| `struct PropertyInfo` | Name + attributes + type-erased get/set; `valid()`, `attribute<A>()`, `has<A>()`, `display_label()`. Invalid descriptors are safe to use. |
| `class ObjectClass` | The mediator: `name()`, `type_id()`, `property_count()`, `property_at(i)`, `get(obj, prop)`, `set(obj, prop, value)`. |
| `template <class T> class ObjectClassBuilder` | Fluent: `.property("x", &T::x, attr::Range{0, 1}, attr::Slider{})`, `.computed(name, getter, setter, attrs...)`, `.build()`. Wrong-typed writes are silently ignored. |
| `concept Reflectable` | `get_class(obj)` resolves by ADL. Helpers `get_property(obj, name)` / `set_property(obj, name, value)`. |
| `class ObjectBase : public Trackable` | Optional convenience base: trackable + virtual `object_class()`. Never required. |

## Key bindings (`affineui/keymap.h`)

Header-only. `Chord { Key key; bool ctrl, shift, alt, super; }` (aggregate:
`{Key::Z, true}` = Ctrl+Z); `chord_from_event(ev)`.

`class Keymap`: `bind(chord, action_id, Layer::Default|User)` (User layer
overrides Default on lookup), `command_for(chord)` → action id,
`shortcut_text(action_id)` → "Ctrl Z" for menus, `static key_name(key)`.

## Logging (`affineui/log.h`)

One funnel for the library's diagnostics; default sink writes to the
console **and** an attached devtools viewer.

| Signature | Description |
|---|---|
| `struct LogRecord { LogLevel level; std::string_view msg; std::uint64_t frame; double t_ms; }` | `msg` is only valid during the handler call — copy to retain. |
| `void set_log_handler(LogHandler)` | Replace the sink (empty restores default). A custom handler owns each line; call `forward_to_affinetools(record)` to keep devtools fed. |
| `void log(LogLevel, std::string_view)` + `log_info/warn/error` | Emit through the active handler (apps may use it too). |
| `forward_to_console(record)` / `forward_to_affinetools(record)` | The default-sink primitives. |

## Memory (`affineui/memory.h`)

Namespace `affineui::mem`. All engine heap traffic routes through one
allocator so hosts can supply memory and debug builds can track leaks
(lexbor, Yoga, NanoVG, sokol included).

| Signature | Description |
|---|---|
| `const Allocator* set_allocator(const Allocator*)` | Install the host allocator (**before any allocations** — i.e. before the first Document); null restores the built-in. |
| `allocate(size, align)` / `reallocate(p, new_size, align)` / `deallocate(p)` | Raw entry points. |
| `struct Stats` + `Stats stats()` | `live_bytes/live_blocks/peak_bytes/total_allocs/total_frees` (cheap atomics, always compiled). |
| `std::size_t report_leaks()` | Live blocks at shutdown (0 = clean); with `AFFINEUI_MEM_DEBUG`, a per-block report goes to the log. |
| `struct Tag` | RAII allocation tag for leak attribution *(no-op without `AFFINEUI_MEM_DEBUG`)*. |
| `void install_lexbor_hooks()` | Idempotent; auto-called before the first Document. |

## Version (`affineui/version.h`)

`struct Version { major, minor, patch }` defaulted from the
`AFFINEUI_VERSION_*` build macros; `const char* version_string() noexcept`.

---

# Part 5 — Embedding & integration

Headers: `affineui/embed.h`, `affineui/renderer.h`, `affineui/sokol.h`,
`affineui/sdl.h`, `affineui/c_api.h`, `affineui/c_api_app.h`,
`affineui/tools.h`, `affineui/telemetry.h`. Model and walkthrough:
[EMBEDDING.md](EMBEDDING.md).

## Embedded mode (`affineui/embed.h`)

Two ownership modes, all-or-nothing: **standalone** (AffineUI owns device,
window, loop) or **embedded** (host owns device and present; AffineUI draws
into host-supplied render targets inside its own sokol_gfx pass and never
presents). Treat host GPU state as **undefined after render returns**
(D3D11/GL) — the Dear-ImGui contract. All graphics handles are opaque
`void*`; backend must match the compiled `AFFINEUI_BACKEND_*`.

| Type | Description |
|---|---|
| `enum class Backend { d3d11, metal, gl, wgpu }` | |
| `enum class PixelFormat { default_, rgba8, bgra8, depth, depth_stencil }` | `default_` = platform-typical swapchain formats. |
| `struct GpuContext` | Host device objects, supplied once at init (borrowed, never destroyed by AffineUI): per-backend handles + `color_format`/`depth_format` + `sample_count`. Partially-filled contexts are rejected. |
| `struct InitDesc` | `gpu` (non-null = embedded), `resource_loader`, `allocator`, `log`/`log_user`, `default_font_family`/`_size`. |
| `struct FrameTarget` | Per-frame targets: `width/height` (px), `dpi_scale`, `sample_count`, `clear` (false = composite over host content), `commit`, `viewport` sub-rect (render-to-texture / panel-in-scene), per-backend view handles, optional debug-overlay fields. |
| `struct Allocator` | `alloc/realloc/free` function pointers + `user`. |
| `LogFn` / `enum class LogLevel { debug, info, warn, error }` | Host log sink. |

A block of *planned* surface (async asset resolver, atlases, hit-test /
click-through, input intents, partial repaint) is declared as intent in the
header — not yet implemented.

## `Renderer` (`affineui/renderer.h`)

Owns the NanoVG context, the root layer, the cached display list, and the
compositor. Does not own the window or loop.

| Signature | Description |
|---|---|
| `bool ready() const` | GPU resources live? |
| `void init_gl()` | Optional eager init against a current GL context (otherwise lazy on first render). |
| `void init_embedded(const GpuContext&, const Allocator* = nullptr)` | Bring up sokol_gfx on the host's device objects. |
| `void render(Document&, int fb_w, int fb_h, float dpi_scale)` | One frame into the default framebuffer. |
| `void render_to(Document&, const FrameTarget&)` | One frame into host views (embedded). |
| `DispatchResult dispatch(Document&, const Event&)` | Dispatch with exact interaction relayout through the renderer-owned Painter. |
| `ImageHandle create_image_rgba(w, h, rgba)` | Create a managed RGBA8 image. Copies share ownership; final release destroys the GPU image; shutdown invalidates all handles. |
| `Rect caret_rect(Document&)` | Exact caret geometry using the renderer-owned Painter without exposing or retaining it. |
| `void shutdown()` | Release GPU resources **while the context is still current**. |
| `set_clear_color(Color)` / `clear_color()` | Default `#1e1e2e`. |
| `const RenderStats& stats() const` | Per-frame/lifetime counters (display-list records/replays, rasterizes, stage timings µs, dirty areas). |
| `void draw_debug_overlay(text, frame_ms, fb_w, fb_h, dpi, corner)` | Native overlay outside the document tree. |

`ImageHandle` is the dynamic-image API. `is_valid()` reports whether its
renderer and generation are live, `update(rgba)` replaces the complete payload,
and `reset()` releases that copy. Drawing or updating an invalid handle is a
safe no-op. Cached display lists retain a handle lease for as long as they may
replay the image. Image creation, update, and drawing occur on the renderer/UI
thread; final release from another thread is retired for renderer-thread
destruction.

`affineui/display_list.h` (the hashable/diffable paint-op stream:
`PaintOp`, `DisplayList`, `rolling_hash()`, `same_pixels_as()`) is public
for tooling and custom rasterizers.

## Windowing adapters

Opt in with `AFFINEUI_WITH_SOKOL` / `AFFINEUI_WITH_SDL` before including
the umbrella header.

- **`affineui::sokol`** — `translate(const sapp_event*)` → `Event`,
  `dispatch(Ui&, ev)`, `render(Ui&)` (inside your pass) or
  `render_frame(Ui&, clear, commit)` (AffineUI owns the pass; enables
  retained-layer caching), `frame_target(clear, commit)`, and
  `wire(sapp_desc&, Ui&, PerfHudOptions)` for "the app *is* the UI"
  programs. Helpers: `cursor_to_sokol`, `key_to_affine`,
  `apply_modifiers`, `utf8_from_codepoint`.
- **`affineui::sdl`** — `translate(const SDL_Event&)`, `dispatch(Ui&, ev)`,
  `render(Ui&, SDL_Window*)`. GL3 contexts only for now; native
  D3D11/Metal/Vulkan SDL paths are planned.

## C ABI (`affineui/c_api.h`, `affineui/c_api_app.h`)

The flat `extern "C"` surface the Rust and C# bindings (and any C-capable
language) consume, shipped as the `affineui_c` shared library. Full spec:
[LANGUAGE_BINDINGS.md](LANGUAGE_BINDINGS.md). Contracts:

- `AFFINEUI_C_ABI_VERSION` / `affineui_c_abi_version()` — bumped on any
  breaking change; wrappers fail fast on mismatch.
- Out-strings are heap copies — free with `affineui_string_free()`
  (exception: `affineui_version()` is borrowed).
- Callbacks are `(fn, user, user_free)` triples; `user_free` fires
  **exactly once** when the core drops its last reference.
- Every function null-checks every handle and no-ops instead of faulting.

Surface, by prefix: `affineui_ui_*` mirrors `Ui` (embedded rendering,
content, events, live mutation); `affineui_app_*` mirrors `App`
(config/lifecycle/loop); `affineui_document_*` mirrors headless `Document`;
`affineui_view_*` / `affineui_widget_*` mirror the component framework's
builders and `WidgetRef`; `affineui_decius_*` exposes the embedded bundle;
`affineui_tools_*` controls the devtools server.

## DevTools & telemetry (`affineui/tools.h`, `affineui/telemetry.h`)

- `tools_listen(port = 0)` starts the loopback affinetools protocol server
  (discovery file + token auth); `tools_open_devtools()` is the F12 path;
  `tools_active()`, `tools_port()`, `tools_shutdown()`,
  `tools_listen_from_env()` (honors `AFFINEUI_TOOLS_LISTEN`). All become
  no-op stubs when compiled out.
- `FrameTelemetry` — one record per presented frame: wall-clock `gap_ms`
  (the truth metric for stalls), stage timings, display-list/dirty
  counters, allocator deltas. In-process via `App::frame_telemetry()`;
  JSONL via `AFFINEUI_TELEMETRY=<path>` (+ `AFFINEUI_TELEMETRY_EVERY=N`);
  streamed to attached devtools. Perf-sensitive call sites gate on
  `telemetry::sink_active()` / `tools::wants_telemetry()`.

---

# Appendix — Compile-time switches & environment variables

| Switch | Default | Effect |
|---|---|---|
| `AFFINEUI_PERF` | `1` | Master gate for telemetry, perf overlay, devtools hotkey. `0` compiles them out (APIs remain as no-op stubs). |
| `AFFINEUI_TOOLS` | `AFFINEUI_PERF` | Gates the devtools socket server separately. |
| `AFFINEUI_BUNDLE_DECIUS` (CMake) / `AFFINEUI_NO_BUNDLE_DECIUS` (macro) | bundled | Drop the embedded Decius CSS + fonts from the binary. |
| `AFFINEUI_NO_IMM` | off | Exclude the immediate-mode layer from the umbrella header. |
| `AFFINEUI_NO_C_API` | off | Exclude the C ABI declarations. |
| `AFFINEUI_WITH_SOKOL` / `AFFINEUI_WITH_SDL` | off | Pull in the windowing adapters. |
| `AFFINEUI_MEM_DEBUG` | off | Per-block allocation tracking, leak reports, allocation tags. |
| `AFFINEUI_BACKEND_*` / `SOKOL_*` | platform | Selects the GPU backend the build targets. |

| Environment variable | Effect |
|---|---|
| `AFFINEUI_TELEMETRY=<path>` | Stream per-frame telemetry JSONL to a file. |
| `AFFINEUI_TELEMETRY_EVERY=N` | Sample every Nth frame record. |
| `AFFINEUI_TOOLS_LISTEN=1|<port>` | Start the devtools server at startup. |
| `AFFINEUI_TOOLS_EXE=<path>` | Override the affinetools viewer executable the F12 hotkey launches. |
