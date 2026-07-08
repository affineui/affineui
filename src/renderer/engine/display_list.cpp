#include "affineui/display_list.h"

#include <algorithm>
#include <cstring>

namespace affineui {

namespace {

// FNV-1a step over `n` bytes. Constant time per byte; ~0.3 ns / byte.
inline std::uint64_t fnv_step(std::uint64_t h, const void* p, std::size_t n) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= bytes[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

}  // namespace

void DisplayList::clear() noexcept {
    ops_.clear();
    runs_.clear();
    gradients_.clear();
    glyphs_.clear();
    advances_.clear();
    strings_.clear();
    bounds_       = RectF{};
    rolling_hash_ = 0xcbf29ce484222325ull;
}

void DisplayList::reserve(std::size_t n_ops) {
    ops_.reserve(n_ops);
}

void DisplayList::push(const PaintOp& op) noexcept {
    ops_.push_back(op);
    rolling_hash_ = fnv_step(rolling_hash_, &op, sizeof(PaintOp));

    if (op.rect.empty()) return;
    if (bounds_.empty()) {
        bounds_ = op.rect;
    } else {
        const float x0 = std::min(bounds_.x, op.rect.x);
        const float y0 = std::min(bounds_.y, op.rect.y);
        const float x1 = std::max(bounds_.right(), op.rect.right());
        const float y1 = std::max(bounds_.bottom(), op.rect.bottom());
        bounds_ = RectF{x0, y0, x1 - x0, y1 - y0};
    }
}

std::uint32_t DisplayList::push_text_run(const TextRunPayload& run) {
    const auto idx = static_cast<std::uint32_t>(runs_.size());
    runs_.push_back(run);
    return idx;
}

std::uint32_t DisplayList::push_gradient(const GradientPayload& g) {
    const auto idx = static_cast<std::uint32_t>(gradients_.size());
    gradients_.push_back(g);
    return idx;
}

}  // namespace affineui
