#pragma once

#include <cstdint>
#include <functional>

namespace affineui::detail {

/// Stable identifier for an element across its lifetime in a
/// Document. `index` is the slot in the per-Document SoA arrays;
/// `generation` rolls over each time a slot is reused, so a stale
/// ElementId from before a reset is detectably-invalid.
///
/// 0 with generation 0 is the canonical invalid value.
struct ElementId {
    std::uint32_t index{0};
    std::uint16_t generation{0};

    constexpr bool valid() const noexcept {
        return generation != 0;
    }
    constexpr bool operator==(const ElementId& o) const noexcept {
        return index == o.index && generation == o.generation;
    }
    constexpr bool operator!=(const ElementId& o) const noexcept {
        return !(*this == o);
    }
};

static_assert(sizeof(ElementId) == 8, "ElementId must pack to 8 bytes");

}  // namespace affineui::detail

namespace std {
template <>
struct hash<affineui::detail::ElementId> {
    std::size_t operator()(const affineui::detail::ElementId& id) const noexcept {
        // Splittable-mix: avoids degenerate buckets when ids cluster.
        std::uint64_t x = (std::uint64_t(id.index) << 16) ^ id.generation;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<std::size_t>(x);
    }
};
}  // namespace std
