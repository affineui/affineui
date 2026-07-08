#pragma once

// `Rasterizer` — converts a layer's DisplayList into pixels in the
// layer's texture. Runs only when a layer is rasterize-dirty, never
// per-frame.
//
// Uses NanoVG internally; NanoVG only exists inside this stage. The
// rasterizer is the *only* file in the engine that includes NanoVG
// headers. Everyone else sees `DisplayList` going in, `LayerTexture`
// coming out.
//
// Texture lifecycle is owned by the rasterizer:
//   • First rasterize:  allocates a texture (from pool if available,
//                       otherwise from GPU).
//   • Re-rasterize:     reuses the existing texture if its size still
//                       fits; otherwise releases and re-allocates.
//   • Layer freed:      texture goes to the per-size free-list (see
//                       docs/OPTIMIZATION.md § 3.5).
//
// Atlas integration:
//   Small layers (≤ atlas_eligible_threshold_px on both axes) rasterize
//   into a sub-rect of a shared atlas texture. The atlas allocator is
//   internal to the rasterizer; the compositor receives an atlas
//   texture id + sub-rect via the Layer.

#include "affineui/display_list.h"
#include "affineui/geom.h"
#include "renderer/engine/layer.h"

#include <cstdint>
#include <memory>

namespace affineui {

class FontRegistry;
class ImageRegistry;

struct RasterizeStats {
    std::uint32_t layers_rasterized{0};
    std::uint32_t layers_skipped_hash_match{0};
    std::uint32_t textures_allocated{0};
    std::uint32_t textures_recycled{0};
    std::uint32_t atlas_inserts{0};
    std::uint32_t atlas_evictions{0};
    double        cpu_time_ms{0.0};
};

struct RasterizerConfig {
    /// Atlas dimensions. Shared with Compositor — must match so that
    /// the atlas texture handle is meaningful at composite time.
    int atlas_size_px{4096};
    int atlas_eligible_threshold_px{512};
    int max_atlases{2};

    /// Generate mipmaps for layers tagged LF_NeedsMipmap. Costs a few
    /// ms per layer at rasterize time; saves shimmer at composite.
    /// See docs/OPTIMIZATION.md § 3.7.
    bool generate_mipmaps_when_scaled{true};

    /// Use a 16-bit linear-encoded float framebuffer when available.
    /// Improves gradient/shadow quality; costs 2x VRAM. Default off.
    bool linear_float_textures{false};
};

class Rasterizer {
public:
    explicit Rasterizer(RasterizerConfig cfg = {});
    ~Rasterizer();

    Rasterizer(const Rasterizer&)            = delete;
    Rasterizer& operator=(const Rasterizer&) = delete;
    Rasterizer(Rasterizer&&) noexcept;
    Rasterizer& operator=(Rasterizer&&) noexcept;

    /// One-time GPU resource init. Creates the NanoVG context, the
    /// initial atlas texture, the free-list. Must be called after
    /// sokol_gfx is set up.
    void init(FontRegistry& fonts, ImageRegistry& images);

    /// Mirror of init(). Releases NanoVG context and all textures.
    void shutdown();

    /// Walk the LayerTree, rasterize every dirty layer. Skips a layer
    /// when `layer.display_list.rolling_hash() == layer.rasterized_hash`
    /// (docs/OPTIMIZATION.md § 3.1).
    RasterizeStats rasterize_pass(LayerTree& layers, float dpi_scale);

    /// Force-rasterize a single layer. Useful for tests and for the
    /// "promote during idle" use case.
    void rasterize_layer(Layer& layer, float dpi_scale);

    /// Return a texture to the pool (called by LayerTree::free_layer).
    void return_to_pool(LayerTextureHandle tex, SizeF px_size) noexcept;

    /// Total VRAM held by textures + atlases. Approximate.
    std::uint64_t vram_in_use_bytes() const noexcept;

    const RasterizerConfig& config() const noexcept { return cfg_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RasterizerConfig      cfg_;
};

}  // namespace affineui
