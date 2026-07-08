#include "renderer/engine/box.h"

namespace affineui {

void BoxTree::reserve(std::size_t n) {
    boxes_.reserve(n);
}

void BoxTree::reset() noexcept {
    boxes_.clear();
    root_ = kInvalidBox;
}

BoxId BoxTree::allocate(BoxKind kind, NodeId node) {
    const auto id = static_cast<BoxId>(boxes_.size());
    boxes_.emplace_back();
    auto& box = boxes_.back();
    box.kind = kind;
    box.node = node;
    return id;
}

}  // namespace affineui
