#pragma once

// `Box` — the per-element layout result. One Box per element that
// produces a rendered box (not all DOM nodes do — `display: none`,
// pure-text, etc. produce none or different shapes).
//
// Memory layout target: 64 bytes (one cache line). Layout walks the box
// tree in tree order during paint; one cache line per element keeps the
// paint walk close to memory-bandwidth-bound.
//
// Box is *not* shared. It's allocated from the document's box arena
// and lives until the next full relayout. Incremental relayout reuses
// existing Box memory in place where possible.

#include "affineui/computed_style.h"
#include "affineui/geom.h"

#include <cstdint>
#include <vector>

namespace affineui {

using BoxId    = std::uint32_t;
using NodeId   = std::uint32_t;   // index into the DOM (lexbor) node table
using LayerId  = std::uint32_t;
constexpr BoxId    kInvalidBox   = ~BoxId{0};
constexpr LayerId  kInvalidLayer = ~LayerId{0};

enum class BoxKind : std::uint8_t {
    Block,
    Inline,
    InlineBlock,
    Flex,
    FlexItem,
    Replaced,    // <img>, <video>, <canvas>, <input> chrome
    Text,        // inline text run; one per contiguous run within a line
    LineBox,     // synthetic — a finalized line in an inline flow
};

enum BoxFlags : std::uint16_t {
    BF_None            = 0,
    BF_Anonymous       = 1 << 0,  // synthesized (anon block, line box)
    BF_FloatLeft       = 1 << 1,
    BF_FloatRight      = 1 << 2,
    BF_PositionedAbs   = 1 << 3,
    BF_PositionedFixed = 1 << 4,
    BF_LayoutDirty     = 1 << 5,
    BF_PaintDirty      = 1 << 6,
    BF_HasOwnLayer     = 1 << 7,
    BF_ClipsOverflow   = 1 << 8,
    BF_HasBackground   = 1 << 9,
    BF_HasBorders      = 1 << 10,
    BF_HasShadows      = 1 << 11,
};

/// 64-byte layout box.
struct alignas(16) Box {
    // ── Tree structure (16 B) ────────────────────────────────────────
    BoxId   first_child{kInvalidBox};
    BoxId   next_sibling{kInvalidBox};
    BoxId   parent{kInvalidBox};
    NodeId  node{0};

    // ── Geometry (post-layout, 16 B) ─────────────────────────────────
    // Rect of the *border box*. Padding box = border-box minus border
    // widths; content box = padding box minus padding. Margins live
    // outside the box rect. We store only the border-box here and
    // recompute the others on demand to keep the struct small.
    RectF   rect{};

    // ── Resolved edge widths (16 B; floats not Lengths) ──────────────
    // Margin / padding / border resolved to px during layout. Stored
    // here so paint doesn't need to re-resolve relative units.
    float   margin_top{0}, margin_right{0}, margin_bottom{0}, margin_left{0};

    // ── Discrete fields (16 B) ───────────────────────────────────────
    BoxKind kind{BoxKind::Block};
    std::uint8_t  pad0{0};
    std::uint16_t flags{BF_None};
    LayerId       layer{kInvalidLayer};

    // Baseline offset within the box (for inline alignment). 0 for
    // non-text boxes.
    float         baseline{0.0f};

    // Z-stack index within the parent stacking context. Sorted once at
    // layout end. See docs/OPTIMIZATION.md § 3.12.
    std::int32_t  stack_index{0};

    // ── Inline helpers (constexpr accessors) ─────────────────────────
    constexpr bool is_text()   const noexcept { return kind == BoxKind::Text; }
    constexpr bool is_inline() const noexcept {
        return kind == BoxKind::Inline || kind == BoxKind::Text
            || kind == BoxKind::InlineBlock;
    }
    constexpr bool has_own_layer() const noexcept {
        return (flags & BF_HasOwnLayer) != 0;
    }
};

static_assert(sizeof(Box) == 64,
              "Box must stay one cache line; check field padding before growing");

/// Box tree storage. Arena-allocated dense array of Box; tree edges are
/// just BoxId indices. Allocation is O(1), iteration is linear-memory.
class BoxTree {
public:
    BoxTree() = default;

    /// Reserve capacity for an expected node count. Sizes the arena so
    /// the typical layout doesn't reallocate mid-pass.
    void reserve(std::size_t n);

    /// Reset the arena to empty. Memory is *not* freed — kept for the
    /// next layout pass. See docs/OPTIMIZATION.md § 3.5 (pool reuse).
    void reset() noexcept;

    /// Append a new box. Returns its BoxId. The arena grows
    /// geometrically.
    BoxId allocate(BoxKind kind, NodeId node);

    Box&        at(BoxId id)       noexcept { return boxes_[id]; }
    const Box&  at(BoxId id) const noexcept { return boxes_[id]; }

    BoxId       root() const noexcept { return root_; }
    void        set_root(BoxId r)    noexcept { root_ = r; }

    std::size_t size() const noexcept { return boxes_.size(); }

private:
    std::vector<Box> boxes_;
    BoxId            root_{kInvalidBox};
};

}  // namespace affineui
