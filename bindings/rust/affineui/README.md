# affineui

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_game_editor.png" width="720" alt="AffineUI game-editor demo — the default Decius CSS look">

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI rendering a Bootstrap dashboard">

Safe, idiomatic Rust bindings for
[AffineUI](https://github.com/affineui/affineui) — a tiny
GPU-accelerated HTML/CSS UI renderer for tools and game engines.
Build UIs with a gradio/imgui-style component tree; AffineUI renders
them natively (D3D11/Metal/GL) with real CSS. No JS, no webview.

**Status:** alpha. Broad standards coverage and real UIs run today, but
expect bugs, and expect the API and the underlying C ABI to move while
AffineUI is pre-1.0.

> **Note:** this release carries no `-alpha` suffix in its version number,
> but AffineUI is alpha and pre-1.0. Features, APIs, and the set of supported
> platforms are all still in flux.
>
> Verified on: **Linux (Ubuntu, x86-64)**, **macOS (Apple Silicon)**, and
> **Windows (x86-64)** — the full suite (600+ tests) passes on each. Other
> platforms and architectures are not yet supported.

---

## Getting started

```sh
cargo add affineui
```

That's the whole setup. The crate vendors the AffineUI C++ source and
the default `vendored` feature builds the native library (`affineui_c`)
automatically via cmake — you need **CMake ≥ 3.20 and a C++20 compiler**
on the machine (MSVC 2022 on Windows), the usual `-sys`-crate convention.
The first build compiles the engine (~1–2 min); after that it's cached.
The build script also copies the shared library next to every binary
cargo produces (examples and test executables included), so `cargo run`
and `cargo test` work with no PATH / LD_LIBRARY_PATH setup.

The default **Decius** framework — CSS, UI fonts, and the icon/symbol
font — is **embedded inside the library itself**. `Theme::Decius` apps
render fully styled with zero on-disk assets and zero configuration.
Opt out with `Config::default().no_bundle_decius(true)` if you want to
serve your own copy via `asset_folders`.

### Linking a pre-built library instead (optional)

If you'd rather skip the vendored build (faster CI, or you build the
engine yourself), disable default features and point the build script
at your library directory:

```sh
cargo add affineui --no-default-features --features system
```

which writes the equivalent of:

```toml
[dependencies]
affineui = { version = "*", default-features = false, features = ["system"] }
```

```toml
# .cargo/config.toml in your project
[env]
AFFINEUI_LIB_DIR = "C:\\path\\to\\affineui\\build\\ninja"
```

Build the library from a repo checkout with
`cmake -S . -B build/ninja -DAFFINEUI_BUILD_C_SHARED=ON` followed by
`cmake --build build/ninja --target affineui_c`.

### Non-default frameworks and your own assets

Only Decius is embedded. `Theme::Bootstrap` (and any assets of your
own — images, extra fonts, custom stylesheets) still resolve against
the app's `asset_folders`; the Bootstrap CSS bundles live in the
AffineUI repo's `examples/` directory:

```rust
let app = App::new(
    Config::default()
        .title("My Tool")
        .asset_folders(&[r"C:\path\to\affineui\examples", "."]),
);
```

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

    // No asset_folders needed — the embedded Decius bundle supplies the
    // CSS, fonts, and icon font.
    let app = App::new(Config::default().title("AffineUI Rust").size(720, 480));
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
| build fails: "The C compiler identification is unknown" / "No CMAKE_C_COMPILER could be found" (Windows, compiler installed) | Usually MSVC's 260-char path limit, not a missing compiler: a deep project directory pushes cmake's probe files past it and `cl.exe` fails with C1083. The build script detects this and relocates the native build to a short `%LOCALAPPDATA%\affineui-sys\…` dir automatically; if you still hit it, set `CARGO_TARGET_DIR` to a short path. (The `LongPathsEnabled` registry switch does **not** help — `cl.exe` isn't long-path aware.) |
| build fails: cmake or a C++ compiler not found | The default `vendored` feature compiles the engine — install CMake ≥ 3.20 + a C++20 toolchain, or use the `system` feature with a pre-built library. |
| link error: cannot find `affineui_c.lib` (`system` feature) | `AFFINEUI_LIB_DIR` not set / wrong, or the native library was never built. |
| `STATUS_DLL_NOT_FOUND` (0xc0000135) at run | Stale target dir from before the DLL-copy existed: `cargo clean -p affineui-sys`, rebuild. |
| `Theme::Bootstrap` (or your own assets) render unstyled | Non-Decius frameworks aren't embedded — `asset_folders` must contain the `frameworks/css/...` bundles. |
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
Callback-provided `View` clones retain an opaque weak token rather than a raw
borrowed pointer. `is_alive()` reports expiry, and a stale View operation
produces a catchable Rust panic before any native View operation is invoked.

---

## Links

- **Repository:** <https://github.com/affineui/affineui>
- **Docs:** <https://github.com/affineui/affineui/tree/main/docs>
- **Issues:** <https://github.com/affineui/affineui/issues>
- **Discussion:** [r/affineui](https://www.reddit.com/r/affineui/) — questions,
  show-and-tell, and general chat
- **License:** MIT
