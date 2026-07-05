# AffineUI Rust bindings

Safe, idiomatic Rust bindings over the AffineUI C ABI
(`docs/LANGUAGE_BINDINGS.md` is the architecture spec).

- `affineui-sys` — raw, hand-written FFI declarations for
  `include/affineui/c_api.h` + `c_api_app.h`, linking the `affineui_c`
  shared library.
- `affineui` — the safe crate: `App`/`View`/`Widget`/`Document`
  (app-owned mode), `embedded::Ui` (host-owned GPU mode), typed
  components, closure callbacks.

## Build the native library first

```powershell
# from the repo root
cmake -S . -B build/ninja -DAFFINEUI_BUILD_C_SHARED=ON
cmake --build build/ninja --target affineui_c
```

`affineui-sys`'s build script finds `build/ninja` automatically in a repo
checkout; outside one, set `AFFINEUI_LIB_DIR` to the directory containing
`affineui_c.lib` / `libaffineui_c.so`.

## Build / run

The build script copies `affineui_c.dll` beside every binary cargo
produces (target dir, examples/, deps/), so no loader-path setup is
needed:

```powershell
cd bindings/rust
cargo build
cargo test -- --test-threads=1          # headless contract tests
cargo run --example hello -- --headless # no window
cargo run --example hello               # native window
cargo run --example component_gallery
```

After rebuilding the native library (`cmake --build ... --target
affineui_c`), `touch affineui-sys/build.rs` (or `cargo clean -p
affineui-sys`) to refresh the copied DLL.

`--test-threads=1` matters: AffineUI is single-threaded by contract and
the handle types are `!Send`, but separate tests would otherwise run on
separate threads concurrently.

## Shape of the API

```rust
use affineui::{App, Config, Theme, View};

let view = View::new(Theme::Decius);
view.build(|v| {
    v.heading(1, "Photo Edit", "", "");
    v.toolbar("main", |v| {
        v.icon_button("save", "save").on_click(|| println!("saved"));
    });
    v.slider("Exposure", 0.5, 0.0, 1.0, "exposure")
        .on_change(|value| println!("exposure = {value}"));
});

let app = App::new(Config::default().title("Demo").size(1280, 800));
app.load_view(&view);
app.run();
```

Embedded mode (host engine owns device/loop/input) lives in
`affineui::embedded` — see the module docs; `init`/`render` are `unsafe`
because they accept raw device/render-target pointers.
