#pragma once

// `Layer` — the compositor's unit of caching. A Layer owns:
//   • a display list (the paint output for its content)
//   • a GPU texture (the rasterized result of that display list)
//   • a transform + opacity uniform (animated at composite time)
//   • a hash of its display list (for skip-rasterize)
//
// The composition tree of layers is *parallel to* but not identical
// with the DOM. Most elements paint into their parent's layer; only
// elements that satisfy `ComputedStyle::requires_own_layer()` or carry
// transforms/opacity/etc. get their own.
//
// See docs/OPTIMIZATION.md § 3 for the compositing strategy this
// supports.

#include "affineui/display_list.h"
#include "affineui/geom.h"

#include "renderer/engine/box.h"  // LayerId, kInvalidLayer

#include <cstdint>
#include <vector>

namespace affineui {

using LayerTextureHandle = std::uint32_t;
constexpr LayerTextureHandle kInvalidTexture = 0;

enum LayerFlags : std::uint16_t {
    LF_None            = 0,
    LF_RasterizeDirty  = 1 << 0,  // display list changed; texture stale
    LF_CompositeDirty  = 1 << 1,  // uniforms changed; needs re-composite
    LF_Opaque          = 1 << 2,  // wholly opaque; participates in occlusion culling
    LF_OffScreen       = 1 << 3,  // outside viewport; skip rasterize+composite
    LF_RootLayer       = 1 << 4,  // the document root; gets the single-layer fast path
    LF_NeedsMipmap     = 1 << 5,  // scaled below 1.0 somewhere; mipmap at rasterize
    LF_AtlasResident   = 1 << 6,  // texture lives in shared atlas, not standalone
    LF_HasTransform    = 1 << 7,
    LF_HasOpacity      = 1 << 8,
    LF_HasFilter       = 1 << 9,
};

/// One compositor layer.
struct Layer {
    // ── Identity / tree ─────────────────────────────────────────────
    LayerId            id{kInvalidLayer};
    LayerId            parent{kInvalidLayer};
    std::vector<LayerId> children;

    // ── Spatial ─────────────────────────────────────────────────────
    // Bounds in *document* coordinates (before this layer's transform
    // is applied). Compositor uses this to test viewport intersection
    // and to size the layer's render target.
    RectF              bounds{};

    // Affine transform applied at composite time. Animations write
    // here directly (see docs/OPTIMIZATION.md § 1.3). Updating this
    // does NOT invalidate rasterize — only composite.
    Mat2x3             transform{Mat2x3::identity()};

    // Per-layer opacity multiplier. Same shape as transform: animatable
    // without paint/rasterize.
    float              opacity{1.0f};

    // ── Cached paint output ─────────────────────────────────────────
    DisplayList        display_list;

    // Hash of the most recently *rasterized* display list. After paint
    // emits a new display list, compare the new `display_list.rolling_hash()`
    // against `rasterized_hash_`; if equal, skip rasterize. See
    // docs/OPTIMIZATION.md § 3.1.
    std::uint64_t      rasterized_hash{0};

    // ── Cached rasterization output ─────────────────────────────────
    LayerTextureHandle texture{kInvalidTexture};
    SizeF              texture_pixel_size{};   // texture dims in *device* px

    // ── Compositor metadata ─────────────────────────────────────────
    std::int32_t       z_index{0};
    std::uint16_t      flags{LF_RasterizeDirty | LF_CompositeDirty};
    std::uint16_t      _pad{0};

    // Helpers — terse names because callers chain them frequently.
    constexpr bool dirty_raster()    const noexcept { return (flags & LF_RasterizeDirty) != 0; }
    constexpr bool dirty_composite() const noexcept { return (flags & LF_CompositeDirty) != 0; }
    constexpr bool offscreen()       const noexcept { return (flags & LF_OffScreen) != 0; }
    constexpr bool is_root()         const noexcept { return (flags & LF_RootLayer) != 0; }

    void mark_raster_dirty()    noexcept { flags |= LF_RasterizeDirty | LF_CompositeDirty; }
    void mark_composite_dirty() noexcept { flags |= LF_CompositeDirty; }
    void clear_raster_dirty()   noexcept { flags &= ~LF_RasterizeDirty; }
    void clear_composite_dirty()noexcept { flags &= ~LF_CompositeDirty; }
};

/// LayerTree owns all Layers for one Document. Allocation strategy is
/// the same arena pattern as BoxTree: dense vector, tree edges as ids,
/// reset-without-free between layout passes.
class LayerTree {
public:
    LayerTree();

    /// The root layer always exists. ID is stable across resets.
    LayerId root() const noexcept { return root_; }

    Layer&       at(LayerId id)       noexcept { return layers_[id]; }
    const Layer& at(LayerId id) const noexcept { return layers_[id]; }

    /// Allocate a new layer as a child of `parent`. Marks both layers
    /// raster-dirty.
    LayerId allocate(LayerId parent);

    /// Free a layer (returns its texture to the recycler — see
    /// docs/OPTIMIZATION.md § 3.5). Invalidates LayerId, so callers
    /// must finish their walk first.
    void free_layer(LayerId id);

    /// Z-sort all children of every layer. Called once after layout.
    /// See docs/OPTIMIZATION.md § 3.12.
    void sort_z();

    std::size_t size() const noexcept { return layers_.size(); }

    /// Walk visible layers in composite order (back-to-front).
    /// `visitor` receives `(Layer&, depth)`. Skips off-screen and
    /// fully-occluded layers (occlusion test is delegated to
    /// Compositor — this is a tree walk, not the full pass).
    template <typename Visitor>
    void walk_composite(Visitor&& visitor);

private:
    std::vector<Layer> layers_;
    LayerId            root_{kInvalidLayer};
    std::vector<LayerId> free_list_;
};

}  // namespace affineui
