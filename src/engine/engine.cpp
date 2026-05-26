#include "engine/engine.h"

#include <chrono>

namespace affineui {

struct Engine::Impl {
    StyleEngine      style;
    LayoutEngine     layout;
    PaintEngine      paint;
    Rasterizer       rasterizer;
    Compositor       compositor;
    AnimationRuntime animations;
    LayerTree        layers;
    BoxTree          boxes;
    RestyleQueue     restyle_queue;
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

void Engine::init(detail::DomHandle& dom) {
    impl_->style.attach_dom(dom);
    impl_->layout.attach(dom, impl_->style);
    impl_->paint.attach(impl_->layout.boxes(), impl_->style, impl_->layers);
}

void Engine::init_gpu() {
    impl_->compositor.init();
    // Rasterizer::init expects FontRegistry/ImageRegistry; those are
    // stubbed-out and accessed via the App. In the wired build the App
    // owns them and passes them in here.
}

void Engine::shutdown_gpu() {
    impl_->compositor.shutdown();
    impl_->rasterizer.shutdown();
}

void Engine::add_stylesheet(std::string_view css, StyleEngine::Origin origin) {
    impl_->style.add_stylesheet(css, origin);
    dirty_ |= DB_All;
}

void Engine::clear_stylesheets() {
    impl_->style.clear_stylesheets();
    dirty_ |= DB_All;
}

void Engine::mark_style_dirty(NodeId node) {
    impl_->restyle_queue.enqueue({node, MutationKind::AttrSet, 0});
    dirty_ |= DB_Style | DB_Paint | DB_Rasterize | DB_Composite;
}

void Engine::mark_subtree_dirty(NodeId root) {
    impl_->restyle_queue.enqueue_subtree(root);
    dirty_ |= DB_All;
}

void Engine::mark_layout_dirty(NodeId /*node*/) {
    dirty_ |= DB_Layout | DB_Paint | DB_Rasterize | DB_Composite;
}

void Engine::mark_paint_dirty(NodeId /*node*/) {
    dirty_ |= DB_Paint | DB_Rasterize | DB_Composite;
}

void Engine::on_dom_mutation(const Mutation& m) {
    impl_->restyle_queue.enqueue(m);
    dirty_ |= DB_All;
}

void Engine::on_viewport_changed(SizeF new_size) {
    last_viewport_ = new_size;
    dirty_ |= DB_Layout | DB_Paint | DB_Rasterize | DB_Composite;
}

FrameStats Engine::tick(SizeF viewport_px, float dpi_scale, double time_seconds) {
    auto stats = tick_no_composite(viewport_px, dpi_scale, time_seconds);
    if (dirty_ & DB_Composite) {
        stats.composite = impl_->compositor.composite(
            impl_->layers, RectF{0, 0, viewport_px.w, viewport_px.h}, dpi_scale);
        dirty_ &= ~DB_Composite;
    }
    return stats;
}

FrameStats Engine::tick_no_composite(SizeF viewport_px, float dpi_scale, double time_seconds) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    FrameStats stats{};

    if (dirty_ & DB_Style) {
        stats.style = impl_->style.resolve_dirty(impl_->restyle_queue);
        dirty_ &= ~DB_Style;
    }
    if (dirty_ & DB_Layout) {
        stats.layout = impl_->layout.layout_full(viewport_px, dpi_scale);
        dirty_ &= ~DB_Layout;
    }
    if (dirty_ & DB_Paint) {
        stats.paint = impl_->paint.paint_pass();
        dirty_ &= ~DB_Paint;
    }
    if (impl_->animations.active_count() > 0) {
        stats.active_animations = impl_->animations.sample(impl_->layers, time_seconds);
        if (stats.active_animations > 0) dirty_ |= DB_Composite;
        impl_->animations.gc_finished();
    }
    if (dirty_ & DB_Rasterize) {
        stats.rasterize = impl_->rasterizer.rasterize_pass(impl_->layers, dpi_scale);
        dirty_ &= ~DB_Rasterize;
    }

    const auto t1 = clock::now();
    stats.total_cpu_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return stats;
}

CompositorStats Engine::composite_only(SizeF viewport_px, float dpi_scale) {
    auto s = impl_->compositor.composite(
        impl_->layers, RectF{0, 0, viewport_px.w, viewport_px.h}, dpi_scale);
    dirty_ &= ~DB_Composite;
    return s;
}

StyleEngine&     Engine::style()       noexcept { return impl_->style; }
LayoutEngine&    Engine::layout()      noexcept { return impl_->layout; }
PaintEngine&     Engine::paint()       noexcept { return impl_->paint; }
Rasterizer&      Engine::rasterizer()  noexcept { return impl_->rasterizer; }
Compositor&      Engine::compositor()  noexcept { return impl_->compositor; }
AnimationRuntime& Engine::animations() noexcept { return impl_->animations; }
LayerTree&       Engine::layers()      noexcept { return impl_->layers; }
BoxTree&         Engine::boxes()       noexcept { return impl_->boxes; }

BoxId Engine::hit_test(Vec2 viewport_point) const {
    return impl_->layout.hit_test(viewport_point);
}

}  // namespace affineui
