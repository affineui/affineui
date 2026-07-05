# affineui-sys

Raw, hand-written FFI declarations for the
[AffineUI](https://github.com/benjcooley/affineui) C ABI
(`affineui_c` shared library).

**EXPERIMENTAL** — the C ABI is versioned (`affineui_c_abi_version()`)
but not yet stable; expect breaking releases while AffineUI is
pre-1.0.

This crate links, but does not build or ship, the native library.
Point the build at a compiled `affineui_c` via the `AFFINEUI_LIB_DIR`
environment variable (the directory containing `affineui_c.lib` /
`libaffineui_c.so` / `libaffineui_c.dylib`); in a repo checkout the
build script finds `build/ninja` automatically. Build the native
library with:

```sh
cmake -S . -B build/ninja -DAFFINEUI_BUILD_C_SHARED=ON
cmake --build build/ninja --target affineui_c
```

Use the [`affineui`](https://crates.io/crates/affineui) crate for the
safe, idiomatic surface.
