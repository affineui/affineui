# affineui-sys

Raw, hand-written FFI declarations for the
[AffineUI](https://github.com/benjcooley/affineui) C ABI
(`affineui_c` shared library).

**EXPERIMENTAL** — the C ABI is versioned (`affineui_c_abi_version()`)
but not yet stable; expect breaking releases while AffineUI is
pre-1.0.

## Build modes

Two modes, gated by cargo features:

### `vendored` (default) — build from source

`cargo add affineui-sys` + `cargo build` just works. The crate ships
a vendored copy of the AffineUI C++ source; `build.rs` invokes cmake
to compile `affineui_c` and links against the resulting shared
library. First build takes ~1–2 min on a laptop (cmake compiles the
whole engine); subsequent builds hit the cargo cache.

Requirements: `cmake` and a C++20 compiler on the build machine
(MSVC on Windows; GCC/Clang elsewhere). Standard fare for `-sys`
crates in the Rust ecosystem.

### `system` — link against an already-built library

Fast build. Requires you to build `affineui_c` yourself and point
the crate at it via `AFFINEUI_LIB_DIR` (the directory containing
`affineui_c.lib` / `libaffineui_c.so` / `libaffineui_c.dylib`).

```toml
[dependencies]
affineui-sys = { version = "…", default-features = false, features = ["system"] }
```

To produce the native library from a repo checkout:

```sh
cmake -S . -B build/ninja -DAFFINEUI_BUILD_C_SHARED=ON
cmake --build build/ninja --target affineui_c
export AFFINEUI_LIB_DIR="$PWD/build/ninja"
```

In a monorepo dev build (this crate consumed from the AffineUI repo
itself), the `system` mode automatically finds `build/ninja/` under
the repo root — no env var needed once cmake has built the target.

## Use the safe wrapper

Use [`affineui`](https://crates.io/crates/affineui) for the safe,
idiomatic surface. This crate only exposes the raw `unsafe` FFI.
