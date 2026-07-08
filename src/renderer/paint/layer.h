#pragma once

#include <cstdint>

// Forward declarations to keep NanoVG/GL out of this header.
struct NVGcontext;
struct NVGLUframebuffer;

namespace affineui::detail {

/// One rasterized layer. Owns an NVGLUframebuffer; the underlying GL
/// texture is composited every frame by the per-frame composite pass.
///
/// Phase 2C: a Document has exactly one root layer covering the full
/// viewport. Phase 3 introduces sub-layers for `will-change`,
/// `transform`, `opacity`, scroll containers, etc.
class Layer {
public:
    Layer() = default;
    ~Layer();

    Layer(const Layer&)            = delete;
    Layer& operator=(const Layer&) = delete;
    Layer(Layer&& o) noexcept;
    Layer& operator=(Layer&& o) noexcept;

    /// (Re)allocate to the given pixel size if it changed. No-op if
    /// the layer is already at this size.
    void ensure_size(NVGcontext* vg, int width_px, int height_px);

    void destroy();

    NVGLUframebuffer* fb()         const { return fb_; }
    unsigned          gl_texture() const;  ///< GL texture name; 0 if no FBO

    int width_px()  const { return w_; }
    int height_px() const { return h_; }

    // Content hash of the DisplayList most recently rasterized into
    // this layer. The frame loop compares the next frame's hash
    // against this to decide whether to rasterize again.
    std::uint64_t last_content_hash{0};

private:
    NVGLUframebuffer* fb_{nullptr};
    int               w_{0};
    int               h_{0};
};

}  // namespace affineui::detail
