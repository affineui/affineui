#pragma once

// affineui::Renderer — the graphics-API-specific part of AffineUI.
//
// Owns:
//   • the NanoVG context (and underlying GL state)
//   • the root layer's FBO
//   • the cached display list across frames
//   • the compositor (textured-quad blit to default framebuffer)
//
// Does NOT own:
//   • the window (your windowing toolkit does — sokol_app, SDL,
//     glfw, whatever)
//   • the event loop
//   • the input source
//
// Lifecycle:
//   The first call to `render()` lazily initializes GPU resources
//   against the *currently bound* graphics context, so you can just
//   construct a Renderer and call render() from inside your render
//   pass without remembering an init step. Call `shutdown()` before
//   the GL context goes away (or rely on the destructor when the
//   process ends).
//
// Thread model:
//   Single-threaded. Construct, render, and destroy on the same
//   thread — same one that owns your graphics context.

#include "affineui/embed.h"
#include "affineui/types.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace affineui {

class Document;

struct RenderStats {
    std::uint64_t frames{0};
    std::uint64_t display_list_records{0};
    std::uint64_t display_list_replays{0};
    std::uint64_t display_list_ops_culled{0};
    std::uint64_t display_list_changes{0};
    std::uint64_t display_list_unchanged{0};
    std::uint64_t root_layer_rasterizes{0};
    std::uint64_t root_layer_partial_rasterizes{0};
    std::uint64_t root_layer_composites{0};
    std::uint64_t root_layer_direct_composites{0};
    std::uint32_t cached_ops{0};
    std::uint32_t dirty_rects{0};
    std::uint32_t dirty_area_px{0};
    std::uint32_t dirty_area_pct_x100{0};
    std::uint32_t display_list_ops_culled_this_frame{0};
    bool          recorded_this_frame{false};
    bool          display_list_changed_this_frame{false};
    bool          root_layer_rasterized_this_frame{false};
    bool          root_layer_partial_this_frame{false};
    bool          root_layer_direct_this_frame{false};
    bool          root_layer_reused_this_frame{false};
    bool          viewport_changed{false};
    bool          layout_dirty{false};
    bool          paint_dirty{false};
    bool          animations_active{false};
};

namespace detail { struct RendererImpl; }

class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

    /// True once GPU resources are live. Becomes true after the first
    /// render() (or an explicit init_gl()) and false again after
    /// shutdown().
    bool ready() const noexcept;

    /// One-time GPU resource creation. Assumes a current GL3 context.
    /// You normally don't have to call this — `render()` does it on
    /// the first invocation. Useful if you want to pay the init cost
    /// at a known time (e.g. during a loading screen) rather than on
    /// the first frame.
    void init_gl();

    /// Embedded mode: bring up sokol_gfx on the host's graphics objects
    /// (sg_setup against `gpu`), then create NanoVG resources. AffineUI
    /// then owns the sokol_gfx instance and tears it down in shutdown().
    /// The host keeps ownership of the device objects themselves. When
    /// `allocator` is non-null, sokol_gfx allocates through it.
    /// Throws/aborts only via the logger; on failure ready() stays false.
    void init_embedded(const GpuContext& gpu, const Allocator* allocator = nullptr);

    /// Embedded mode: render `doc` into the host's per-frame render-target
    /// views. Opens a sokol_gfx pass into `target` (clearing to
    /// clear_color when target.clear is set), draws, then ends + commits.
    /// Does not present. Requires a prior init_embedded().
    void render_to(Document& doc, const FrameTarget& target);

    /// Release all GPU resources. Must be called while the graphics
    /// context that created them is still current — usually right
    /// before your windowing layer tears the context down.
    void shutdown();

    /// Render one frame of `doc` into the default framebuffer.
    ///   fb_w, fb_h  : framebuffer dimensions in pixels
    ///   dpi_scale   : pixels-per-point (1.0 standard, 2.0 Retina, etc.)
    ///
    /// Calls into the document's layout + paint pipeline as needed,
    /// rasterizes into an internal FBO, then blits the result to the
    /// default framebuffer with the configured clear color. No
    /// frame-loop control flow — call this once per your-game's-frame
    /// from inside your render pass.
    void render(Document& doc, int fb_w, int fb_h, float dpi_scale);

    /// Draw a lightweight native debug overlay into the current render
    /// pass. This intentionally bypasses the document tree so perf
    /// instrumentation does not force HTML reparsing or affect layout.
    void draw_debug_overlay(std::string_view text,
                            std::span<const float> frame_ms,
                            int fb_w,
                            int fb_h,
                            float dpi_scale);

    /// Background color (RGBA) applied to the default framebuffer
    /// before the document quad is composited on top. Default is the
    /// catppuccin Base #1e1e2e.
    void set_clear_color(Color c);
    Color clear_color() const;

    /// Lightweight counters for profiling retained rendering. A frame
    /// can replay the cached display list without recording document
    /// paint; `recorded_this_frame` tells those paths apart.
    const RenderStats& stats() const noexcept;

private:
    std::unique_ptr<detail::RendererImpl> impl_;
};

}  // namespace affineui
