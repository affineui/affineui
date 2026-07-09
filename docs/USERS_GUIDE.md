# AffineUI User's Guide

A practical guide to building applications with AffineUI. It assumes you have
seen the [README](../README.md) pitch and can build or install the library
(see [BUILDING.md](BUILDING.md) and the per-language install commands in the
README). For a systematic list of every public type and function, see
[REFERENCE.md](REFERENCE.md).

AffineUI is in **alpha**. The APIs described here are usable for real UIs
today, but expect changes before 1.0.

---

## The two layers

**AffineUI is, first and foremost, a UI framework** — a native peer of Qt,
Dear ImGui, and the other application UI toolkits. That framework is the
point of the project: typed widgets, docking, menus, themes, and an
interaction layer, for building tools and applications. Underneath it sits
a very efficient, conformant HTML5 renderer that makes the framework
possible — and is independently useful in its own right.

So AffineUI is two distinct things stacked on top of each other, and
knowing which one you are talking to makes everything else in this guide
fall into place:

1. **The HTML5 renderer.** A standards-compliant web rendering engine —
   the same job a browser's rendering pipeline does. You hand it *web
   content*: an `.html` document, HTML strings, CSS stylesheets. It parses,
   cascades, lays out, and GPU-paints that content. At this layer there are
   no "widgets" — there are elements, selectors, and computed styles, exactly
   as on a webpage. Any CSS you can write, any CSS framework you can load
   (Bootstrap ships as a demo), renders here. What it deliberately is *not*:
   a browser. There is no JavaScript engine, no networking, no security
   sandbox — never feed it untrusted markup.

2. **The AffineUI component framework.** A typed, programmatic
   widget/builder API in the spirit of Dear ImGui, Gradio, or React — `View`,
   `button()`, `slider()`, `dock_panel()`, typed component handles, menus,
   docking, themes. You don't have to write a line of HTML at this layer —
   though you still can: dropping raw HTML snippets under or around widgets
   (`v.html(...)`) is a first-class feature you may use frequently, not a
   workaround. The framework *generates* HTML/CSS-shaped DOM, hands it to
   the renderer underneath, and reconciles it efficiently when your app
   state changes. Widget behavior (sliders dragging, menus opening, panels
   docking) is implemented in C++ inside the engine, not in page script —
   which is why every language binding gets the full interaction layer for
   free.

Both layers share a set of **core facilities** used heavily across every
API: value types, **versioned weak references and handles** — the one
stability idiom behind `WidgetRef`, `DomHandle`, and crash-safe callbacks,
and the reason a stale handle degrades instead of dangling — plus logging
and memory. They are described at the end of this guide.

**Which layer do you want?**

- Building an application UI — a tool, an editor, a settings panel? Use the
  **component framework**. This is the primary, recommended API and most of
  this guide.
- Rendering existing web-style content — an HTML document, a report, a page
  authored with your own CSS design system? Use the **renderer directly**
  (`load_html`, `set_stylesheet`, or the `Ui` facade).
- Both mix freely: component-built UIs can embed raw HTML fragments, and
  HTML documents can be live-mutated from C++.

---

# Part I — The component framework

## Hello, world

```cpp
#include <affineui/affineui.h>
using namespace affineui;

int main() {
    View view;                       // ViewTheme::Decius is the default
    view.begin();
    view.heading(1, "Hello from C++");
    view.paragraph("Native HTML/CSS rendering — no browser, no JS.");
    view.button("Click me", /*primary=*/true, "go")
        .on_click([] { std::printf("clicked!\n"); });
    view.end();

    App app(App::Config{ .title = "Hello", .width = 720, .height = 480 });
    app.load_view(view);
    return app.run();
}
```

That is a complete, **styled** application. The Decius CSS framework — the
stylesheet, the UI fonts, and the icon font — is compiled into the library
itself and applied by default. There are no asset files to ship and no
stylesheet to wire up. (Opt out with `Config::no_bundle_decius = true` or
build with `AFFINEUI_BUNDLE_DECIUS=OFF`; see
[Styling and themes](#styling-and-themes).)

The same program in the bindings looks nearly identical — see the README's
Hello World section for Python, Rust, and C# versions.

## The mental model: View is a reconciler

A `View` is not a retained widget tree you mutate, and not an immediate-mode
API that redraws every frame. It is a **description you re-run**, React
style:

1. `view.begin()` starts a build pass.
2. Builder calls (`heading`, `button`, `container`, …) describe the tree.
3. `view.end()` diffs the description against the previous pass and emits
   only the minimal DOM operations — created elements, changed attributes,
   changed text. An unchanged tree emits **zero** operations.

Widgets are matched across passes by their **key** — the trailing
`std::string_view key` parameter every builder takes. Give stable keys to
anything you want to find again, hold a handle to, or whose identity matters
in a reordering list.

For a one-shot UI, `app.load_view(view)` is fine. For an app that rebuilds
its UI when state changes, install a builder instead:

```cpp
App app(cfg);
app.set_view([&](View& v) { build_my_ui(v, state); });
// ... later, whenever state changes:
app.rebuild_view();   // re-runs the builder, reconciles the live document
```

`set_view`/`rebuild_view` is the fast path: attribute and text diffs go
straight to paint, structural edits settle with a single restyle, and no
HTML is ever re-parsed. `load_view` by contrast serializes and reparses the
whole tree — fine once at startup, wasteful per state change.

## The widget vocabulary

Content and input widgets (each returns a `WidgetRef`):

| Category | Builders |
|---|---|
| Text | `heading(level, text)`, `paragraph(text)`, `text(t)` |
| Buttons | `button(label, primary)`, `icon_button(icon)` |
| Toggles | `checkbox(label, checked)`, `toggle(label, on)` |
| Text entry | `input(label, value, type)`, `password(label, value)`, `textarea(label, value, rows)` |
| Choices | `dropdown(label, options, selected)`, `button_group(label, options, selected)` |
| Numeric | `slider(label, value, min, max)`, `knob(label, value, min, max, bipolar)`, `combo(label, value, step)` (bare drag-scrub number), `vec(label, channels)` (X/Y/Z rows) |
| Color | `colorfield(label, hex)` (chip + hex + picker popover), `color_field(label, value, swatches)` |
| Escape hatch | `html(markup)` — a trusted raw-HTML fragment inside a component tree |

Containers return a `View::Scope` — an RAII guard that keeps the container
open for children and closes it when the scope ends:

```cpp
{
    auto card = v.card("Render Settings");
    v.slider("Exposure", 0.5, 0.0, 1.0, "exposure");
    v.checkbox("Denoise", true, "denoise");
}   // card closes here
```

Structural builders: `container(classes)`, `element(tag, classes)`,
`panel()`, `card(title)`, `foldout(title, expanded)` (collapsible section),
`tree()` + `tree_row(label, opts)`, `virtual_list(key, opts, build_item)`
(windowed lists), `toolbar()`, `floating_toolbar(opts)`, `menu_bar()`,
`status_bar()`, `splitter()`.

Declaring a `menu_bar`, `toolbar`, `status_bar`, or `document_view` at the
root automatically switches the window from a padded content page to an
edge-to-edge **app shell** — you don't lay that out yourself.

## Handles: WidgetRef and typed components

Every builder returns a `WidgetRef` — a lightweight, id-addressed handle
that survives reconciliation and **degrades gracefully**: if the underlying
node is gone, reads return defaults and writes no-op. This is a core
contract across the whole framework: it must be hard to crash.

```cpp
auto save = v.icon_button("save", "save-btn");
save.attr("title", "Save the document");
save.add_class("app-save");           // append a class token
```

One sharp edge worth memorizing: **`cls()` replaces the entire class list**,
including the framework classes the builder emitted (a Decius button's
`dcs-btn`, for example). To add a modifier or an app hook class, use
`add_class()`.

For semantic access, wrap a widget in a **typed component**
(`Button`, `Checkbox`, `TextField`, `Dropdown`, `Slider`, `ColorField`,
`DockPanel`, `Foldout`):

```cpp
if (auto blend = view.component<Dropdown>("blend")) {
    std::string current = blend.selected();
    blend.set_selected("Multiply");
}
```

`component<T>` is always safe. Three outcomes: **Valid** (live node of the
right type — typed accessors work), **WrongType** (a node with that key
exists but isn't a `T` — a diagnostic is logged, typed accessors go inert,
generic `attr`/`text` access still works), **NotPresent** (nothing resolves —
everything no-ops).

## Events and crash-safe callbacks

Widgets take plain lambdas:

```cpp
v.button("Apply", true, "apply").on_click([&] { apply_changes(); });
v.slider("Gain", 0.5, 0.0, 1.0, "gain")
    .on_change([&](std::string_view value) { set_gain(value); });
```

For handlers that live on an object — a controller, a document model — use
`bind()`, which produces a **Qt-style safe callback**: it holds a versioned
weak reference to the object and becomes a silent no-op the moment the
object is destroyed. No dangling `this`, ever.

```cpp
struct Controller : Trackable {         // opt in to weak tracking
    void save();
    void select_item(int id);
};

v.icon_button("save", "save").on_click(bind(&ctl, &Controller::save));
// bind can also carry per-item arguments:
row.on_click(bind(&ctl, &Controller::select_item, item_id));
```

Opting a class into tracking is one of: inherit `Trackable`, drop the
`AFFINEUI_WEAK_TRACKABLE()` macro into the class body, or satisfy the
`WeaklyTrackable` concept structurally. Only long-lived objects that receive
UI callbacks need this — plain value types should stay plain.

Register handlers on the widget **before** `App::load_view` — `load_view`
copies the view, callbacks included.

## Application structure: menus, toolbars, docking

Menus are declared inline; the framework wires the popup targeting for you:

```cpp
{
    auto bar = v.menu_bar();
    v.menu_brand("MyTool", "cube");
    v.menu_button("File", [&](View& m) {
        v.menu_item("Open…", "folder", "Ctrl O", "open").on_click(...);
        v.menu_separator();
        v.menu_item("Exit", {}, {}, "exit").on_click(...);
    });
    v.menu_spacer();
    v.menu_meta("untitled.proj");
}
```

Docking is **declarative**: inside a `document_view` you declare the center
document pane and any number of dockable panels, each carrying a
`DockLocation`, in any order. The engine resolves the flat declarations into
a split-tree layout and inserts splitters automatically.

```cpp
v.document_view("main", [&](View& dv) {
    auto doc = dv.document([&](View& c) { build_viewport(c); }, "Scene");
    dv.dockpanel("Outliner",  Dock::Left,                    build_outliner);
    dv.dockpanel("Inspector", DockLocation::docked(Dock::Right, 320),
                 build_inspector);
    dv.dockpanel("Console",   DockLocation::docked(Dock::Bottom, 180),
                 build_console);
});
```

The declared layout is only the **seed**. Users drag splitters, re-dock, and
tear off panels at runtime; your app persists that arrangement and feeds it
back through the dock providers (`set_dock_layout_provider`,
`set_dock_size_provider`, `set_dock_placement_provider`,
`set_dock_active_tab_provider`), and a saved workspace then wins over the
seed on every rebuild. `App::Config::on_layout_changed` tells you when to
persist; `Document::dock_layout()` reads the live arrangement.

Keyboard shortcuts go through `Keymap` — chords (`{Key::Z, /*ctrl=*/true}`)
map to action-id strings in two layers, with user rebindings overriding app
defaults. `Keymap::shortcut_text()` produces the accelerator text you show
in menus.

## Styling and themes

A `View` has a **theme** (personality): `ViewTheme::Decius` (default),
`ViewTheme::Bootstrap`, or `ViewTheme::Plain`. The theme decides which
concrete class names the builders emit — the same `button()` call produces
Decius or Bootstrap markup. Decius is the framework AffineUI develops
against and embeds; Bootstrap demonstrates that the emitted markup is
ordinary HTML any CSS framework can style; Plain emits unstyled structure
for your own design system.

Theme-wide knobs are **selector attributes** set on the view or a widget.
The two main knobs, across all selectors, are **density** and the **key
(accent) color**:

```cpp
view.selector("density", "compact");   // compact | comfortable | spacious
view.selector("accent", "#4d8be8");    // the key color widgets derive from
```

Further selectors refine from there: `size` (`sm`/`md`/`lg`), `style`
(`flat`/`3d`), `radius`, and `dark`.

Customizing appearance, in order of preference:

1. **Use the framework's own variants** — `add_class("dcs-btn--sm")`,
   selector attributes, the personality's documented modifiers.
2. **Add your own CSS on top** via `App::set_stylesheet(css)` — it cascades
   above the framework bundle. Target your own hook classes
   (`add_class("app-save")`), not the framework's internals.
3. If a stylesheet references relative assets (an icon font, images), pass
   its base URL: `set_stylesheet(css, base_url)` — relative `url()`s then
   resolve exactly as they would for a `<link>`ed sheet.

Never *replace* a widget's framework class list to restyle it (`cls()` on a
Decius button) — that fights the framework instead of extending it.

Framework versions are first-class: `view.set_framework_version("0.6.2")`
pins the CSS bundle the view targets, and the version is stamped on the
document root so version-branching behavior can read it.

## Custom drawing: the canvas

For dynamic, per-frame vector content — waveforms, curves, node graphs,
patch cables — use a **custom paint canvas** rather than generating SVG or
DOM per frame:

```cpp
v.canvas("scope", "app-scope", "scope");

app.set_custom_paint("scope", [&](Painter& p, const Rect& r) {
    p.stroke_line(r.x, r.y + r.h / 2, r.x + r.w, r.y + r.h / 2,
                  Color::rgb(90, 220, 120), 2.0f);
    // fill_path/stroke_path take an SVG-like command stream with
    // gradients, caps, and joins — see REFERENCE.md, Painter.
});

// per frame, when the geometry changed:
app.request_custom_repaint("scope");   // repaint only; no restyle, no layout
```

The rule of thumb the project itself follows: **SVG for static declarative
art, the painter for anything dynamic.** `request_custom_repaint` is the
cheap path — it never touches the DOM.

## Extensions

The library ships **sidecar widget kits** under [`extras/`](../extras/) —
optional libraries built on the same component framework, linked only by
apps that want them.

The flagship is the **skeuomorphic hardware kit**
(`extras/skeuo`, `affineui_skeuo.h`): builders and typed components for
hardware-emulation UIs — modular synthesizers, drum machines, guitar
pedals, mixing consoles. It provides chassis faceplates with selectable
finishes and corner screws (`hw_panel`), silkscreen group boxes (`silk`),
etched panel labels, TS/TRS patch jacks with chrome hex nuts, backlit
buttons, 7-segment LED displays (`lcd`), LED bargraph meters, synth
faders, and steppers — plus **PatchBay**, Reason-style patch cabling
between jacks: drag to connect, re-route, click to remove, with cables
hanging on a catenary and swinging on a spring-damper while dragged. (The
cables are a custom-paint canvas, exactly the SVG-static / painter-dynamic
split described above.) The kit is Decius-only by design; see
[`examples/17_affine_2600`](../examples/17_affine_2600) — a full
ARP-2600-style semi-modular synth — for it in action.

---

# Part II — The HTML5 renderer

Everything in Part I compiles down to this layer. You can also use it
directly, as a web rendering engine — no components involved.

## Rendering web content

Hand the renderer an HTML document, exactly as a browser would receive one:

```cpp
#include <affineui/affineui.h>

int main() {
    return affineui::run(R"(
        <h1>Quarterly report</h1>
        <p class="lead">Rendered by AffineUI — real HTML, real CSS.</p>
    )");
}
```

Or with an `App` for control:

```cpp
App app(cfg);
app.load_html_file("report.html");      // <link> stylesheets resolve
app.set_stylesheet(my_css);             // user stylesheet, above the page's
return app.run();
```

What you get at this layer is a webpage's rendering model: the full CSS
cascade (author/user/UA origins), selectors and specificity, flexbox layout,
`@font-face`, `@media`, CSS animations and transitions, images,
backgrounds, borders, shadows, and gradients. `01_bootstrap` and
`10_bootstrap_dashboard` in `examples/` render real, unmodified Bootstrap
4.6 CSS from disk to prove the point.

**Inline SVG** is supported, with honest caveats: coverage is currently
*so-so* — good enough for the static vector art the framework and examples
use (icons, panel artwork, decorative shapes), but it is not a full SVG
engine, and complex SVG documents will find the edges. Treat it as a
static-art facility (and for anything dynamic, use the
[custom-paint canvas](#custom-drawing-the-canvas) instead — that's the
project's own rule).

What you do **not** get is a browser: no JavaScript, no network fetch (see
resource loading below), no cross-origin machinery, and no sandbox — treat
markup as trusted input only.

## Interactivity without JavaScript

An HTML document rendered this way is static by default. Three ways to make
it live:

- **Selector click handlers** (via the `Ui` facade):
  `ui.on_click("#save", [] { ... })`. The selector grammar is currently the
  minimal subset — `#id`, `.cls`, `tag`, and comma lists.
- **Live DOM mutation from C++** — the retained-mode analog of small JS
  state updates: `app.document().set_attribute_by_id("status", "class",
  "ok")`, `set_text_by_id`, `remove_attribute_by_id`. Each mutates one
  element and invalidates only the affected subtree; they return `true` only
  when the document actually changed.
- **Behavior scripts** — `Document::attach_script(DocumentScript::UiControls)`
  attaches the native C++ interaction layer (form controls, sliders,
  menus, docking) to a plain HTML document. This is the same machinery the
  component framework uses; it keys off `data-dcs-*` attributes in the
  markup.

## The Document

`Document` is the renderer's core object — a parsed HTML document with its
CSS, layout, and event state. `App` owns one (`app.document()`), and you can
also create them **headless** for layout tests and tools: `set_html` →
`layout(w, h)` → `content_size()` / `find_element_rect(...)` without any
window or GPU.

`Document::dispatch(event)` routes input through the page — hover, focus,
`:active`, form controls — and reports what needs repainting. Named widget
activity from behavior scripts is drained with `take_activated_widgets()` /
`take_widget_changes()`.

## Resource loading

URLs referenced by content (`<img src>`, `<link href>`, `url()` in CSS)
resolve through, in order:

1. the embedded Decius bundle (for `frameworks/…` URLs, unless opted out),
2. `App::Config::asset_folders` — a list of directories to probe,
3. a custom `ResourceLoader` hook — `std::function<std::string(url)>` —
   for archives, PAK files, or your engine's VFS.

## The Ui facade and immediate mode

`Ui` is a single-type facade over Document + Renderer for embedders: 
`ui.html(source)`, `ui.css(source)`, `ui.load(path)`, `ui.dispatch(event)`,
`ui.render(w, h, dpi)`. It is the type the sokol/SDL adapters and the C ABI
wrap. See [EMBEDDING.md](EMBEDDING.md) for the host-owned-GPU contract
(`Ui::init(InitDesc)` + `Ui::render(FrameTarget)`), render-to-texture, and
the state-clobber rules.

There is also a Dear-ImGui-shaped **immediate-mode layer** (`affineui::imm`)
over the renderer: `imm::div()`, `imm::button()`, `use_state<T>()`, with
call-site identity and React-style keys. The view function re-runs only when
state changed — painting always runs off the retained DOM. It is the
smallest way to put a quick UI over a game loop; the component framework is
the recommended surface for real applications. See `04_imm_counter` and
`05_imm_todo`.

---

# Part III — Shared machinery

## State, frames, and updates

- `app.on_frame([](double dt) { ... })` — the requestAnimationFrame analog.
  Call `app.invalidate()` inside when you need a repaint; do nothing and the
  app stays idle-cheap.
- `app.dispatch(event)` returns whether the UI consumed an event — embedders
  use this to decide whether the host should also handle it.
- Rendering is on-demand: frames paint only when something is dirty or
  animating.

## Reflection and the inspector

`affineui/object.h` provides a non-intrusive reflection mediator: a type
exposes an `ObjectClass` (name + property table) via an ADL `get_class()`
overload — no base class, no macros in the target, no boxing.
`affineui/inspector.h` then builds editor fields for any `Reflectable`
object (`inspect(view, obj, on_edit)`), choosing widgets from property
attributes (`attr::Range`, `attr::Slider`, `attr::Color`, `attr::ReadOnly`).
Reflection is independent of lifetime tracking, but objects bound into live
UI should also be `Trackable` so the binding no-ops after destruction.

## Automation and testing

`affineui/automation.h` drives a headless Document with scripted input —
`click("#save")`, `drag("tab-a", "dock-right")`, `type_text("hello")` — and
validates results against the trace log and dock-layout invariants. The
conformance harness ([CONFORMANCE.md](CONFORMANCE.md)) A/B-renders content
against real Chrome and pixel-diffs the result.

## DevTools: affinetools

AffineUI ships **affinetools**, a Chrome DevTools clone for AffineUI apps.
Use it the way you use Chrome's: press **F12** (or Ctrl+Shift+I) in any
running app and the tools open against it — inspect the live element tree
and its styles, watch performance, follow the log.

Under the hood it works differently from Chrome in one deliberate way:
affinetools is a **separate application**, not an in-process panel. The
hotkey starts a small loopback protocol server inside your app and launches
the affinetools viewer as its own process, attached to yours. That keeps
the tools' cost out of your app (a disabled server is one atomic check per
frame) and means the viewer can attach to any process — including one you
started headless (`AFFINEUI_TOOLS_LISTEN=1`).

affinetools is a **work in progress**; the goal is to keep it generally in
sync with what Chrome DevTools provides, so the workflow you already know
carries over. Disable the hotkey with `Config::devtools_hotkey = false`
(e.g. when your app binds F12 itself).

## Performance

### The stance

AffineUI's target is **120 Hz on most systems with modest-sized trees** —
not "eventually smooth", but headroom to spare on ordinary hardware.
Performance regressions are treated as **serious bugs**, on par with
crashes: they get root-caused and fixed at the engine layer, not papered
over in apps.

To be candid about where things stand: **some operations regularly miss
120 Hz today**. Those cases are known, on the bug list, and being worked
through with the same root-cause discipline — the target is the target.
In the meantime, **test your UI with affinetools** to see how it actually
behaves with the renderer: the frame timeline shows you exactly which
interactions are cheap and which currently spike.

The reason the bar is set there: AffineUI aspires to be a **general
game-engine UI system** — in-game UI, not just game/DCC tooling — and a UI
library is only usable inside a game if it is *always* safe to have around:
predictable frame cost, near-zero cost when idle, no hidden work, no frame
spikes. Violations of that are, again, bugs — report them.

### On-demand rendering

AffineUI does not render every frame. A frame paints only when something
is **dirty or animating**; an idle app short-circuits to almost nothing (a
few flag checks). Input and rendering are **queued separately**: every
input event is dispatched immediately through the document — updating
hover, focus, control state, and firing your callbacks — while *painting*
is a separate decision made at most once per display frame from the
accumulated dirty state. A burst of mouse moves costs you event dispatch,
not five repaints. In embedded hosts the same split is explicit:
`dispatch()` everything, then `should_render()`/`render()` once per pulse,
optionally throttled by `set_min_frame_time`.

### How frames stay cheap

- **Retained display lists.** Paint output is recorded as a compact,
  hashable op stream per layer. If a layer's ops hash the same as last
  frame, rasterization is skipped entirely; unchanged frames replay from
  cache.
- **Dirty-rect damage.** Live mutations (an attribute change, a text
  update) invalidate only the affected subtree's rectangles; partial
  rasterization repaints just those regions of the layer.
- **Layers and compositing.** Elements that animate (CSS `transform`,
  `opacity`, `will-change` hints, fixed/scrolling content) are promoted to
  their own compositing layers. Moving a layer re-composites a cached
  texture — a quad blit — instead of re-painting content.
- **CSS computation is engineered, not naive.** Computed styles are
  pre-resolved (no `inherit` chasing at layout time), packed into a
  cache-dense ~256-byte struct, and reference-counted so identical siblings
  share one allocation. Selector matching is allocation-free, restyles are
  scoped to the mutated subtree rather than the document, and attribute
  changes consult a dependency cache so a mutation that *can't* change
  style doesn't trigger one. More is planned — style sharing/dedup across
  the tree and further struct tightening are on the roadmap.
- **Reconciliation is diff-based.** A `set_view` rebuild diffs the
  component tree and emits only net changes; attribute/text-only changes
  take a paint-only path with no layout at all. A clean rebuild emits zero
  operations.

### Memory

The same discipline applies to memory. All engine heap traffic — including
the vendored parsers' DOM and CSS arenas — funnels through **one
allocator**, so a host engine can supply its own memory, budgets are
observable (`mem::stats()`: live bytes/blocks, peak, alloc/free counts),
and debug builds track every block for leak and use-after-free detection on
every platform (`mem::report_leaks()`, allocation tags). Styles are shared
by refcount rather than duplicated per element; rare style data (shadows,
filters, transforms) lives behind an optional pointer so the common case
allocates nothing extra; and paint output is compact tagged ops, not
per-node object graphs. The library itself stays small — the whole engine
is a couple of megabytes.

### Best practices for well-behaved UI

The engine works hard so ordinary UIs are fast by default, but apps decide
which path their updates take. In rough order of importance:

- **Prefer updates that don't cause re-layout.** Attribute, text, class,
  and inline-color changes ride the cheap restyle/paint paths. Structural
  churn (adding/removing/reordering many elements) forces layout — batch it
  into one rebuild rather than dribbling it out.
- **Animate `transform` and `opacity`,** not width/height/margins/insets.
  Transform and opacity animate on the compositor against cached layer
  textures; geometry properties re-run layout every frame.
- **Use the canvas for per-frame geometry.** Anything whose shape changes
  every frame — meters, scopes, cables, drag previews — belongs in a
  custom-paint canvas with `request_custom_repaint`, which repaints without
  touching DOM, styles, or layout. Don't generate SVG or DOM per frame.
- **Keep keys stable.** Stable widget keys let the reconciler match nodes
  across rebuilds; unstable keys turn updates into teardown-and-recreate.
- **Use `virtual_list` for long lists** — it builds only the visible
  window.
- **Toggle visibility, don't rebuild.** Showing/hiding subtrees
  (`display:none`, `hidden`) is a restyle, not a structural edit; the
  engine retains the hidden subtree's layout state.
- **Let idle be idle.** Drive animations from `on_frame` and call
  `invalidate()` only when something actually changed; an `on_frame` that
  does nothing keeps the app in its near-zero idle path.

### Instrumentation

A native perf overlay (`Config::perf_overlay`) and per-frame telemetry
(`App::frame_telemetry()`, JSONL via `AFFINEUI_TELEMETRY=<path>`) are built
in — the telemetry's headline metric is the wall-clock frame gap, which
exposes stalls that per-stage timings hide. All of it — devtools hotkey
included — compiles out with `AFFINEUI_PERF=0`, and when compiled in but
inactive it costs one atomic check per frame. See
[TRACING_AND_PERFORMANCE_LOGGING.md](TRACING_AND_PERFORMANCE_LOGGING.md)
and [HOW_TO_PROFILE.md](HOW_TO_PROFILE.md).

## Language bindings

The component framework, the renderer, docking, themes, and the interaction
layer are identical across **Python** (`pip install affineui`), **Rust**
(`cargo add affineui`), and **C#** (`dotnet add package AffineUI`) — the
widget behavior lives in the C++ core, so nothing is reimplemented per
language. Python binds the C++ API directly; Rust and C# wrap the shared
`affineui_c` C ABI ([LANGUAGE_BINDINGS.md](LANGUAGE_BINDINGS.md)). Each
binding's README carries its own getting-started.

## Where to go next

A good path through `examples/`:

1. [`00_hello`](../examples/00_hello) — smallest program, raw HTML + a click
   handler (renderer layer).
2. [`15_command_panel`](../examples/15_command_panel) — smallest
   component-framework program.
3. [`11_decius_game_editor`](../examples/11_decius_game_editor) — the
   canonical full app: pure component API, docking, undo, safe callbacks,
   no raw HTML.
4. [`16_decius_dender`](../examples/16_decius_dender) — production-shaped
   app with a custom viewport.
5. [`09_embed_d3d11`](../examples/09_embed_d3d11) /
   [`02_hello_sdl`](../examples/02_hello_sdl) — embedding in a host engine.
6. [`17_affine_2600`](../examples/17_affine_2600) — how far custom paint +
   custom skinning can go.

For the complete API surface, continue to [REFERENCE.md](REFERENCE.md).
