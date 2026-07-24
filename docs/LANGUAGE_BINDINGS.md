# Language Bindings — Architecture Guide

Status: **C ABI implemented and smoke-tested** — `c_api.h` (embedded
surface + shared types), `c_api_app.h` (app surface), the `affineui_c`
shared-library target (`-DAFFINEUI_BUILD_C_SHARED=ON`), and the ABI
version gate all exist and pass a plain-C headless smoke test (version
gate, null-handle hardening, document layout, view builders, widget
attrs, exactly-once `user_free`). One C-specific naming rule it
surfaced: C has a single identifier namespace, so a getter may not share
its name with an enum typedef — hence `affineui_widget_get_kind` /
`affineui_view_get_theme`.

**Rust wrapper: implemented** (`bindings/rust`, both modes; headless
contract tests pass — including capture-phase consumption, caret timing,
`event_consumed`, and exactly-once closure release — and both examples run;
clippy-clean). **C# wrapper: implemented** (`bindings/csharp`, both modes;
builds warning-free; the headless example verifies the ABI gate, capture
callback round-trip, caret timing, typed-component validity, layout, and
exactly-once GCHandle release). Python's pybind11 suite covers the same
capture/caret/consumption contract plus synthetic IME composition. Windowed
verification of Rust and .NET remains a user-tester step.

This document specifies AffineUI's universal language-bindings system —
one curated C ABI that every wrapper consumes — and the concrete designs
for the first two wrappers built on it (Rust and C#). It defines what
layer each piece lives at and the contracts (ownership, callbacks,
threading, ABI stability, the no-crash rules) every binding shares.
Future bindings (Zig, Odin, Swift, plain C) follow the same recipe: §3 is
the system; §4/§5 are worked examples of applying it. It is the language-bindings counterpart to
`EMBEDDING_DESIGN.md` (host-owned GPU) and follows the same principles as
the Python binding (`bindings/python/README.md`): the C++ core owns the
App, rendering, widget behavior, and callback lists; each language exposes
an idiomatic package **on top of** that core, never a parallel
implementation of it.

---

## 1. Goals and non-goals

**Goals**

- One idiomatic wrapper per language over the same core: Rust crate
  `affineui`, .NET library `AffineUI`, both shaped like the Python package
  (gradio/imgui-style `View` builder, typed components, safe widget
  handles, hard-to-crash callbacks).
- **Both operating modes in both languages:**
  - *App-owned mode* — AffineUI owns the window and main loop
    (`affineui::App` via sokol_app). The mode used by tools written
    directly in Rust/C#.
  - *Embedded mode* — the **host owns the GPU device, window, and loop**
    (`affineui::Ui` + `InitDesc`/`FrameTarget` from `embed.h`). The mode
    used when AffineUI renders inside a game engine or an existing
    renderer. The host passes native device/RTV handles across the FFI as
    opaque pointers and forwards translated input events.
- Preserve the core invariants across the FFI: hard to crash (invalid
  handles no-op, never fault), callbacks that can be dropped safely,
  single-threaded UI thread affinity made explicit in each language's type
  system.
- Zero new runtime dependencies in the core. The bindings layer may use
  language-side tooling (cargo, MSBuild), but the C++ core stays the
  two-file-SDK-shaped library it is.

**Non-goals (for now)**

- No remote/browser transport in the wrappers (same as Python: native
  only, `RemotePatchQueue` plumbing comes later).
- No attempt at cross-language app-framework parity yet (the
  `affineui_app` DCC template sidecar is a separate, later effort — see
  the tri-language sync bookmark).
- No auto-generated bindings from C++ headers. The C ABI is a **curated,
  stable surface**, not a mirror of every C++ signature.

---

## 2. Layering: one C ABI seam, two wrappers

```
                 ┌────────────────────────┐
                 │   C++ core (affineui)  │  App / Ui / Document / View /
                 │                        │  WidgetRef / components / callbacks
                 └───────────┬────────────┘
                             │ (C++ , internal)
                 ┌───────────▼────────────┐
                 │  C ABI  (affineui_c)   │  include/affineui/c_api.h      (embedded surface)
                 │  extern "C", locked    │  include/affineui/c_api_app.h  (app surface)
                 │  enums, POD structs    │  src/c_api.cpp, src/c_api_app.cpp
                 └──────┬─────────┬───────┘
                        │         │
          ┌─────────────▼──┐   ┌──▼──────────────┐
          │ Rust           │   │ C#              │
          │ affineui-sys   │   │ NativeMethods   │  raw FFI (1:1 with the C ABI)
          │ affineui       │   │ AffineUI        │  idiomatic safe layer
          └────────────────┘   └─────────────────┘
```

**Why a C ABI and not per-language C++ bridges:**

- Rust (`cxx`/`bindgen`-on-C++) and C# (C++/CLI) each have C++ bridge
  technologies, but they are per-language, platform-constrained (C++/CLI
  is Windows-only), and would mean maintaining *two* additional binding
  layers against the C++ headers. A single C ABI is one layer, testable
  by itself, and serves every future language (Zig/Odin/ctypes are
  already named in `c_api.h`'s charter).
- The C ABI is also exactly what the game-engine embedding story needs
  (engines consume C ABIs natively), so app-mode bindings and embed-mode
  hosts share one seam. This matters for the "embeddable in game engines"
  mission: backend breadth and interop live behind this one surface.
- **Python remains on pybind11 — decided.** It predates this work,
  pybind is the right tool there (rich object protocol, keep_alive, GIL
  handling), and its conventions are the *behavioral* reference for the
  new wrappers. Python is deliberately NOT being migrated to the C ABI;
  the two binding paths coexist, and behavioral parity between them is
  maintained at the spec level (this document + the Python README), not
  by sharing a layer. Python build callbacks receive a `CallbackView`
  backed directly by the same native weak-lifetime token: it becomes
  false when expired and raises `ReferenceError` on stale View operations.

**Build artifact:** a new CMake target `affineui_c` — a SHARED library
(`affineui_c.dll` / `libaffineui_c.so` / `libaffineui_c.dylib`) that
compiles `src/c_api.cpp` + `src/c_api_app.cpp` with `AFFINEUI_C_BUILD_DLL`
defined (dllexport on Windows) and links the static core. Gated by a new
option `AFFINEUI_BUILD_C_SHARED` (default OFF; the static-lib C API remains
available under the existing `AFFINEUI_ENABLE_C_API`). Both wrappers load
this one artifact.

---

## 3. The C ABI contract

### 3.1 Files and naming

| File | Surface |
|---|---|
| `include/affineui/c_api.h` | Embedded mode (`affineui_ui_*`): init against host GPU, render into host targets, dispatch, live DOM mutation. Also the **shared value types**: `affineui_event`, key/mouse/event-type enums, `affineui_string_free`, `affineui_user_free_fn`. |
| `include/affineui/c_api_app.h` | App mode: `affineui_app_*`, `affineui_document_*`, `affineui_view_*`, `affineui_widget_*`. Includes `c_api.h` for the shared types. |

Conventions (existing, kept):

- Functions: `affineui_<object>_<verb>` (`affineui_view_button`,
  `affineui_widget_on_click`).
- Opaque handles: `typedef struct affineui_app affineui_app;` etc. —
  `affineui_app`, `affineui_document`, `affineui_view`, `affineui_widget`,
  `affineui_ui`.
- Booleans are `int` (0/1); out-params are pointers; every function
  null-checks every handle/pointer and **no-ops on null** (the C-level
  expression of the hard-to-crash invariant).

### 3.2 Enums are ABI-locked

C enums mirror the C++ enums **by explicit value** (`AFFINEUI_KEY_A = 12`,
…). The implementation carries `static_assert`s pinning the C++ values to
the C values so a reordering in `types.h`/`view.h` breaks the build, not
the ABI. New enumerators append; values never change meaning.

### 3.3 Strings

- **In:** UTF-8, null-terminated `const char*`. `NULL` means "empty" and
  is always accepted.
- **Out:** heap copies (`malloc`) returned as `char*`; the caller frees
  with `affineui_string_free`. No borrowed `string_view`s cross the FFI —
  a returned pointer is never invalidated by a later API call. (Small,
  cold paths; UI mutation rates make the copies irrelevant.)
- Lists in: `const char* const* items, size_t count` (dropdown options,
  swatches, asset folders).

### 3.4 Callbacks: fn pointer + user data + destructor

Every callback registration takes the triple:

```c
typedef void (*affineui_user_free_fn)(void* user);
typedef void (*affineui_click_fn)  (void* user);
typedef void (*affineui_change_fn) (void* user, const char* value);
typedef void (*affineui_build_fn)  (void* user, affineui_view* view);

void affineui_widget_on_click(affineui_widget* w,
                              affineui_click_fn fn,
                              void* user,
                              affineui_user_free_fn user_free);
```

- The C++ side wraps `(fn, user, user_free)` in a
  `std::shared_ptr<holder>` whose deleter calls `user_free(user)` —
  **exactly once**, when the core drops its last reference to the handler
  (view cleared, handler replaced, view destroyed). This is what lets
  Rust box a closure and C# pin a `GCHandle` with no leaks and no
  use-after-free. It is the C-level analog of the Python binding's
  `keep_python_function` + GIL-safe deleter.
- Scope and append/replace callbacks (`affineui_build_fn`) are invoked
  synchronously; persistent App builders and deferred docking/list builders
  may be invoked later. In every case, the `affineui_view*` they receive is
  borrowed and only valid for the duration of that invocation. A wrapper
  whose callback object can escape must call `affineui_view_weak_ref`
  during the callback, retain only that opaque token, and resolve it with
  `affineui_weak_view_get` immediately before each operation. A null
  resolution becomes the language's normal stale-object failure
  (`ObjectDisposedException`, a catchable Rust panic, or Python
  `ReferenceError`); it is never passed on as a native View pointer.
- Callbacks must never unwind across the FFI. Each wrapper guarantees it
  on its side (Rust `catch_unwind`, C# catch-all — see §4/§5); this
  mirrors `call_python_function`'s catch-everything contract.

### 3.5 Ownership and lifetime rules

| Handle | Created by | Destroyed by | Notes |
|---|---|---|---|
| `affineui_app` | `affineui_app_create(cfg)` | `affineui_app_destroy` | `cfg` is a POD struct; `affineui_app_config_init()` fills defaults (sokol-style). |
| `affineui_ui` | `affineui_ui_create` | `affineui_ui_destroy` | Embedded mode. |
| `affineui_document` | **borrowed** from `affineui_app_document(app)`; or owned via `affineui_document_create` (headless) | owned: `affineui_document_destroy`; borrowed: never | A borrowed document is valid exactly as long as its app. |
| `affineui_view` | `affineui_view_create(theme)` | `affineui_view_destroy` | `affineui_app_load_view` **copies** the view into the app (same as Python: the app never borrows the caller's view object). |
| `affineui_weak_view` | `affineui_view_weak_ref(view)` while `view` is known live | `affineui_weak_view_destroy` | Opaque invalidating token for callback Views. It neither retains the View nor exposes C++ layout/offsets; `get` returns `NULL` after destruction. |
| `affineui_widget` | returned by every `affineui_view_*` builder / `find_widget` | `affineui_widget_destroy` | A heap-copied `WidgetRef`. **Must not outlive its view** — the C header documents it; each wrapper *enforces* it (§4.2, §5.2). Operations on a stale-but-in-lifetime ref follow WidgetRef semantics: reads return defaults, writes no-op. |

#### Capture-phase input interception

`affineui_app_on_event_capture` and `affineui_ui_on_event_capture` receive
every translated event sent through the corresponding dispatcher, including
mouse move/down/up/wheel, key down/up, text input, and IME composition. A
non-zero return consumes the event before document/widget dispatch and before
ordinary event handlers. This lets a host implement global shortcuts, modal
tools, or an input grab without focused controls also acting on the event.

Consumption is local to AffineUI; it does not cancel the underlying operating
system event or stop another host subsystem from seeing it. A physical IME
keystroke can produce later composition and committed-text events, so a host
that suppresses the complete sequence must consume those events as well. The
capture callback receives the current **pre-dispatch** hover chain; ordinary
post-document handlers receive the refreshed chain after hit testing.

### 3.6 Threading

The whole surface is single-threaded: create, mutate, render, and dispatch
on one thread (the thread that owns the graphics context in embedded
mode). The C header states it; the wrappers encode it (Rust: `!Send +
!Sync`; C#: documented thread affinity + debug-mode thread checks).

### 3.7 App-surface function inventory (slice 1)

Mirrors the Python binding's proof-of-concept surface:

- **App** — create/destroy, `load_html`, `load_html_file`, `load_view`,
  `set_stylesheet(css, base_url /*nullable*/)`, `invalidate`,
  `set_perf_overlay_enabled`/`perf_overlay_enabled`, `dispatch(event)`,
  `on_event_capture(fn,user,user_free)`, `run`, `quit(code)`, `window_size`,
  `framebuffer_size`, `dpi_scale`, `document` (borrow).
- **Document** (headless + via app) — create/destroy, `set_html`,
  `set_user_stylesheet(css, base_url)`, `reload_stylesheets`,
  `layout(w,h)`, `content_size`, `set_attribute_by_id`,
  `remove_attribute_by_id`, `set_text_by_id`, `dispatch(event, out
  result)` (`event_consumed` included), caret blink interval get/set/tick,
  `attach_script`/`detach_script`, `hovered_cursor`.
- **View** — create/destroy(theme), `clear`, `begin`/`end`, `set_theme`,
  `set_framework_version`, `selector`, `to_html_fragment`,
  `to_html_document`, `find_widget`; builders: heading, paragraph, text,
  html, button, checkbox, toggle, input, password, textarea, dropdown,
  button_group, slider, knob, combo, color_field/colorfield; scope
  builders (immediate `affineui_build_fn`): container, element, panel,
  card, toolbar, menu_bar, status_bar, tree, foldout; leaf structurals:
  toolbar_separator, icon_button, menu_button(+build), menu_item,
  menu_separator, submenu, menu_brand, menu_spacer, menu_meta, tree_row,
  splitter.
- **Widget** — destroy, `valid`, `kind` (for typed-component checks),
  `name`, `attr`, `text`, `has_attr`, `set_text`, `set_attr`,
  `remove_attr`, `set_selector`, `add_class`, `clear`, `on_click`,
  `on_change`, `append(build)`, `replace(build)`, `find_widget`.

**Deliberately deferred** (slice 2+): declarative docking
(`document_view`/`dockpanel`/`DockLocation` need a POD encoding designed
against the retained dock-tree model), `virtual_list` (per-item build
callback — straightforward, just not slice 1), `RemotePatchQueue`,
automation, weak `DomHandle` round-tripping.

**Typed components** (Button/Checkbox/TextField/…) get **no C entry
points**. `components.h` wrappers are header-only sugar over
`WidgetRef` attrs (`aria-checked`, `data-value`, …) plus a kind check —
each language reimplements that thin layer natively (Rust
`view.checkbox_at("x") -> Checkbox`, C# `view.CheckboxAt("x")`) using
`affineui_widget_kind` + attr get/set. This keeps the ABI small and the
per-language layer idiomatic; the attribute names are part of the
framework contract, already shared with the interaction layer.

### 3.8 Embedded-surface additions

Already present: init/render/needs_update/mark_dirty/reset/set_html/css.
Added for a usable embedded host (all thin over `affineui::Ui`):
`dispatch(event)`, `on_click(selector, fn, user, user_free)`,
`on_event_capture(fn, user, user_free)`, `hovered_cursor`, caret blink-rate
configuration, `set_attr`/`remove_attr`/`set_text`, `content_size`, `load_file`.
GPU handles stay opaque `const void*` in
`affineui_gpu_context`/`affineui_frame_target` exactly as designed in
`EMBEDDING_DESIGN.md` (all-or-nothing ownership; AffineUI opens/ends its
own pass into host views and never presents).

### 3.9 ABI versioning

- `affineui_version()` already exists. Add `AFFINEUI_C_ABI_VERSION`
  (integer, bumped on any breaking change) and
  `int affineui_c_abi_version(void)`; each wrapper checks it at load and
  fails fast with a clear message rather than corrupting memory.
- Additive-only evolution: new functions and appended enum values are
  fine. A struct-layout change requires an ABI bump and coordinated wrapper
  update. ABI v2 appends `event_consumed` to `affineui_dispatch_result` and
  adds event-capture/caret-blink entry points; Rust and .NET both require v2.
  Future config growth should use a `_v2` or sized-struct pattern.

---

## 4. Rust wrapper (`bindings/rust/`)

### 4.1 Crate layout

```
bindings/rust/
├── Cargo.toml            # workspace
├── affineui-sys/         # raw FFI, #![no_std]-compatible declarations
│   ├── build.rs          # locate/link affineui_c
│   └── src/lib.rs        # hand-written extern "C" decls + POD structs
├── affineui/             # safe, idiomatic crate
│   ├── src/lib.rs        # App, View, Widget, Document, Event, components
│   └── examples/         # hello.rs, component_gallery.rs, embedded_wgpu.rs (later)
└── README.md
```

- **`affineui-sys` is hand-written, not bindgen-generated.** The ABI is
  small, curated, and locked by explicit enum values; hand-written decls
  mean no libclang build dependency on Windows and reviewable diffs. A
  dev-only `cargo test` in `-sys` can optionally run bindgen to *verify*
  drift when libclang is present, but generation is never in the build
  path.
- **Linking (`build.rs`)**, in priority order:
  1. `AFFINEUI_LIB_DIR` env var → `cargo:rustc-link-search` +
     `cargo:rustc-link-lib=dylib=affineui_c`.
  2. Well-known relative dev path (`../../build/ninja` from the repo
     layout) so `build.ps1` + `cargo run --example hello` just works in a
     checkout.
  3. (later) `build-from-source` cargo feature using the `cmake` crate.

### 4.2 Safe-layer object model

```rust
let app = App::new(Config::default().title("Demo").size(1280, 800))?;
let view = View::new(Theme::Decius);
view.build(|v| {
    v.heading(1, "Photo Edit", "", "");
    v.toolbar("main", |v| {
        v.icon_button("save", "save").on_click(|| println!("saved"));
    });
    v.slider("Exposure", 0.5, 0.0, 1.0, "exposure")
        .on_change(|val| println!("exposure = {val}"));
});
app.load_view(&view);
app.run();
```

- **Invalidating widget handles.** `View` owns its native View; `Widget` owns
  only the C ABI's heap-copied weak `WidgetRef`. A Widget does not retain its
  View and becomes inert after View destruction; `Drop` order is safe. `App`, `View`,
  `Widget`, `Ui` all embed `PhantomData<*const ()>` → `!Send + !Sync`,
  encoding the thread-affinity contract at compile time.
- **Closures:** `on_click(impl FnMut() + 'static)` boxes the closure,
  passes `Box::into_raw` as `user`, an `extern "C"` trampoline as `fn`,
  and a drop-trampoline as `user_free`. Every trampoline body is wrapped
  in `std::panic::catch_unwind` (a panic aborts the callback with a log
  line, never unwinds into C++). `'static` is required because the core
  may hold the handler until the view is destroyed.
- **Capture:** `App::on_event_capture` and `embedded::Ui::on_event_capture`
  expose the complete pre-widget event phase. Returning `true` consumes mouse,
  keyboard, text, or composition input. `Document::dispatch` exposes
  `DispatchResult::event_consumed`; Document and embedded Ui expose caret blink
  interval configuration (custom Document drivers also call `tick_caret_blink`).
- **Builder scopes** are closures (`v.container("cls", "key", |v| …)`) —
  same shape as the Python binding's `build=` parameters; no RAII scope
  object crosses the FFI. Callback-provided `View` clones retain an opaque
  native weak token rather than the borrowed pointer. They expose
  `is_alive()` and panic before forwarding a stale pointer to any native
  View operation.
- **Strings:** `&str` → `CString` at the boundary (interior NULs
  rejected with a clear panic-free error); returned `char*` → owned
  `String` then `affineui_string_free`.
- **Typed components:** `Button`, `Checkbox`, `TextField`, `Dropdown`,
  `Slider`, `ColorField`, `DockPanel`, `Foldout` structs wrapping
  `Widget` + a `Validity` enum (`Valid/WrongType/NotPresent`), gated by
  `affineui_widget_kind` — a faithful port of `components.h` semantics
  including the inert-when-invalid contract.
- **Errors:** the core never signals recoverable errors on this surface
  (hard-to-crash = degrade, don't fail), so the API is largely
  infallible; fallible spots (`load_html_file`) return `bool`-like
  `Result<(), LoadError>`.

### 4.3 Embedded mode (Rust)

Module `affineui::embedded`:

```rust
let mut ui = embedded::Ui::new();
unsafe {
    ui.init(&InitDesc {
        gpu: Some(GpuContext::d3d11(device_ptr, context_ptr)
                    .color_format(PixelFormat::Bgra8)),
        ..Default::default()
    });
}
// per frame, inside the host's render loop:
if ui.needs_update() { /* host may skip otherwise */ }
ui.render(&FrameTarget::d3d11(rtv, dsv, w, h, dpi));
ui.dispatch(&event);
```

`init` and `render` are `unsafe` (the caller vouches that raw device/view
pointers are live and match the compiled backend); everything else is
safe. This is the natural surface for wgpu/ash/windows-rs hosts and is
why the embed C API exists at all — Rust game engines are its primary
audience.

---

## 5. C# wrapper (`bindings/csharp/`)

### 5.1 Project layout

```
bindings/csharp/
├── AffineUI/
│   ├── AffineUI.csproj        # net8.0; AllowUnsafeBlocks; source-gen P/Invoke
│   ├── NativeMethods.cs       # [LibraryImport("affineui_c")] — 1:1 with the C ABI
│   ├── App.cs  View.cs  Widget.cs  Document.cs  Types.cs  Components.cs
│   └── Embedded/Ui.cs  GpuContext.cs  FrameTarget.cs
├── examples/
│   ├── Hello/                 # app-owned mode console app
│   └── EmbeddedHost/          # (slice 2) D3D11 host via Silk.NET or TerraFX
└── README.md
```

- `net8.0`, `[LibraryImport]` source-generated P/Invoke (no runtime
  marshalling surprises, AOT-friendly), UTF-8 string marshalling
  (`StringMarshalling.Utf8`).
- **Native library resolution:** `NativeLibrary.SetDllImportResolver`
  checks, in order: `AFFINEUI_NATIVE_DIR` env var → app base directory →
  NuGet `runtimes/<rid>/native` (default probing). Dev loop: the example
  csproj copies `affineui_c.dll` from the CMake build dir via an
  `AffineUINativeDir` MSBuild property.

### 5.2 Object model

```csharp
using var app = new App(new AppConfig { Title = "Demo", Width = 1280, Height = 800 });
using var view = new View(Theme.Decius);
view.Build(v => {
    v.Heading(1, "Photo Edit");
    v.Toolbar("main", v2 => {
        v2.IconButton("save", key: "save").OnClick(() => Console.WriteLine("saved"));
    });
    v.Slider("Exposure", 0.5, 0, 1, key: "exposure")
     .OnChange(val => Console.WriteLine($"exposure = {val}"));
});
app.LoadView(view);
app.Run();
```

- **Handles:** `SafeHandle` subclasses for owned handles (`App`, `View`,
  `Document`-owned, `Ui`, `Widget`) — correct finalization even under
  thread aborts. Borrowed documents wrap the raw pointer and hold a
  strong reference to their `App` (never finalized).
- **Widget → View lifetime:** a `Widget` does not keep its `View` alive. The
  native invalidating handle returns defaults/no-ops after View collection or
  disposal, and the Widget can still be disposed independently.
- **Callback View lifetime:** every `Action<View>` receives a wrapper over an
  opaque native weak token, never a retained raw pointer. `View.IsAlive`
  becomes false when its framework owner dies; further View operations throw
  `ObjectDisposedException` before invoking a native View operation.
- **Callback lifetime — the load-bearing detail:** managed closures are
  held via `GCHandle.Alloc(closure)`; `user` = `GCHandle.ToIntPtr`;
  `fn` = a single static `[UnmanagedCallersOnly]` trampoline per
  callback shape; `user_free` = a static trampoline that calls
  `GCHandle.Free`. The core's exactly-once `user_free` contract (§3.4)
  makes this leak-free without any "keep delegates in a list" bookkeeping
  — the unmanaged function pointers are static methods, immune to GC.
- **Exceptions never cross the FFI:** every trampoline wraps the managed
  callback in `try/catch`, reports via an overridable
  `AffineUIRuntime.OnCallbackException` (default: `Console.Error`), and
  returns — the direct analog of the Python binding's
  `call_python_function`.
- **Idioms:** properties where Python used properties
  (`checkbox.Checked`, `button.Enabled`, `foldout.Open`), `Action`/
  `Action<string>` for handlers, `Action<View>` build callbacks, typed
  components with `Validity` enum, `bool` implicit-truth replaced by an
  explicit `IsValid` property.
- **Capture and editor state:** `App.OnEventCapture` and
  `Embedded.Ui.OnEventCapture` receive every translated input event and return
  `bool` to consume it. `DispatchResult.EventConsumed` reports focused-widget
  handling. `Document.CaretBlinkInterval`/`TickCaretBlink()` and
  `Embedded.Ui.CaretBlinkInterval` mirror the C++ timing surface.

### 5.3 The two C# modes (explicit requirement)

**Mode A — C# owns the system** (`AffineUI.App`): AffineUI creates the
native window and runs the loop on the calling thread; `App.Run()` blocks
like the Python `app.launch(native=True)`. For standalone C# tools.

**Mode B — embedded, host owns the UI system** (`AffineUI.Embedded.Ui`):
the surrounding engine (a C++ engine hosting .NET, Unity native plugin,
a Silk.NET/TerraFX renderer, MonoGame) owns device, swapchain, loop, and
input pump. C# supplies:

```csharp
var ui = new Embedded.Ui();
ui.Init(new InitDesc {
    Gpu = GpuContext.D3D11(devicePtr, contextPtr,
                           color: PixelFormat.Bgra8, sampleCount: 1),
});
ui.SetHtml(html);            // or ui.LoadView(view) once slice 2 lands
// per frame, called by the host:
ui.Render(FrameTarget.D3D11(rtvPtr, dsvPtr, width, height, dpiScale));
bool consumed = ui.Dispatch(in evt);   // host suppresses its own input if true
```

All GPU handles are `IntPtr` (opaque, exactly as the C ABI defines them);
`Render` is called from inside the host's frame with the documented
state-clobber contract from `EMBEDDING_DESIGN.md`. `NeedsUpdate` lets an
on-demand host skip idle frames. Nothing in Mode B touches sokol_app —
no window, no loop, no input hooks — matching the all-or-nothing
ownership rule.

Both modes ship in the same assembly; they share `Event`, `View`,
`Document`, and component types, so an app can be developed windowed
(Mode A) and dropped into an engine (Mode B) without rewriting UI code.

---

## 6. Build & packaging

- **Native:** `build.ps1 bindings-native` (new verb) → configures with
  `-DAFFINEUI_BUILD_C_SHARED=ON` and builds `affineui_c.dll` into
  `build/ninja`. CI later produces per-platform artifacts.
- **Rust:** `cargo build` in `bindings/rust` (picks up the dev DLL path
  automatically and copies it beside every built binary). Crates
  `affineui` / `affineui-sys` carry full publish metadata (0.0.1,
  marked EXPERIMENTAL) and pass `cargo package`; crates.io cannot host
  the native binary, so `-sys` requires `AFFINEUI_LIB_DIR` outside a
  repo checkout until a vendored build-from-source feature lands (note:
  the dist/ amalgamation is clang-only on Windows, which gates that
  feature).
- **C#:** `dotnet build bindings/csharp/AffineUI`. `dotnet pack`
  produces `AffineUI.0.0.1-alpha.nupkg` with the managed assembly +
  `runtimes/win-x64/native/affineui_c.dll` (pack fails fast if the
  native DLL is missing); verified end-to-end by installing from a
  local feed into a fresh app. Other RIDs come from CI builds later.

---

## 7. Testing strategy

- **C ABI tests** (doctest, in `tests/`): headless — create a document,
  set html/css, layout, content_size; build a view, `to_html_fragment`
  golden check; register a callback with a counting `user_free` and
  verify exactly-once release on view destroy. These pin the contract
  both wrappers rely on, independent of either language toolchain.
- **Rust:** `cargo test -- --test-threads=1` — headless document + view
  contracts, capture callback payload/consumption, caret interval and
  `event_consumed`, plus closure drop-count checks (`user_free` exactly once).
- **C#:** `dotnet run --project bindings/csharp/examples/Hello -- --headless`
  is the executable contract test: ABI gate, headless layout, typed components,
  capture callback payload/consumption, caret interval, and exactly-once
  `GCHandle` release. There is no separate xUnit project yet.
- **Python:** `pytest bindings/python/tests` builds/loads the pybind11 module
  and covers capture callback payload/consumption, caret timing,
  `DispatchResult.event_consumed`, and synthetic UTF-8 IME preedit/commit.
- **Windowed verification** stays a user-tester step (per project
  practice): the wrapper hello apps opening a native window and
  responding to clicks is the acceptance gate before either wrapper is
  called "done".

---

## 8. Phasing

| Slice | Contents |
|---|---|
| **1** | C ABI app surface (§3.7) + `affineui_c` target; Rust `-sys` + safe crate with App/View/Widget/Document + closures; C# NativeMethods + App/View/Widget/Document; `hello` + `component_gallery` examples in both; headless tests. Embedded-mode input additions to `c_api.h` (done). |
| **2** | Embedded mode wrappers (`embedded::Ui` / `Embedded.Ui`) + a real D3D11 host example in each language; typed components; virtual_list; ABI version gate; `build.ps1 bindings-native`. |
| **3** | Declarative docking across the ABI (DockLocation POD encoding, providers), workspace persistence hooks, RemotePatchQueue, NuGet/crates packaging, CI artifacts. |

---

## 9. Open questions (flagged, not blocking slice 1)

1. **Dock API encoding** — `DockLocation` + the three provider callbacks
   (`size/placement/layout`) are the one C++ surface that doesn't map
   trivially to POD. Proposal: defer until the retained dock-tree model
   (per `decius-js-docking-algorithm`) lands, then expose the *model*
   (serializable layout tree) rather than the provider closures.
2. **Per-frame view functions** (`App::mount` imm mode) — deliberately
   out of scope for both wrappers; the retained `View` + `load_view` path
   is the bindings story, matching Python.
3. **C# `Run()` + callbacks re-entrancy** — Mode A blocks the calling
   thread while native code calls back into managed handlers; this is
   fine for .NET (unlike Python's GIL discussion) but deserves a stress
   test with a high-frequency `on_change` (slider drag) before slice 1
   is declared verified.
4. **Headless view attachment** (found during C# verification) — views
   only reach a document via `affineui_app_load_view`, so a widget
   callback round-trip cannot be exercised without opening a window. A
   headless attach (e.g. `affineui_document_load_view`, pending the C++
   core exposing the same) would let every binding CI-test the full
   click path. Until then, bindings verify callback registration +
   exactly-once release instead.
