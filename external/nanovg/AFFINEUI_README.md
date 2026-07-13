# affineui_nanovg — AffineUI's NanoVG fork

This repository is a **maintained fork of [NanoVG](https://github.com/memononen/nanovg)** (Mikko Mononen, zlib), kept as part of the [**AffineUI**](https://github.com/affineui/affineui) project — a two-file, zero-dependency C++ HTML/CSS renderer for native apps and games.

It is the **source of truth** for the NanoVG code AffineUI ships. AffineUI doesn't depend on this repo at build time; instead the relevant files in [`src/`](src/) are **vendored** (copied) into `affineui/external/nanovg/`. Develop here, then sync into AffineUI. (See [README.md](README.md) for upstream NanoVG's own documentation.)

## Why this fork exists

Upstream NanoVG is excellent but (a) has been effectively unmaintained since Aug 2023, and (b) its render backends talk **directly** to a graphics API — `nanovg_gl.h` (OpenGL), `nanovg_mtl.m` (Metal), etc.

AffineUI's hard architectural rule is **"sokol only"**: all GPU work goes through [sokol_gfx](https://github.com/floooh/sokol), which abstracts D3D11 / Metal / OpenGL / WebGPU / Vulkan behind one API. No raw GL/D3D/Metal in our code. There is no upstream NanoVG backend for sokol_gfx, so we wrote one and maintain it here, plus a couple of correctness patches AffineUI relies on.

## What's different from upstream

### 1. `src/nanovg_sokol.h` — **new** sokol_gfx render backend
A complete NanoVG render backend on top of sokol_gfx (`nvgCreateSokol()` / `nvgDeleteSokol()`), so the same NanoVG drawing API runs on **every** sokol backend with no raw GL/D3D/Metal. Notable design points (because sokol_gfx differs from raw GL):
- **Hand-written HLSL + GLSL shaders** and a manually-built `sg_shader_desc` (no `sokol-shdc` toolchain — there's only one shader program). MSL/WGSL can be added later.
- **Lazy pipeline cache** replacing NanoVG's per-draw `glStencil*`/`glBlend*` toggling — render state (stencil/blend/color-mask/primitive) is immutable in sokol_gfx pipeline objects, so we pre-build the handful of variants the fill/stroke/AA/cover passes need.
- **CPU fan → triangle-list expansion.** D3D11/Metal/WebGPU have no `TRIANGLE_FAN`, and `base_vertex` isn't universal, so NanoVG fill fans are expanded to triangle lists on the CPU — maximally portable.
- **`sg_view` texture binding** (sokol's modern binding model) and **coalesced font-atlas uploads** (one `sg_update_image` per image per frame, since sokol forbids more).

Usage:
```c
#define NANOVG_SOKOL_IMPLEMENTATION
#include "sokol_gfx.h"     // consumer provides sokol on the include path
#include "nanovg.h"
#include "nanovg_sokol.h"

NVGcontext* vg = nvgCreateSokol(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
```
Like `nanovg_gl.h` expects you to provide OpenGL, `nanovg_sokol.h` expects you to provide sokol_gfx (with a backend selected, e.g. `SOKOL_D3D11`). It must render into a pass that has a **depth-stencil** attachment (the fill algorithm needs stencil).

### 2. `src/fontstash.h` — subpixel glyph advances
Upstream rounds glyph advances to whole pixels (`*x += (int)(adv + 0.5f)`). We keep them **fractional** for accurate text measurement/layout (matters a lot for HTML/CSS line-fitting). The two changed lines are marked `// affineui:`.

### 3. Demo on sokol_gfx
- [`example/example_sokol.c`](example/example_sokol.c) + [`example/_sokol_impl.c`](example/_sokol_impl.c) — renders NanoVG's standard demo screen through the sokol_gfx backend (a full-feature visual test: gradients, paths, images, text/icons, scissor, shadows).
- [`example/demo.c`](example/demo.c) — the GL/GLFW dependency (`saveScreenShot`'s `glReadPixels`) is gated behind `NANOVG_DEMO_NO_GL` so the demo builds with non-GL backends. Upstream GL examples are unaffected.

### 4. `tests/sokol_compile/` — backend smoke test
A tiny program that builds `nanovg_sokol.h` against sokol's **dummy backend** and runs a frame through sokol's validation layer — compile + validation coverage with no GPU or window.

## Relationship to the AffineUI repo
- **This fork** = source of truth for NanoVG + the sokol backend + patches.
- **`affineui/external/nanovg/src/`** = a vendored copy of this fork's `src/` (so AffineUI builds self-contained, no sibling checkout needed).
- Workflow: change here → commit → copy changed `src/` files into `affineui/external/nanovg/src/` → commit AffineUI.

## Building the sokol demo (standalone)
Needs sokol headers on the include path (AffineUI vendors them under `affineui/external/sokol`). Example (MSVC, D3D11):
```
cl /TC /std:c11 /DSOKOL_D3D11 /DSOKOL_NO_ENTRY /DNANOVG_DEMO_NO_GL ^
   /I <sokol> /I src /I example ^
   example/example_sokol.c example/_sokol_impl.c src/nanovg.c example/demo.c ^
   /Fe:nvgdemo.exe /link d3d11.lib dxgi.lib user32.lib gdi32.lib shell32.lib ole32.lib
```
Run it from a directory whose `../example/` holds the demo fonts/images.

## License
NanoVG is zlib-licensed (see [LICENSE.txt](LICENSE.txt)); our additions are under the same terms.
