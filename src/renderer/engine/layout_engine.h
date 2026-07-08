#pragma once

// `LayoutEngine` — runs the layout pass for a Document.
//
// Inputs:
//   • DOM (lexbor)
//   • per-element ComputedStyle (produced by StyleEngine)
//   • viewport size
//
// Output:
//   • BoxTree, populated with resolved geometry
//   • layer-promotion decisions written to Box::layer
//
// Sub-engines (block / inline / flex / grid) are internal. The outer
// API is just "run a pass." Incremental relayout for dirty subtrees is
// the same call with a starting subtree root.

#include "affineui/geom.h"
#include "renderer/engine/box.h"

#include <memory>

namespace affineui {

namespace detail { struct DomHandle; }
class StyleEngine;

struct LayoutStats {
    std::uint32_t boxes_created{0};
    std::uint32_t boxes_reused{0};
    std::uint32_t inline_lines{0};
    std::uint32_t flex_passes{0};
    std::uint32_t yoga_node_count{0};
    double        cpu_time_ms{0.0};
};

class LayoutEngine {
public:
    LayoutEngine();
    ~LayoutEngine();

    LayoutEngine(const LayoutEngine&)            = delete;
    LayoutEngine& operator=(const LayoutEngine&) = delete;
    LayoutEngine(LayoutEngine&&) noexcept;
    LayoutEngine& operator=(LayoutEngine&&) noexcept;

    void attach(detail::DomHandle& dom, StyleEngine& style);

    /// Layout the whole document against the given viewport. Rebuilds
    /// the box tree from scratch.
    LayoutStats layout_full(SizeF viewport_px, float dpi_scale);

    /// Layout only the subtree rooted at `node`. Caller is responsible
    /// for marking only the subtrees whose ComputedStyle changed.
    LayoutStats layout_subtree(NodeId node, SizeF viewport_px, float dpi_scale);

    BoxTree&       boxes()       noexcept;
    const BoxTree& boxes() const noexcept;

    /// Hit-test a viewport-space point against the layout. Returns the
    /// deepest box whose border-box contains the point, or kInvalidBox.
    BoxId hit_test(Vec2 point) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace affineui
