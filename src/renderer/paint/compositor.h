#pragma once

#include <cstdint>

namespace affineui::detail {

/// Hand-written GL compositor: blits a layer's GPU texture into the
/// default framebuffer with optional transform + opacity. This is the
/// *only* code that runs every frame on an idle UI, so it gets to be
/// tight.
///
/// Phase 2C: one shader, one VAO, one VBO of 4 verts. Per-frame cost
/// dominated by the texture bind + 6-index draw.
class Compositor {
public:
    Compositor() = default;
    ~Compositor();

    Compositor(const Compositor&)            = delete;
    Compositor& operator=(const Compositor&) = delete;

    /// Lazy GL resource creation. Must be called with a valid GL
    /// context current.
    bool init();
    void shutdown();

    /// Blit `texture` to the default framebuffer at the configured
    /// viewport. `opacity` in [0,1] modulates the output.
    /// `flip_y` true → sample upside-down (NanoVG's FBO output is
    /// upside-down vs the default framebuffer's GL convention).
    void blit_fullscreen(unsigned texture, float opacity, bool flip_y);

private:
    unsigned program_{0};
    unsigned vao_{0};
    unsigned vbo_{0};
    int      u_opacity_{-1};
    int      u_tex_{-1};
    int      u_flip_y_{-1};
    bool     ready_{false};
};

}  // namespace affineui::detail
