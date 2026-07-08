#pragma once

// `Compositor` — the only stage that runs every frame for idle UI.
//
// Reads:   LayerTree (which textures, which transforms, which opacity)
// Writes:  the swapchain backbuffer
//
// Does NOT use NanoVG. Issues raw sokol_gfx draw calls against a
// pre-built pipeline (one shader, one VAO, one uniform block, one quad
// instance buffer). Per-frame work is O(visible layers) and bounded.
//
// Why it's its own object, not a Painter::end_frame() side-effect:
//   • Lifetime: GPU resources (pipeline, atlases, quad VBO) outlive any
//     single frame.
//   • Zero per-frame allocation. Buffers are reused, never freed.
//   • Tight inner loop. No virtual dispatch, no vtable; the compiler
//     inlines through the whole composite pass.
//
// See docs/OPTIMIZATION.md § 3 for the full strategy this implements.

#include "affineui/geom.h"
#include "renderer/engine/layer.h"

#include <cstdint>
#include <memory>

namespace affineui {

struct CompositorConfig {
    // Initial atlas dimensions. Atlases grow on demand; reaching the
    // GPU's max texture size promotes layers out of the atlas instead.
    int atlas_size_px{4096};

    // Layers smaller than this on both axes are eligible for atlas
    // packing. Larger layers get their own texture (see § 3.2).
    int atlas_eligible_threshold_px{512};

    // Max concurrent atlases before pressure triggers LRU eviction.
    int max_atlases{2};

    // Enable damage tracking (§ 3.4). Skip-presenting unchanged regions
    // matters less on tile-based GPUs (Apple Silicon); allow opt-out.
    bool damage_tracking{true};

    // Enable occlusion culling (§ 3.13). Front-to-back opaque pass to
    // build a coverage mask; covered layers skip composite.
    bool occlusion_culling{true};

    // Single-layer fast path (§ 3.11): if there's only the root layer
    // with no transforms/opacity, rasterize straight to the backbuffer.
    bool single_layer_fast_path{true};
};

/// Per-frame stats returned by `composite()` for profiling.
struct CompositorStats {
    std::uint32_t layers_visited{0};
    std::uint32_t layers_drawn{0};
    std::uint32_t layers_culled_offscreen{0};
    std::uint32_t layers_culled_occluded{0};
    std::uint32_t draw_calls{0};
    std::uint32_t damaged_rect_count{0};
    double        gpu_time_ms{0.0};   // populated when GPU timer queries are available
};

/// Compositor — concrete, non-virtual. Swappable at compile time via
/// CMake option for headless / alternate-backend builds, not at runtime.
class Compositor {
public:
    explicit Compositor(CompositorConfig cfg = {});
    ~Compositor();

    Compositor(const Compositor&)            = delete;
    Compositor& operator=(const Compositor&) = delete;
    Compositor(Compositor&&) noexcept;
    Compositor& operator=(Compositor&&) noexcept;

    /// One-time GPU resource init. Must be called after sokol_gfx is
    /// set up. Creates the shader, pipeline, quad buffers.
    void init();

    /// Mirror of `init()`. Called before sokol_gfx teardown.
    void shutdown();

    /// Per-frame entry point. Reads the LayerTree, draws all visible
    /// layers as textured quads, returns stats. Does NOT begin/end the
    /// sokol_gfx pass — caller manages the pass lifecycle so the
    /// embedder can composite into their own pass.
    CompositorStats composite(LayerTree& layers,
                              const RectF& viewport,
                              float dpi_scale);

    /// Mark a viewport rect dirty (used by damage tracking when an
    /// external system invalidates a region — e.g. a video frame
    /// inside an `<video>` element). Coalesced into the next composite.
    void mark_damaged(const RectF& rect);

    /// Sample any active animations against `time_seconds`, writing
    /// their interpolated values onto the appropriate layers'
    /// transform/opacity. Called before composite when there are
    /// active composite-only animations. See docs/OPTIMIZATION.md
    /// § 1.3.
    void sample_animations(LayerTree& layers, double time_seconds);

    const CompositorConfig& config() const noexcept { return cfg_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    CompositorConfig      cfg_;
};

}  // namespace affineui
