#pragma once

// Yoga layout adapter. The single seam between the engine's
// ComputedStyle world and Yoga's YGNode world.
//
// Why this exists as its own module:
//   • Layout is the highest-risk subsystem (cf. docs/DESIGN.md §
//     Correctness discipline). Containing every Yoga touchpoint to
//     one file keeps the blast radius small and the failure modes
//     isolated.
//   • Yoga's quirks vs CSS (defaults, margin non-collapse, abs
//     positioning) are encoded here once, documented inline, and
//     never leak past this boundary.
//   • Tests can target this adapter directly — feed in styles, read
//     out bounds, no GL or NanoVG required.
//
// Interface contract:
//   Input  : viewport width in px + a span of "block descriptors"
//            (computed style + intrinsic content size).
//   Output : a parallel span of bounds (x, y, w, h in px relative to
//            the viewport origin).
//   Side effects: none. All Yoga nodes allocated here are freed before
//   return. The adapter is stateless across calls.

#include "affineui/geom.h"
#include "affineui/painter.h"
#include "affineui/types.h"
#include "internal/computed_style.h"

#include <cstdint>
#include <array>
#include <span>
#include <string_view>

namespace affineui {
class Painter;
}

namespace affineui::detail {

inline constexpr std::size_t kMaxGridTrackHints = 8;

struct GridTrackHint {
    // Fixed px track when px > 0; fractional track when fr_x100 > 0.
    // Unsupported/auto tracks are represented as 1fr by the CSS parser.
    std::int16_t px{0};
    std::int16_t fr_x100{0};
};

/// One block's worth of input to the layout pass.
struct BlockLayoutInput {
    const ComputedStyle* style;          // not owned, must outlive the call
    int                  intrinsic_w_px; // (unused; reserved for future)
    int                  intrinsic_h_px; // explicit height; ignored when `text` is set
    int                  auto_min_w_px{0};
    /// Index into the same `inputs` span of this block's parent block,
    /// or -1 for a top-level block (child of the synthetic root). Inputs
    /// MUST be in DFS order so every child's `parent_idx` is less than
    /// its own index — the adapter relies on this for the parent-
    /// relative → document-relative coordinate accumulation.
    int                  parent_idx{-1};

    /// When non-empty AND `font` is non-zero AND a `measurer` is
    /// supplied, the adapter installs a Yoga measure callback on this
    /// node. Yoga then asks the callback for the actual rendered
    /// width × height at the cross-axis constraint width — so text
    /// wraps at the container width with real glyph metrics. When
    /// either is missing, `intrinsic_h_px` is used as a fixed height.
    std::string_view     text{};
    std::uint32_t        font{0};

    /// Baseline offset from this node's border-box top, in CSS px.
    /// Zero means "let Yoga synthesize a baseline". Text-bearing leaves
    /// set this from the resolved font metrics so inline rows align by
    /// baseline instead of by rectangle center/bottom.
    float                baseline_px{0.0f};

    /// True when this node's parent is Document's synthetic inline line box.
    /// Only in that context does CSS vertical-align affect cross-axis layout;
    /// ordinary flex items must ignore it.
    bool                 inline_parent{false};

    /// CSS letter-spacing in pixels (0 = normal, no extra spacing).
    float                letter_spacing_px{0.0f};
    /// CSS text-indent in pixels for the first rendered line.
    float                text_indent_px{0.0f};

    /// Explicit CSS grid column template hints parsed by AffineUI and solved
    /// in the vendored Yoga fork.
    std::array<GridTrackHint, kMaxGridTrackHints> grid_columns{};
    std::uint8_t        grid_column_count{0};

    /// When true, the text measure callback passes a very large wrap
    /// width so the text is never broken across lines (CSS
    /// white-space: nowrap). The painter draw path also receives a
    /// huge max-width so the rendered lines match the measure.
    bool                 nowrap{false};
};

/// Run layout. `measurer` is consulted via Yoga's measure callbacks
/// for any text-bearing leaves — pass the live painter so text wraps
/// against real font metrics. nullptr is OK (text leaves fall back to
/// their `intrinsic_h_px`).
///
/// `viewport_height_px` is the height of the initial containing block (the
/// viewport). It is what `position: fixed` boxes resolve against — a
/// `position:fixed; inset:0` (or `top:0;bottom:0`, or `height:100%`) element
/// fills the viewport rather than collapsing. In-flow content may still exceed
/// it (the document's scrollable content height is computed separately). Pass 0
/// to leave the available height undefined (legacy "grow to content").
/// `body_style` (optional): the BODY element's computed style. The adapter's
/// synthetic root IS the body box (its padding is baked in by the caller);
/// when the body is a flex container, the root takes its flex-direction /
/// justify / align / wrap / gaps instead of the default block-flow column.
void layout_blocks_with_yoga(int viewport_width_px,
                             int viewport_height_px,
                             std::span<const BlockLayoutInput> inputs,
                             std::span<Rect> out_bounds,
                             Painter* measurer = nullptr,
                             std::span<RectF> out_float_bounds = {},
                             const ComputedStyle* body_style = nullptr);

}  // namespace affineui::detail
