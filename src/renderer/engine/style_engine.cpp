#include "renderer/engine/style_engine.h"

namespace affineui {

struct StyleEngine::Impl {
    // Stylesheet storage and dom binding land in Phase 2. The struct
    // exists today so the unique_ptr<Impl> destructor in this TU sees
    // a complete type.
};

StyleEngine::StyleEngine() : impl_(std::make_unique<Impl>()) {}
StyleEngine::~StyleEngine() = default;
StyleEngine::StyleEngine(StyleEngine&&) noexcept = default;
StyleEngine& StyleEngine::operator=(StyleEngine&&) noexcept = default;

void StyleEngine::attach_dom(detail::DomHandle& /*dom*/) {}
void StyleEngine::add_stylesheet(std::string_view /*css*/, Origin /*origin*/) {}
void StyleEngine::clear_stylesheets() {}

StyleStats StyleEngine::resolve_dirty(RestyleQueue& queue) {
    StyleStats s{};
    auto nodes = queue.drain_to_dirty_nodes();
    s.nodes_restyled = static_cast<std::uint32_t>(nodes.size());
    return s;
}

StyleStats StyleEngine::resolve_all() { return {}; }

const ComputedStyle* StyleEngine::style_of(NodeId /*node*/) const {
    return nullptr;
}

bool StyleEngine::apply_animated_value(NodeId /*node*/, PropertyId /*prop*/, float /*value*/) {
    return false;
}

}  // namespace affineui
