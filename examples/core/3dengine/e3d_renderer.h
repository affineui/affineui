// e3d_renderer.h — sokol_gfx forward renderer.
//
// Renders a Scene through a PerspectiveCamera into an offscreen,
// MSAA-resolved color target that UI code composites via the Painter
// (see draw_to()). Draw order follows three.js: opaque
// front-to-back, then transparent back-to-front, with
// Object3D::render_order overriding both.
//
// One directional shadow map is supported: the first DirectionalLight
// with cast_shadow lights shadow-casting meshes into a depth map that
// shadow-receiving surfaces (and ShadowMaterial catchers) sample with
// a 3x3 PCF compare.
//
// Frame contract: render() runs its own sg_begin_pass/sg_end_pass
// brackets, so call it OUTSIDE any active pass — App::on_frame is the
// right place (it ticks before the UI's swapchain pass). The custom
// paint handler then only draws the finished texture.
#pragma once

#include <cstdint>
#include <memory>

#include "e3d_scene.h"

namespace affineui {
class Painter;
struct Rect;
}

namespace e3d {

class Renderer {
public:
    explicit Renderer(int sample_count = 4);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Resize the offscreen target (device pixels). Cheap when the size
    /// is unchanged; otherwise targets are recreated lazily on the next
    /// render(). Also updates camera aspect inside render().
    void set_size(int width, int height);
    int  width() const;
    int  height() const;

    /// Render one frame. Must be called outside any sokol render pass.
    void render(Scene& scene, PerspectiveCamera& camera);

    /// sg_image id of the resolved color target (0 before first render).
    std::uint32_t color_image_id() const;
    /// True when the target's row 0 is the bottom scanline (GL).
    bool flip_y() const;

    /// Emit the resolved color target as one frame-scoped paint command.
    /// Neither this renderer nor the caller retains the Painter or a
    /// painter-owned resource handle. Before the first successful render the
    /// target is invalid and this is a no-op.
    void draw_to(affineui::Painter& painter,
                 const affineui::Rect& dst) const;

    struct Impl;  // implementation detail (e3d_renderer.cpp)

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace e3d
