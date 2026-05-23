#include "layout/yoga_adapter.h"

#include <cassert>
#include <cstdlib>
#include <vector>

#if !defined(AFFINEUI_STUB_BUILD)
#    include <yoga/Yoga.h>
#endif

namespace affineui::detail {

#if defined(AFFINEUI_STUB_BUILD)

void layout_blocks_with_yoga(int /*viewport_width_px*/,
                             std::span<const BlockLayoutInput> /*inputs*/,
                             std::span<Rect> out_bounds,
                             Painter* /*measurer*/) {
    // Stub: zero everything out. Real layout requires Yoga which is
    // not linked in this build configuration.
    for (auto& r : out_bounds) r = Rect{};
}

#else  // !AFFINEUI_STUB_BUILD

namespace {

// Convert one of our int16 edge values into a Yoga float, treating
// negative as "unset" (Yoga's default is 0 so passing 0 is fine).
inline float to_yoga_edge(std::int16_t v) noexcept {
    return v > 0 ? static_cast<float>(v) : 0.0f;
}

// Map our ComputedStyle::JustifyContent → Yoga's YGJustify.
inline YGJustify to_yg(ComputedStyle::JustifyContent j) noexcept {
    switch (j) {
        case ComputedStyle::JustifyContent::Start:        return YGJustifyFlexStart;
        case ComputedStyle::JustifyContent::End:          return YGJustifyFlexEnd;
        case ComputedStyle::JustifyContent::Center:       return YGJustifyCenter;
        case ComputedStyle::JustifyContent::SpaceBetween: return YGJustifySpaceBetween;
        case ComputedStyle::JustifyContent::SpaceAround:  return YGJustifySpaceAround;
        case ComputedStyle::JustifyContent::SpaceEvenly:  return YGJustifySpaceEvenly;
    }
    return YGJustifyFlexStart;
}

inline YGAlign to_yg(ComputedStyle::AlignItems a) noexcept {
    switch (a) {
        case ComputedStyle::AlignItems::Stretch:  return YGAlignStretch;
        case ComputedStyle::AlignItems::Start:    return YGAlignFlexStart;
        case ComputedStyle::AlignItems::End:      return YGAlignFlexEnd;
        case ComputedStyle::AlignItems::Center:   return YGAlignCenter;
        case ComputedStyle::AlignItems::Baseline: return YGAlignBaseline;
    }
    return YGAlignStretch;
}

inline YGFlexDirection to_yg(ComputedStyle::FlexDirection f) noexcept {
    switch (f) {
        case ComputedStyle::FlexDirection::Row:           return YGFlexDirectionRow;
        case ComputedStyle::FlexDirection::RowReverse:    return YGFlexDirectionRowReverse;
        case ComputedStyle::FlexDirection::Column:        return YGFlexDirectionColumn;
        case ComputedStyle::FlexDirection::ColumnReverse: return YGFlexDirectionColumnReverse;
    }
    return YGFlexDirectionRow;
}

inline YGWrap to_yg(ComputedStyle::FlexWrap w) noexcept {
    switch (w) {
        case ComputedStyle::FlexWrap::NoWrap:      return YGWrapNoWrap;
        case ComputedStyle::FlexWrap::Wrap:        return YGWrapWrap;
        case ComputedStyle::FlexWrap::WrapReverse: return YGWrapWrapReverse;
    }
    return YGWrapNoWrap;
}

// Map our CSS `position` onto Yoga's position type. Yoga has no
// `fixed`; treat it as `absolute` (anchored to the nearest positioned
// ancestor — the synthetic root in our flat tree). `sticky` is mapped
// to `relative` at cascade time, so it never reaches here.
inline YGPositionType to_yg(ComputedStyle::Position p) noexcept {
    switch (p) {
        case ComputedStyle::Position::Static:   return YGPositionTypeStatic;
        case ComputedStyle::Position::Relative: return YGPositionTypeRelative;
        case ComputedStyle::Position::Absolute: return YGPositionTypeAbsolute;
        case ComputedStyle::Position::Fixed:    return YGPositionTypeAbsolute;
    }
    return YGPositionTypeStatic;
}

void apply_style(YGNodeRef node, const ComputedStyle& cs,
                 int intrinsic_w, int intrinsic_h) {
    // ── Box-sizing ─────────────────────────────────────────────────
    // Yoga's *default* is border-box, but the CSS default is
    // content-box (width/height size the content; padding+border add
    // on top). Without honoring the cascade, a 50px height + 32px
    // padding produces a 50px outer rect with content squashed to
    // 50-32 = 18px. Forward whatever `box-sizing` the cascade
    // resolved: content-box (CSS default) or border-box (where the
    // declared size already includes padding+border).
    YGNodeStyleSetBoxSizing(node,
        cs.box_sizing == ComputedStyle::BoxSizing::BorderBox
            ? YGBoxSizingBorderBox
            : YGBoxSizingContentBox);

    // ── Positioned layout ──────────────────────────────────────────
    // `position: relative` shifts the box from its in-flow slot by the
    // specified insets while still reserving that slot; `absolute`
    // (and our `fixed` fallback) takes it out of flow and pins it to
    // the containing block's edges. Yoga implements both natively once
    // we set the position type + the edges the author specified. We
    // only push edges whose presence bit is set so an unspecified side
    // stays "undefined" (auto) — that's what lets an absolute box
    // anchor to top/left vs bottom/right correctly. Insets may be
    // negative, so set them verbatim rather than via to_yoga_edge.
    if (cs.position != ComputedStyle::Position::Static) {
        YGNodeStyleSetPositionType(node, to_yg(cs.position));
        if (cs.inset_has.top)
            YGNodeStyleSetPosition(node, YGEdgeTop,    static_cast<float>(cs.inset_top));
        if (cs.inset_has.right)
            YGNodeStyleSetPosition(node, YGEdgeRight,  static_cast<float>(cs.inset_right));
        if (cs.inset_has.bottom)
            YGNodeStyleSetPosition(node, YGEdgeBottom, static_cast<float>(cs.inset_bottom));
        if (cs.inset_has.left)
            YGNodeStyleSetPosition(node, YGEdgeLeft,   static_cast<float>(cs.inset_left));
    }

    // ── display:none ───────────────────────────────────────────────
    // CSS `display:none` removes the element from layout entirely —
    // no box, no space. Yoga models this via YGDisplayNone which
    // collapses the node and excludes it from the parent's layout.
    // Return early; the remaining style properties don't matter for
    // a node that contributes no geometry.
    if (cs.display == ComputedStyle::Display::None) {
        YGNodeStyleSetDisplay(node, YGDisplayNone);
        return;
    }

    // ── Flex container properties ──────────────────────────────────
    // CSS only honors these when display: flex. For plain block flow
    // (Yoga's column-stretch shape) the values still get pushed —
    // Yoga ignores most of them when there's a single child column.
    // Pushing them universally keeps the adapter branch-free.
    if (cs.display == ComputedStyle::Display::Flex) {
        YGNodeStyleSetFlexDirection (node, to_yg(cs.flex_direction));
        YGNodeStyleSetJustifyContent(node, to_yg(cs.justify_content));
        YGNodeStyleSetAlignItems    (node, to_yg(cs.align_items));
        YGNodeStyleSetFlexWrap      (node, to_yg(cs.flex_wrap));
        if (cs.row_gap    > 0) YGNodeStyleSetGap(node, YGGutterRow,    static_cast<float>(cs.row_gap));
        if (cs.column_gap > 0) YGNodeStyleSetGap(node, YGGutterColumn, static_cast<float>(cs.column_gap));
    } else if (cs.display == ComputedStyle::Display::TableRow) {
        // A table row lays its cells out horizontally; stretch makes every
        // cell as tall as the row (the tallest cell wins).
        YGNodeStyleSetFlexDirection(node, YGFlexDirectionRow);
        YGNodeStyleSetAlignItems   (node, YGAlignStretch);
    } else {
        // Plain block flow shape — column-stretch is what gives us
        // CSS-like vertical stacking with cross-axis fill. Table and
        // TableRowGroup (thead/tbody/tfoot) use this too: they stack
        // their rows vertically and stretch them to the table width.
        YGNodeStyleSetFlexDirection(node, YGFlexDirectionColumn);
        YGNodeStyleSetAlignItems   (node, YGAlignStretch);
    }

    // inline / inline-block items don't stretch to fill their parent
    // — they size to content. flex-shrink: 0 keeps Yoga from
    // compressing the child's preferred (single-line) measured width
    // when the parent has generous room (otherwise the measure
    // callback gets re-run with AtMost(parent_w) and wraps text that
    // fit fine on its own).
    //
    // We deliberately don't touch align-self here — inline children
    // always live inside a synthetic line-box (collect_blocks wraps
    // every inline-run), and the line-box's `align-items: center`
    // gives them the vertical alignment they need. Overriding with
    // flex-start used to top-align the contents and visibly broke
    // mixed button + text rows.
    if (cs.display == ComputedStyle::Display::Inline ||
        cs.display == ComputedStyle::Display::InlineBlock ||
        cs.display == ComputedStyle::Display::TableCell) {
        // A table cell keeps the column width the layout pre-pass assigned
        // it — don't let Yoga shrink it to fit the row.
        YGNodeStyleSetFlexShrink (node, 0.0f);
    }

    // ── Flex item properties ───────────────────────────────────────
    // These apply to the node when its *parent* is a flex container.
    // Yoga reads them only when relevant, so it's safe to push them
    // unconditionally.
    if (cs.flex_grow   != 0) YGNodeStyleSetFlexGrow  (node, static_cast<float>(cs.flex_grow));
    if (cs.flex_shrink != 1) YGNodeStyleSetFlexShrink(node, static_cast<float>(cs.flex_shrink));
    // flex-basis: percentage takes priority over px value.
    // flex_basis_pct is int8_t (0..100), flex_basis is px (int16_t).
    if (cs.flex_basis_pct >= 0) {
        YGNodeStyleSetFlexBasisPercent(
            node, static_cast<float>(cs.flex_basis_pct));
    } else if (cs.flex_basis >= 0) {
        YGNodeStyleSetFlexBasis(node, static_cast<float>(cs.flex_basis));
    }

    // ── Margin (Yoga handles this; we do NOT pre-walk margins
    // ourselves anymore) ────────────────────────────────────────────
    // Margins may be negative (CSS allows it; Bootstrap's grid gutters and
    // .card-subtitle pull-up rely on it). Set them verbatim — to_yoga_edge
    // clamps negatives to 0, which is right for padding/border but wrong
    // for margins.
    //
    // `margin-left:auto` / `margin-right:auto` (recorded via the cascade's
    // margin_auto bits) maps to Yoga's YGNodeStyleSetMarginAuto so block
    // boxes get proper horizontal centering (e.g. Bootstrap's .container).
    YGNodeStyleSetMargin(node, YGEdgeTop,    static_cast<float>(cs.margin_top));
    if (cs.margin_auto.right) {
        YGNodeStyleSetMarginAuto(node, YGEdgeRight);
    } else {
        YGNodeStyleSetMargin(node, YGEdgeRight, static_cast<float>(cs.margin_right));
    }
    YGNodeStyleSetMargin(node, YGEdgeBottom, static_cast<float>(cs.margin_bottom));
    if (cs.margin_auto.left) {
        YGNodeStyleSetMarginAuto(node, YGEdgeLeft);
    } else {
        YGNodeStyleSetMargin(node, YGEdgeLeft, static_cast<float>(cs.margin_left));
    }

    // ── Padding ────────────────────────────────────────────────────
    YGNodeStyleSetPadding(node, YGEdgeTop,    to_yoga_edge(cs.padding_top));
    YGNodeStyleSetPadding(node, YGEdgeRight,  to_yoga_edge(cs.padding_right));
    YGNodeStyleSetPadding(node, YGEdgeBottom, to_yoga_edge(cs.padding_bottom));
    YGNodeStyleSetPadding(node, YGEdgeLeft,   to_yoga_edge(cs.padding_left));

    // ── Border (Yoga treats border like padding for sizing) ────────
    // border-collapse: collapse — a table's borders sit on shared grid
    // lines; they must NOT inset the child rows/cells. Otherwise a row's
    // top/bottom border (Bootstrap's .table-bordered puts 1px on rows)
    // pushes its cells 1px inward, so the row's border-bottom and the
    // cell's own base border-bottom land a pixel apart (doubled horizontal
    // line) and the 1px row-edge strip is left uncovered by the cell's
    // background tint (a white sliver above the cell). Zero the *layout*
    // border on the row-direction containers; paint still draws the line
    // on the grid (see document.cpp collapse edge-snapping).
    const bool collapse_container = cs.border_collapse &&
        (cs.display == ComputedStyle::Display::Table ||
         cs.display == ComputedStyle::Display::TableRowGroup ||
         cs.display == ComputedStyle::Display::TableRow);
    YGNodeStyleSetBorder(node, YGEdgeTop,    collapse_container ? 0.0f : to_yoga_edge(cs.border_top));
    YGNodeStyleSetBorder(node, YGEdgeRight,  collapse_container ? 0.0f : to_yoga_edge(cs.border_right));
    YGNodeStyleSetBorder(node, YGEdgeBottom, collapse_container ? 0.0f : to_yoga_edge(cs.border_bottom));
    YGNodeStyleSetBorder(node, YGEdgeLeft,   collapse_container ? 0.0f : to_yoga_edge(cs.border_left));

    // ── Intrinsic content size ─────────────────────────────────────
    // Phase 2C: we pre-measure text height (font_size + a small line-h
    // pad) and pass it here. Width is left "auto" so the parent's
    // align-items:stretch fills the cross axis. When real inline /
    // text-wrap arrives, this becomes a YGMeasureFunc that gives Yoga
    // a "given width W, what height H?" answer.
    if (intrinsic_h > 0) {
        YGNodeStyleSetHeight(node, static_cast<float>(intrinsic_h));
    }
    if (intrinsic_w > 0) {
        YGNodeStyleSetWidth(node, static_cast<float>(intrinsic_w));
    }

    // Min/max sizing from ComputedStyle (sentinel -1 means "unset" —
    // skip).
    if (cs.min_width  > 0)  YGNodeStyleSetMinWidth (node, static_cast<float>(cs.min_width));
    if (cs.max_width  > 0)  YGNodeStyleSetMaxWidth (node, static_cast<float>(cs.max_width));
    if (cs.min_height > 0)  YGNodeStyleSetMinHeight(node, static_cast<float>(cs.min_height));

    // Width/height: percentage takes priority over px value.
    // width_pct_x100 stores pct × 100 (e.g. 33.33% → 3333), int16_t.
    // height_pct stores integer percent (0..100), int8_t.
    if (cs.width_pct_x100 >= 0) {
        YGNodeStyleSetWidthPercent(
            node, static_cast<float>(cs.width_pct_x100) / 100.0f);
    } else if (cs.width > 0) {
        YGNodeStyleSetWidth(node, static_cast<float>(cs.width));
    }
    if (cs.height_pct >= 0) {
        YGNodeStyleSetHeightPercent(
            node, static_cast<float>(cs.height_pct));
    } else if (cs.height > 0) {
        YGNodeStyleSetHeight(node, static_cast<float>(cs.height));
    }
}

}  // namespace

// Per-text-leaf context handed to Yoga's measure callback via
// YGNodeSetContext. Lives in a side vector for the duration of the
// layout call.
struct MeasureCtx {
    Painter*      painter;
    const char*   text_data;
    std::size_t   text_size;
    std::uint32_t font;
    float         line_height_mult;
    float         letter_spacing_px;
    bool          nowrap;
};

YGSize measure_text_cb(YGNodeConstRef node,
                       float width, YGMeasureMode width_mode,
                       float /*height*/, YGMeasureMode /*height_mode*/) {
    auto* ctx = static_cast<const MeasureCtx*>(YGNodeGetContext(node));
    if (!ctx || !ctx->painter || ctx->font == 0) return {0.0f, 0.0f};
    // If width is unconstrained OR white-space: nowrap, pass a large
    // wrap width so text measures as a single line of its natural width.
    const float wrap_w =
        (ctx->nowrap ||
         width_mode == YGMeasureModeUndefined || width <= 0.0f) ? 1e6f : width;
    const auto sz = ctx->painter->measure_text_box(
        ctx->font,
        std::string_view(ctx->text_data, ctx->text_size),
        wrap_w,
        ctx->line_height_mult,
        ctx->letter_spacing_px);
    return YGSize{
        static_cast<float>(sz.width),
        static_cast<float>(sz.height),
    };
}

void layout_blocks_with_yoga(int viewport_width_px,
                             std::span<const BlockLayoutInput> inputs,
                             std::span<Rect> out_bounds,
                             Painter* measurer) {
    assert(inputs.size() == out_bounds.size());

    // Synthetic root. Stack children vertically (CSS block flow shape).
    YGNodeRef root = YGNodeNew();
    YGNodeStyleSetBoxSizing(root, YGBoxSizingContentBox);
    YGNodeStyleSetFlexDirection(root, YGFlexDirectionColumn);
    YGNodeStyleSetAlignItems(root, YGAlignStretch);
    YGNodeStyleSetWidth(root, static_cast<float>(viewport_width_px));
    // Height: undefined → Yoga grows the root to its content.

    // Build the Yoga tree mirroring the DOM tree. Inputs are in DFS
    // order, so we can build nodes in one pass and insert each as a
    // child of its parent (or the synthetic root for top-level
    // blocks).
    //
    // Per-leaf measure contexts live in a parallel vector so the
    // raw `MeasureCtx*` pointers we hand to Yoga remain stable for
    // the duration of YGNodeCalculateLayout.
    std::vector<YGNodeRef>  nodes;
    std::vector<MeasureCtx> measure_ctxs(inputs.size());
    nodes.reserve(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        YGNodeRef n = YGNodeNew();
        apply_style(n, *inputs[i].style,
                    inputs[i].intrinsic_w_px,
                    inputs[i].intrinsic_h_px);

        // Wire the measure callback for text-bearing leaves. Yoga
        // will call back during layout with the constraint width;
        // the callback runs nvgTextBoxBounds to compute the actual
        // wrapped rendered size for that width.
        if (measurer != nullptr && !inputs[i].text.empty() && inputs[i].font != 0) {
            measure_ctxs[i] = MeasureCtx{
                measurer,
                inputs[i].text.data(),
                inputs[i].text.size(),
                inputs[i].font,
                inputs[i].style ? effective_line_height_mult(*inputs[i].style) : 1.0f,
                inputs[i].letter_spacing_px,
                inputs[i].nowrap,
            };
            YGNodeSetContext(n, &measure_ctxs[i]);
            YGNodeSetMeasureFunc(n, measure_text_cb);
        }

        YGNodeRef parent = (inputs[i].parent_idx < 0)
                               ? root
                               : nodes[static_cast<std::size_t>(inputs[i].parent_idx)];
        const std::uint32_t idx_in_parent =
            static_cast<std::uint32_t>(YGNodeGetChildCount(parent));
        YGNodeInsertChild(parent, n, idx_in_parent);
        nodes.push_back(n);
    }

    // Run the solver. Width is fixed, height is allowed to grow.
    YGNodeCalculateLayout(root,
                          static_cast<float>(viewport_width_px),
                          YGUndefined,
                          YGDirectionLTR);

    // Read back. Yoga reports LEFT/TOP relative to the IMMEDIATE
    // parent — convert to document-relative by accumulating along the
    // parent chain. Since inputs are in DFS order, parent_idx < i for
    // all i, so by the time we read child i we've already resolved
    // out_bounds[parent_idx] to doc-relative.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto* n = nodes[i];
        const float local_x = YGNodeLayoutGetLeft(n);
        const float local_y = YGNodeLayoutGetTop(n);
        const float w       = YGNodeLayoutGetWidth(n);
        const float h       = YGNodeLayoutGetHeight(n);
        int dx = 0, dy = 0;
        if (inputs[i].parent_idx >= 0) {
            const auto& parent_rect = out_bounds[
                static_cast<std::size_t>(inputs[i].parent_idx)];
            dx = parent_rect.x;
            dy = parent_rect.y;
        }
        out_bounds[i] = Rect{
            dx + static_cast<int>(local_x + 0.5f),
            dy + static_cast<int>(local_y + 0.5f),
            static_cast<int>(w + 0.5f),
            static_cast<int>(h + 0.5f),
        };
    }

    // Detach + free all nodes. YGNodeFreeRecursive walks the children;
    // calling it on root cleans up everything in one pass.
    YGNodeFreeRecursive(root);
}

#endif  // AFFINEUI_STUB_BUILD

}  // namespace affineui::detail
