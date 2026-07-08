#include "renderer/engine/compositor.h"

namespace affineui {

struct Compositor::Impl {
    // Holds the sokol_gfx pipeline + shader + quad buffers in Phase 2.
};

Compositor::Compositor(CompositorConfig cfg)
    : impl_(std::make_unique<Impl>()), cfg_(cfg) {}
Compositor::~Compositor() = default;
Compositor::Compositor(Compositor&&) noexcept = default;
Compositor& Compositor::operator=(Compositor&&) noexcept = default;

void Compositor::init() {}
void Compositor::shutdown() {}

CompositorStats Compositor::composite(LayerTree& layers,
                                      const RectF& /*viewport*/,
                                      float /*dpi_scale*/) {
    CompositorStats s{};
    for (std::size_t i = 0; i < layers.size(); ++i) {
        auto& layer = layers.at(static_cast<LayerId>(i));
        ++s.layers_visited;
        if (layer.offscreen()) {
            ++s.layers_culled_offscreen;
            continue;
        }
        ++s.layers_drawn;
        layer.clear_composite_dirty();
    }
    s.draw_calls = s.layers_drawn;  // pre-batching estimate
    return s;
}

void Compositor::mark_damaged(const RectF& /*rect*/) {}

void Compositor::sample_animations(LayerTree& /*layers*/, double /*time_seconds*/) {}

}  // namespace affineui
