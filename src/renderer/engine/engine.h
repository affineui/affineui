#pragma once

// `Engine` — wires Style → Layout → Paint → Rasterize → Composite into
// a single object with explicit dirty-bit gating. One `Document` owns
// one `Engine`.
//
// The engine *does not* own the frame loop, the input pump, or the
// sokol pipeline lifecycle. Those live on `App`. The engine is the pure
// pipeline: state in, pixels (well, composited-quad draw calls) out.
//
// Each per-frame entry point checks its dirty bit and skips itself
// when clean. The contract — load-bearing for every performance claim
// in docs/OPTIMIZATION.md — is:
//
//   • idle frame (no input, no anim) → only `composite_frame()`
//     touches anything, and even that's a damage-tracked redraw if
//     enabled.
//
//   • style-only change → style + paint + rasterize affected layer +
//     composite.
//
//   • layout change → style + layout + paint + rasterize + composite.
//
//   • composite-only animation step → AnimationRuntime::sample +
//     composite. No re-entry into the pipeline above.

#include "affineui/geom.h"
#include "renderer/engine/animation.h"
#include "renderer/engine/box.h"
#include "renderer/engine/compositor.h"
#include "renderer/engine/layer.h"
#include "renderer/engine/layout_engine.h"
#include "renderer/engine/paint_engine.h"
#include "renderer/engine/rasterizer.h"
#include "renderer/engine/restyle_queue.h"
#include "renderer/engine/style_engine.h"

#include <memory>

namespace affineui {

namespace detail { struct DomHandle; }

/// Bitset of which pipeline stages are dirty. Read by `tick()` to
/// decide which sub-passes to run.
enum DirtyBits : std::uint8_t {
    DB_None      = 0,
    DB_Style     = 1 << 0,
    DB_Layout    = 1 << 1,
    DB_Paint     = 1 << 2,
    DB_Rasterize = 1 << 3,
    DB_Composite = 1 << 4,
    DB_All       = 0x1F,
};

/// Stats aggregated across the five stages for one frame.
struct FrameStats {
    StyleStats      style{};
    LayoutStats     layout{};
    PaintStats      paint{};
    RasterizeStats  rasterize{};
    CompositorStats composite{};
    std::uint32_t   active_animations{0};
    double          total_cpu_ms{0.0};
};

/// Engine — the pipeline-as-object.
class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    void init(detail::DomHandle& dom);

    /// One-time GPU init. Must be called after sokol_gfx is up.
    void init_gpu();

    /// Mirror of init_gpu(). Must be called before sokol_gfx teardown.
    void shutdown_gpu();

    // ── Stylesheets ──────────────────────────────────────────────────
    void add_stylesheet(std::string_view css, StyleEngine::Origin origin);
    void clear_stylesheets();

    // ── Mutation entry points (route through RestyleQueue) ───────────
    void mark_style_dirty(NodeId node);
    void mark_subtree_dirty(NodeId root);
    void mark_layout_dirty(NodeId node);
    void mark_paint_dirty(NodeId node);
    void on_dom_mutation(const Mutation& m);
    void on_viewport_changed(SizeF new_size);

    // ── Per-frame entry ──────────────────────────────────────────────

    /// Advance the pipeline by one frame. Returns the work-done stats.
    /// `time_seconds` is the wall clock (used by animations).
    FrameStats tick(SizeF viewport_px, float dpi_scale, double time_seconds);

    /// Variant for hosts that drive composite separately (e.g. the
    /// app is already in a sokol_gfx pass and just wants the composite
    /// step). Performs all upstream stages, returns without
    /// compositing.
    FrameStats tick_no_composite(SizeF viewport_px, float dpi_scale, double time_seconds);

    /// Just composite. Useful when the host knows nothing has changed
    /// upstream and only wants the every-frame redraw.
    CompositorStats composite_only(SizeF viewport_px, float dpi_scale);

    // ── Component access ─────────────────────────────────────────────
    StyleEngine&     style()       noexcept;
    LayoutEngine&    layout()      noexcept;
    PaintEngine&     paint()       noexcept;
    Rasterizer&      rasterizer()  noexcept;
    Compositor&      compositor()  noexcept;
    AnimationRuntime& animations() noexcept;
    LayerTree&       layers()      noexcept;
    BoxTree&         boxes()       noexcept;

    // ── Inspection ──────────────────────────────────────────────────
    std::uint8_t dirty_bits() const noexcept { return dirty_; }
    BoxId        hit_test(Vec2 viewport_point) const;

private:
    std::uint8_t  dirty_{DB_All};
    SizeF         last_viewport_{};
    float         last_dpi_{1.0f};

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace affineui
