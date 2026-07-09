# affineui

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_game_editor.png" width="720" alt="AffineUI game-editor demo — the default Decius CSS look">

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI rendering a Bootstrap dashboard">

Safe, idiomatic Rust bindings for
[AffineUI](https://github.com/benjcooley/affineui) — a tiny
GPU-accelerated HTML/CSS UI renderer for tools and game engines.
Build UIs with a gradio/imgui-style component tree; AffineUI renders
them natively (D3D11/Metal/GL) with real CSS. No JS, no webview.

**EXPERIMENTAL preview** — published for early testing; APIs and the
underlying C ABI will change while AffineUI is pre-1.0.

---

## Getting started (read all three steps — each one is required)

The crate is a wrapper over a **native shared library** (`affineui_c`)
plus **CSS framework assets**. crates.io ships neither, so a working
setup needs: the crate + the built library + the assets.

### 1. Build the native library (once)

Requires CMake ≥ 3.20 and a C++20 compiler (MSVC 2022 on Windows).
From a clone of the AffineUI repo:

```sh
cmake -S . -B build/ninja -DAFFINEUI_BUILD_C_SHARED=ON
cmake --build build/ninja --target affineui_c
```

This produces `affineui_c.dll` + `affineui_c.lib` (Windows) or
`libaffineui_c.so` / `.dylib` in `build/ninja`.

### 2. Point cargo at it

Create `.cargo/config.toml` **in your project** (adjust the path to
your AffineUI checkout):

```toml
[env]
AFFINEUI_LIB_DIR = "C:\\path\\to\\affineui\\build\\ninja"
```

That's the only wiring needed: the build script links against that
directory **and copies the shared library next to every binary cargo
builds** (including examples and test executables), so `cargo run` and
`cargo test` work with no PATH / LD_LIBRARY_PATH setup.

After rebuilding the native library, refresh the copied DLL with
`cargo clean -p affineui-sys` (or touch anything that rebuilds it).

### 3. Give the app the framework CSS assets

Themes (`Theme::Decius`, `Theme::Bootstrap`) load their stylesheet
from a `frameworks/css/...` path resolved against the app's
`asset_folders`. Those CSS bundles live in the AffineUI repo's
`examples/` directory — pass it (or your own copy of it):

```rust
let app = App::new(
    Config::default()
        .title("My Tool")
        .asset_folders(&[r"C:\path\to\affineui\examples", "."]),
);
```

**If you skip this, the app runs but renders unstyled** (it looks like
a bare web page). Icon fonts and other framework assets resolve from
the same folder.

### Minimal complete program

This exact program ships as the `hello` example
(`cargo run --example hello` in a repo checkout):

```rust
use affineui::{App, Config, Theme, View};

fn main() {
    let view = View::new(Theme::Decius);
    view.build(|v| {
        v.heading(1, "Hello from Rust", "", "");
        v.paragraph("AffineUI — native HTML/CSS, no browser, no JS.", "", "");
        v.button("Click me", true, "go").on_click(|| println!("clicked!"));
        v.slider("Exposure", 0.5, 0.0, 1.0, "exposure")
            .on_change(|value| println!("exposure = {value}"));
        v.checkbox("Enabled", true, "enabled")
            .on_change(|value| println!("enabled = {value}"));
    });

    let app = App::new(
        Config::default()
            .title("AffineUI Rust")
            .size(720, 480)
            .asset_folders(&[r"C:\path\to\affineui\examples", "."]),
    );
    app.load_view(&view);
    std::process::exit(app.run());
}
```

Content views like this get the padded, evenly-spaced page look by
default; the moment you add a root-level `menu_bar` / `toolbar` /
`status_bar` / `document_view`, the window switches to an edge-to-edge
app shell automatically.

### Troubleshooting

| Symptom | Cause / fix |
|---|---|
| link error: cannot find `affineui_c.lib` | `AFFINEUI_LIB_DIR` not set / wrong, or the native library was never built (step 1–2). |
| `STATUS_DLL_NOT_FOUND` (0xc0000135) at run | Stale target dir from before the DLL-copy existed: `cargo clean -p affineui-sys`, rebuild. |
| Window opens but the UI looks like a plain web page | Framework CSS not found — `asset_folders` doesn't contain the `frameworks/css/...` bundles (step 3). |
| panic: "AffineUI C ABI mismatch" | The crate and the built `affineui_c` disagree on ABI version — rebuild the native library from a matching checkout. |
| Widgets render but clicks/changes do nothing | Handlers must be registered on the `Widget` returned by the builder **before** `App::load_view` (load_view copies the view, callbacks included). |

---

## Two modes

- **App-owned** (above) — AffineUI owns the window and the main loop.
- **Embedded** — your engine owns the GPU device, loop, and input;
  AffineUI renders into your render targets. See
  `affineui::embedded::Ui` (D3D11 / Metal / GL / WebGPU handles;
  `init`/`render` are `unsafe`, everything else is safe).

## Contracts

Everything is single-threaded by design (handles are `!Send`).
Callbacks are plain Rust closures, released exactly once; panics in
callbacks are caught at the FFI boundary and never cross into C++.
Widget handles degrade gracefully — reads return defaults and writes
no-op once the underlying node is gone, so stale handles never crash.
