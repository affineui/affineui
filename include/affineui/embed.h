#pragma once

// affineui embedding API — drive AffineUI from a host (game engine) that
// already owns the core graphics-API objects and its own frame loop.
//
// Two ownership modes, all-or-nothing:
//
//   • Standalone — AffineUI creates and owns the device, swapchain, window,
//     and frame loop (see affineui::App / affineui::run). Leave
//     InitDesc::gpu null.
//
//   • Embedded   — the host owns the device and the present/flip. You hand
//     AffineUI a *complete* GpuContext once (Ui::init), then hand it the
//     render-target views for each frame (Ui::render(const FrameTarget&)).
//     AffineUI opens its own sokol_gfx pass into those views, draws, and
//     ends+commits — it never presents.
//
// "All-or-nothing" means exactly that: either you provide every field of
// GpuContext for your backend, or you provide no GpuContext at all.
// Partially-filled contexts are rejected at init.
//
// State contract (D3D11 / GL): AffineUI renders inside its own sokol_gfx
// pass, which clobbers pipeline / device-context state. Treat your GPU
// state as undefined after Ui::render(FrameTarget) returns and rebind what
// you need — the same contract Dear ImGui's renderers use.
//
// All graphics handles are opaque `void*` so this header pulls in no
// D3D11 / Metal / GL / sokol headers. Hand over the native objects:
//   D3D11  device          = ID3D11Device*
//          device_context  = ID3D11DeviceContext*
//          render_view     = ID3D11RenderTargetView*
//          depth_stencil_view = ID3D11DepthStencilView*   (optional)
//   Metal  device          = id<MTLDevice>
//          current_drawable= CAMetalDrawable               (per frame)
//   GL     (no handles; your GL context must be current on this thread)
//          framebuffer     = GL framebuffer object name    (0 = default)
//
// Threading: single-threaded, like the rest of AffineUI. Call init/render
// on the thread that owns the graphics context.

#include "affineui/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace affineui {

/// Concrete GPU backend. Must match the backend AffineUI was compiled for
/// (AFFINEUI_BACKEND_* / SOKOL_*). Mismatches are rejected at init.
enum class Backend {
    d3d11,
    metal,
    gl,
    wgpu,
};

/// Texture formats for the host's color / depth-stencil targets. `default_`
/// lets AffineUI pick the platform-typical swapchain format (BGRA8 color,
/// DEPTH_STENCIL depth) — fine for the common case.
enum class PixelFormat {
    default_,
    rgba8,
    bgra8,
    depth,
    depth_stencil,
};

enum class DebugOverlayCorner {
    top_left,
    top_right,
    bottom_right,
    bottom_left,
};

struct DebugOverlayBounds {
    int x{0};
    int y{0};
    int w{330};
    int h{132};
};

inline DebugOverlayCorner next_debug_overlay_corner(DebugOverlayCorner corner) {
    switch (corner) {
        case DebugOverlayCorner::top_left:
            return DebugOverlayCorner::top_right;
        case DebugOverlayCorner::top_right:
            return DebugOverlayCorner::bottom_right;
        case DebugOverlayCorner::bottom_right:
            return DebugOverlayCorner::bottom_left;
        case DebugOverlayCorner::bottom_left:
            return DebugOverlayCorner::top_left;
    }
    return DebugOverlayCorner::top_right;
}

inline DebugOverlayBounds debug_overlay_bounds(
        int logical_w,
        int logical_h,
        DebugOverlayCorner corner,
        int box_w = 330,
        int box_h = 132,
        int margin = 10) {
    DebugOverlayBounds b{};
    b.w = box_w;
    b.h = box_h;
    const int right = logical_w > 0 ? (logical_w - box_w - margin) : margin;
    const int bottom = logical_h > 0 ? (logical_h - box_h - margin) : margin;
    switch (corner) {
        case DebugOverlayCorner::top_left:
            b.x = margin;
            b.y = margin;
            break;
        case DebugOverlayCorner::top_right:
            b.x = right > margin ? right : margin;
            b.y = margin;
            break;
        case DebugOverlayCorner::bottom_right:
            b.x = right > margin ? right : margin;
            b.y = bottom > margin ? bottom : margin;
            break;
        case DebugOverlayCorner::bottom_left:
            b.x = margin;
            b.y = bottom > margin ? bottom : margin;
            break;
    }
    return b;
}

inline bool debug_overlay_contains(const DebugOverlayBounds& bounds,
                                   int x,
                                   int y) {
    return x >= bounds.x && y >= bounds.y &&
           x < bounds.x + bounds.w && y < bounds.y + bounds.h;
}

/// The host's core graphics objects, supplied once at init in embedded
/// mode. Fill the sub-struct for your backend completely. Handles are
/// borrowed: AffineUI never creates or destroys them.
struct GpuContext {
    Backend backend = Backend::d3d11;

    struct {
        const void* device         = nullptr;  // ID3D11Device*
        const void* device_context = nullptr;  // ID3D11DeviceContext*
    } d3d11;

    struct {
        const void* device = nullptr;          // id<MTLDevice>
    } metal;

    struct {
        const void* device = nullptr;          // WGPUDevice
    } wgpu;

    // gl: no handles — the host's GL context must be current on the
    // calling thread when init/render run.

    /// Formats AffineUI's pipelines should target. Must match the formats
    /// of the render targets you pass to render() each frame.
    PixelFormat color_format = PixelFormat::default_;
    PixelFormat depth_format = PixelFormat::default_;
    int         sample_count = 1;  // MSAA sample count of the targets
};

/// Host memory allocator. When supplied, AffineUI routes its allocations
/// through it (see docs/EMBEDDING_DESIGN.md §6). Null = default malloc/new.
/// `align` is the requested alignment in bytes (power of two).
struct Allocator {
    void* (*alloc)  (size_t size, size_t align, void* user)             = nullptr;
    void* (*realloc)(void* p, size_t old_sz, size_t new_sz, size_t align, void* user) = nullptr;
    void  (*free)   (void* p, void* user)                               = nullptr;
    void*  user                                                         = nullptr;
};

/// Severity for the log hook.
enum class LogLevel { debug, info, warn, error };

/// Host log sink. AffineUI's warnings/errors + parse diagnostics route here
/// instead of stderr. Null = default (stderr in debug, silent otherwise).
using LogFn = void (*)(LogLevel level, const char* msg, void* user);

/// One-time initialization for embedded mode.
struct InitDesc {
    /// Host graphics objects. Non-null + complete → embedded mode (AffineUI
    /// uses the host's objects). Null → standalone (AffineUI owns the GPU).
    const GpuContext* gpu = nullptr;

    /// Optional resource loader for url-referenced assets (CSS, images).
    ResourceLoader resource_loader{};

    /// Optional host allocator (see §6). When set, AffineUI allocates from it.
    const Allocator* allocator = nullptr;

    /// Optional host log sink + its user pointer.
    LogFn log      = nullptr;
    void* log_user = nullptr;

    /// Default font + size used when CSS doesn't specify one.
    std::string default_font_family = "sans-serif";
    int         default_font_size   = 16;
};

/// The host's render-target views for the current frame. AffineUI brackets
/// a sokol_gfx pass around these, draws the UI, and ends+commits. The host
/// owns the views' lifetime and performs the present/flip afterwards.
struct FrameTarget {
    int   width     = 0;     // target width in pixels
    int   height    = 0;     // target height in pixels
    float dpi_scale = 1.0f;  // pixels per CSS point (1.0, 2.0 Retina, ...)
    int   sample_count = 1;  // must match the target's MSAA sample count

    /// Clear the target to the Ui's clear color before drawing. Set false
    /// to draw the UI over content the host already rendered into it.
    /// (A sub-rect viewport forces draw-over: the pass clear is whole-target,
    /// so sub-rect panels ignore `clear` and rely on the body background.)
    bool  clear = true;

    /// Commit the sokol_gfx frame after drawing. Embedded hosts normally
    /// leave this true. Windowing adapters that need to draw native overlays
    /// after AffineUI can set false, issue their extra passes, then call
    /// sg_commit() once at the end of the frame.
    bool  commit = true;

    /// Optional sub-rect of the target to draw into, in physical pixels.
    /// All-zero = the whole target. The document lays out at
    /// viewport.w / dpi_scale by viewport.h / dpi_scale CSS points, so
    /// high-DPI rendering increases raster resolution without changing
    /// authored CSS sizes. See docs/EMBEDDING_DESIGN.md §2.2.
    struct { int x = 0, y = 0, w = 0, h = 0; } viewport;

    struct {
        const void* render_view        = nullptr;  // ID3D11RenderTargetView*
        const void* resolve_view       = nullptr;  // ID3D11RenderTargetView* (MSAA resolve, optional)
        const void* depth_stencil_view = nullptr;  // ID3D11DepthStencilView* (optional)
    } d3d11;

    struct {
        const void* current_drawable      = nullptr;  // CAMetalDrawable (per frame)
        const void* depth_stencil_texture = nullptr;  // MTLTexture (optional)
        const void* msaa_color_texture    = nullptr;  // MTLTexture (optional)
    } metal;

    struct {
        const void* render_view        = nullptr;  // WGPUTextureView
        const void* resolve_view       = nullptr;  // WGPUTextureView (optional)
        const void* depth_stencil_view = nullptr;  // WGPUTextureView (optional)
    } wgpu;

    struct {
        std::uint32_t framebuffer = 0;  // GL framebuffer object (0 = default)
    } gl;

    /// Optional native debug overlay drawn inside the same render pass as
    /// the UI. This keeps diagnostics from reopening the swapchain with a
    /// LOAD pass, which is backend-fragile and can expose stale pixels.
    std::string_view debug_overlay_text{};
    const float*     debug_overlay_frame_ms = nullptr;
    std::size_t      debug_overlay_frame_ms_count = 0;
    DebugOverlayCorner debug_overlay_corner = DebugOverlayCorner::top_right;

    // TODO(embed): per-frame external (live) engine textures referenced as
    //   live://name — transient, used this frame only (DESIGN §3.2):
    //     const ExternalTexture* external; int external_count;
};

// ── Planned embedding surface (see docs/EMBEDDING_DESIGN.md) ──────────────────
// Declared here as intent; not yet implemented. Each is a small free function
// or Ui method with a C ABI mirror when it lands.
//
//   Async asset resolver       AssetResolver hook + Ui::asset_bytes/pixels/sprite,
//                              Ui::invalidate_asset   (DESIGN §3.0)
//   Atlas (POD, owned)         Ui::register_atlas(pixels, size, fmt, AtlasInfo) /
//                              unregister_atlas       (DESIGN §3.3)
//   Hit-test / click-through   Ui::hit_test(point) + panel::hit_test_at/_uv (§4)
//   Resource accounting        Ui::stats() + on_gpu_resource callback (§6)
//   Input intents/hooks        text_input_active()+caret_rect(), clipboard_get/set,
//                              gamepad navigation events (§4)
//   Host-driven time           dt/clock into render/tick (§5)
//   on_damage / partial repaint                                         (§5)
//   NVGcontext escape hatch (opt-in, advanced)                          (§7)

}  // namespace affineui
