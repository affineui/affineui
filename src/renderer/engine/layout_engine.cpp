#include "renderer/engine/layout_engine.h"

namespace affineui {

struct LayoutEngine::Impl {
    BoxTree boxes;
};

LayoutEngine::LayoutEngine() : impl_(std::make_unique<Impl>()) {}
LayoutEngine::~LayoutEngine() = default;
LayoutEngine::LayoutEngine(LayoutEngine&&) noexcept = default;
LayoutEngine& LayoutEngine::operator=(LayoutEngine&&) noexcept = default;

void LayoutEngine::attach(detail::DomHandle& /*dom*/, StyleEngine& /*style*/) {}

LayoutStats LayoutEngine::layout_full(SizeF /*viewport_px*/, float /*dpi_scale*/) {
    impl_->boxes.reset();
    return {};
}

LayoutStats LayoutEngine::layout_subtree(NodeId /*node*/, SizeF /*viewport_px*/, float /*dpi_scale*/) {
    return {};
}

BoxTree&       LayoutEngine::boxes()       noexcept { return impl_->boxes; }
const BoxTree& LayoutEngine::boxes() const noexcept { return impl_->boxes; }

BoxId LayoutEngine::hit_test(Vec2 /*point*/) const { return kInvalidBox; }

}  // namespace affineui
