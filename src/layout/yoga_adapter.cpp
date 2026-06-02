#include "layout/yoga_adapter.h"

#include <algorithm>
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
                             Painter* /*measurer*/,
                             std::span<RectF> out_float_bounds) {
    // Stub: zero everything out. Real layout requires Yoga which is
    // not linked in this build configuration.
    for (auto& r : out_bounds) r = Rect{};
    for (auto& r : out_float_bounds) r = RectF{};
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

inline bool is_flex_container(ComputedStyle::Display d) noexcept {
    return d == ComputedStyle::Display::Flex ||
           d == ComputedStyle::Display::InlineFlex;
}

inline bool is_row_flex_direction(ComputedStyle::FlexDirection d) noexcept {
    return d == ComputedStyle::FlexDirection::Row ||
           d == ComputedStyle::FlexDirection::RowReverse;
}

void apply_style(YGNodeRef node, const ComputedStyle& cs,
                 int intrinsic_w, int intrinsic_h, int auto_min_w,
                 bool inline_parent,
                 bool flex_parent,
                 ComputedStyle::FlexDirection parent_flex_direction,
                 bool percent_width_indefinite,
                 bool percent_height_indefinite,
                 bool suppress_auto_margin_left,
                 bool suppress_auto_margin_right) {
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
        if (cs.inset_has.top) {
            if (cs.inset_has.top_pct)
                YGNodeStyleSetPositionPercent(node, YGEdgeTop,
                    static_cast<float>(cs.inset_top) / 100.0f);
            else
                YGNodeStyleSetPosition(node, YGEdgeTop,
                    static_cast<float>(cs.inset_top));
        }
        if (cs.inset_has.right) {
            if (cs.inset_has.right_pct)
                YGNodeStyleSetPositionPercent(node, YGEdgeRight,
                    static_cast<float>(cs.inset_right) / 100.0f);
            else
                YGNodeStyleSetPosition(node, YGEdgeRight,
                    static_cast<float>(cs.inset_right));
        }
        if (cs.inset_has.bottom) {
            if (cs.inset_has.bottom_pct)
                YGNodeStyleSetPositionPercent(node, YGEdgeBottom,
                    static_cast<float>(cs.inset_bottom) / 100.0f);
            else
                YGNodeStyleSetPosition(node, YGEdgeBottom,
                    static_cast<float>(cs.inset_bottom));
        }
        if (cs.inset_has.left) {
            if (cs.inset_has.left_pct)
                YGNodeStyleSetPositionPercent(node, YGEdgeLeft,
                    static_cast<float>(cs.inset_left) / 100.0f);
            else
                YGNodeStyleSetPosition(node, YGEdgeLeft,
                    static_cast<float>(cs.inset_left));
        }
    }
    if (cs.css_float != ComputedStyle::Float::None && !flex_parent) {
        YGNodeStyleSetPositionType(node, YGPositionTypeAbsolute);
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
    const bool is_grid_container =
        cs.display == ComputedStyle::Display::Grid ||
        cs.display == ComputedStyle::Display::InlineGrid;
    if (is_flex_container(cs.display)) {
        YGNodeStyleSetFlexDirection (node, to_yg(cs.flex_direction));
        YGNodeStyleSetJustifyContent(node, to_yg(cs.justify_content));
        YGNodeStyleSetAlignItems    (node, to_yg(cs.align_items));
        YGNodeStyleSetFlexWrap      (node, to_yg(cs.flex_wrap));
        if (cs.row_gap    > 0) YGNodeStyleSetGap(node, YGGutterRow,    static_cast<float>(cs.row_gap));
        if (cs.column_gap > 0) YGNodeStyleSetGap(node, YGGutterColumn, static_cast<float>(cs.column_gap));
    } else if (is_grid_container) {
        // Minimal CSS grid support: model auto-placed grid items as a
        // single stretched column. This matches Bootstrap's `.d-grid`
        // button stacks and keeps gap semantics in the layout layer.
        YGNodeStyleSetFlexDirection(node, YGFlexDirectionColumn);
        YGNodeStyleSetAlignItems   (node, YGAlignStretch);
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
        cs.display == ComputedStyle::Display::InlineFlex ||
        cs.display == ComputedStyle::Display::InlineGrid ||
        cs.display == ComputedStyle::Display::TableCell) {
        // A table cell keeps the column width the layout pre-pass assigned
        // it — don't let Yoga shrink it to fit the row.
        YGNodeStyleSetFlexShrink (node, 0.0f);
    }
    const bool forced_no_shrink_item =
        (inline_parent &&
         (cs.display == ComputedStyle::Display::Inline ||
          cs.display == ComputedStyle::Display::InlineBlock ||
          cs.display == ComputedStyle::Display::InlineFlex ||
          cs.display == ComputedStyle::Display::InlineGrid)) ||
        cs.display == ComputedStyle::Display::TableCell;

    // CSS vertical-align applies to inline-level boxes, not to ordinary flex
    // items. Document models each inline run as a synthetic flex row, so this
    // is the narrow point where inline vertical alignment can be translated to
    // Yoga without leaking into real author flex containers.
    if (inline_parent) {
        switch (cs.vertical_align) {
            case ComputedStyle::VerticalAlign::Middle:
                YGNodeStyleSetAlignSelf(node, YGAlignCenter);
                break;
            case ComputedStyle::VerticalAlign::Top:
            case ComputedStyle::VerticalAlign::TextTop:
                YGNodeStyleSetAlignSelf(node, YGAlignFlexStart);
                break;
            case ComputedStyle::VerticalAlign::Bottom:
            case ComputedStyle::VerticalAlign::TextBottom:
                YGNodeStyleSetAlignSelf(node, YGAlignFlexEnd);
                break;
            case ComputedStyle::VerticalAlign::Baseline:
            default:
                break;
        }
    }

    // ── Flex item properties ───────────────────────────────────────
    // These apply only when the node's *parent* establishes an author flex
    // formatting context. Plain block flow is implemented with Yoga columns,
    // but CSS flex item properties must not leak into that emulation: a
    // fixed-height block child inside a fixed-height block parent keeps its
    // used height and overflows; it does not flex-shrink to fit.
    if (flex_parent) {
        if (cs.flex_grow != 0) {
            YGNodeStyleSetFlexGrow(node, static_cast<float>(cs.flex_grow));
        }
        YGNodeStyleSetFlexShrink(
            node, forced_no_shrink_item ? 0.0f : static_cast<float>(cs.flex_shrink));
        // flex-basis: percentage takes priority over px value.
        // flex_basis_pct is int8_t (0..100), flex_basis is px (int16_t).
        if (cs.flex_basis_pct >= 0) {
            YGNodeStyleSetFlexBasisPercent(
                node, static_cast<float>(cs.flex_basis_pct));
        } else if (cs.flex_basis >= 0) {
            YGNodeStyleSetFlexBasis(node, static_cast<float>(cs.flex_basis));
        }
    } else {
        YGNodeStyleSetFlexShrink(node, 0.0f);
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
    if (cs.margin_auto.right && !suppress_auto_margin_right) {
        YGNodeStyleSetMarginAuto(node, YGEdgeRight);
    } else {
        YGNodeStyleSetMargin(node, YGEdgeRight, static_cast<float>(cs.margin_right));
    }
    YGNodeStyleSetMargin(node, YGEdgeBottom, static_cast<float>(cs.margin_bottom));
    if (cs.margin_auto.left && !suppress_auto_margin_left) {
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
    YGNodeStyleSetBorder(node, YGEdgeTop,    collapse_container ? 0.0f : to_yoga_edge(cs.used_border_top()));
    YGNodeStyleSetBorder(node, YGEdgeRight,  collapse_container ? 0.0f : to_yoga_edge(cs.used_border_right()));
    YGNodeStyleSetBorder(node, YGEdgeBottom, collapse_container ? 0.0f : to_yoga_edge(cs.used_border_bottom()));
    YGNodeStyleSetBorder(node, YGEdgeLeft,   collapse_container ? 0.0f : to_yoga_edge(cs.used_border_left()));

    const bool flex_basis_set =
        flex_parent && (cs.flex_basis_pct >= 0 || cs.flex_basis >= 0);
    const bool parent_main_axis_row =
        is_row_flex_direction(parent_flex_direction);
    const bool width_from_flex_basis =
        flex_basis_set && parent_main_axis_row;
    const bool height_from_flex_basis =
        flex_basis_set && !parent_main_axis_row;
    const bool percent_main_width =
        flex_parent &&
        parent_main_axis_row &&
        ((cs.width_pct_x100 >= 0 && !percent_width_indefinite) ||
         cs.flex_basis_pct >= 0);

    // ── Intrinsic content size ─────────────────────────────────────
    // Phase 2C: we pre-measure text height (font_size + a small line-h
    // pad) and pass it here. Width is left "auto" so the parent's
    // align-items:stretch fills the cross axis. When real inline /
    // text-wrap arrives, this becomes a YGMeasureFunc that gives Yoga
    // a "given width W, what height H?" answer.
    if (intrinsic_h > 0 && !height_from_flex_basis) {
        YGNodeStyleSetHeight(node, static_cast<float>(intrinsic_h));
    }
    if (intrinsic_w > 0 && !width_from_flex_basis) {
        YGNodeStyleSetWidth(node, static_cast<float>(intrinsic_w));
    }

    // Min/max sizing from ComputedStyle (sentinel -1 means "unset" —
    // skip).
    if (cs.min_width  > 0) {
        YGNodeStyleSetMinWidth(node, static_cast<float>(cs.min_width));
    } else if (flex_parent && cs.min_width < 0 && auto_min_w > 0 &&
               !percent_main_width) {
        // The automatic minimum size protects text-only flex items from
        // collapsing, but it must not overrule a definite main-axis
        // percentage column. Bootstrap grid columns are the common case:
        // their width/flex-basis defines the flex line; wide descendants
        // overflow inside the column instead of forcing the column to wrap.
        int used_auto_min_w = auto_min_w;
        if (!width_from_flex_basis && cs.width > 0) {
            const int definite_w =
                cs.box_sizing == ComputedStyle::BoxSizing::BorderBox
                    ? cs.width
                    : cs.width + cs.padding_left + cs.padding_right +
                          cs.used_border_left() + cs.used_border_right();
            used_auto_min_w = std::min(used_auto_min_w, definite_w);
        }
        YGNodeStyleSetMinWidth(node, static_cast<float>(used_auto_min_w));
    }
    if (cs.max_width  > 0)  YGNodeStyleSetMaxWidth (node, static_cast<float>(cs.max_width));
    if (cs.min_height > 0)  YGNodeStyleSetMinHeight(node, static_cast<float>(cs.min_height));
    if (cs.max_height > 0)  YGNodeStyleSetMaxHeight(node, static_cast<float>(cs.max_height));

    // Width/height: percentage takes priority over px value.
    // width_pct_x100 stores pct × 100 (e.g. 33.33% → 3333), int16_t.
    // height_pct stores integer percent (0..100), int8_t.
    if (!width_from_flex_basis &&
        cs.width_pct_x100 >= 0 && !percent_width_indefinite) {
        YGNodeStyleSetWidthPercent(
            node, static_cast<float>(cs.width_pct_x100) / 100.0f);
    } else if (!width_from_flex_basis && cs.width > 0) {
        YGNodeStyleSetWidth(node, static_cast<float>(cs.width));
    }
    if (!height_from_flex_basis &&
        cs.height_pct >= 0 && !percent_height_indefinite) {
        YGNodeStyleSetHeightPercent(
            node, static_cast<float>(cs.height_pct));
    } else if (!height_from_flex_basis && cs.height > 0) {
        YGNodeStyleSetHeight(node, static_cast<float>(cs.height));
    }
}

struct AutoMarginOverride {
    bool left{false};
    bool right{false};
};

std::int32_t flex_item_main_percent_x100(const ComputedStyle& cs,
                                         ComputedStyle::FlexDirection parent_direction) {
    if (!is_row_flex_direction(parent_direction)) return -1;

    if (cs.flex_basis_pct >= 0) {
        return static_cast<std::int32_t>(cs.flex_basis_pct) * 100;
    }
    if (cs.width_pct_x100 >= 0) {
        return cs.width_pct_x100;
    }
    return -1;
}

void compute_auto_margin_overrides(std::span<const BlockLayoutInput> inputs,
                                   std::span<AutoMarginOverride> overrides) {
    constexpr std::int32_t full_row_percent_x100 = 10000;
    constexpr std::int32_t percent_rounding_tolerance_x100 = 10;

    std::vector<int> first_child(inputs.size(), -1);
    std::vector<int> next_sibling(inputs.size(), -1);
    for (std::size_t child_idx = inputs.size(); child_idx-- > 0;) {
        const int parent_idx = inputs[child_idx].parent_idx;
        if (parent_idx >= 0) {
            next_sibling[child_idx] = first_child[static_cast<std::size_t>(parent_idx)];
            first_child[static_cast<std::size_t>(parent_idx)] =
                static_cast<int>(child_idx);
        }
    }

    for (std::size_t parent_idx = 0; parent_idx < inputs.size(); ++parent_idx) {
        const auto* parent_style = inputs[parent_idx].style;
        if (!parent_style || !is_flex_container(parent_style->display)) continue;
        if (!is_row_flex_direction(parent_style->flex_direction)) continue;

        std::int32_t definite_main_percent_x100 = 0;
        bool has_main_axis_auto_margin = false;
        for (int child = first_child[parent_idx]; child >= 0;
             child = next_sibling[static_cast<std::size_t>(child)]) {
            const auto child_idx = static_cast<std::size_t>(child);
            const auto* child_style = inputs[child_idx].style;
            if (!child_style ||
                child_style->display == ComputedStyle::Display::None ||
                child_style->position == ComputedStyle::Position::Absolute ||
                child_style->position == ComputedStyle::Position::Fixed) {
                continue;
            }

            const std::int32_t pct =
                flex_item_main_percent_x100(*child_style,
                                            parent_style->flex_direction);
            if (pct < 0) {
                definite_main_percent_x100 = -1;
                break;
            }
            definite_main_percent_x100 += pct;
            has_main_axis_auto_margin =
                has_main_axis_auto_margin ||
                child_style->margin_auto.left ||
                child_style->margin_auto.right;
        }

        if (!has_main_axis_auto_margin ||
            definite_main_percent_x100 < 0 ||
            definite_main_percent_x100 <
                full_row_percent_x100 - percent_rounding_tolerance_x100) {
            continue;
        }

        // CSS flexbox treats auto margins as zero while forming flex lines.
        // After the line is known, positive free space is assigned to the
        // auto margins. In a saturated percentage row (Bootstrap's
        // col-md-3 + ms-sm-auto col-md-9, for example) there is no positive
        // free space, so the main-axis auto margins resolve to zero. Yoga
        // otherwise lets the auto margin participate in wrapping and can push
        // the final column to the next line for a narrow width band.
        for (int child = first_child[parent_idx]; child >= 0;
             child = next_sibling[static_cast<std::size_t>(child)]) {
            const auto child_idx = static_cast<std::size_t>(child);
            const auto* child_style = inputs[child_idx].style;
            if (!child_style ||
                child_style->display == ComputedStyle::Display::None ||
                child_style->position == ComputedStyle::Position::Absolute ||
                child_style->position == ComputedStyle::Position::Fixed) {
                continue;
            }
            overrides[child_idx].left = true;
            overrides[child_idx].right = true;
        }
    }
}

}  // namespace

// Per-node context handed to Yoga's measure/baseline callbacks via
// YGNodeSetContext. Lives in a side vector for the duration of the
// layout call.
struct NodeCtx {
    Painter*      painter;
    const char*   text_data;
    std::size_t   text_size;
    std::uint32_t font;
    float         baseline_px;
    float         line_height_mult;
    float         letter_spacing_px;
    float         text_indent_px;
    bool          nowrap;
};

YGSize measure_text_cb(YGNodeConstRef node,
                       float width, YGMeasureMode width_mode,
                       float /*height*/, YGMeasureMode /*height_mode*/) {
    auto* ctx = static_cast<const NodeCtx*>(YGNodeGetContext(node));
    if (!ctx || !ctx->painter || ctx->font == 0) return {0.0f, 0.0f};
    // If width is unconstrained OR white-space: nowrap, pass a large
    // wrap width so text measures as a single line of its natural width.
    const float positive_indent = std::max(0.0f, ctx->text_indent_px);
    const float wrap_w =
        (ctx->nowrap ||
         width_mode == YGMeasureModeUndefined || width <= 0.0f) ? 1e6f : width;
    const auto sz = ctx->painter->measure_text_box(
        ctx->font,
        std::string_view(ctx->text_data, ctx->text_size),
        std::max(1.0f, wrap_w - positive_indent),
        ctx->line_height_mult,
        ctx->letter_spacing_px);
    return YGSize{
        static_cast<float>(sz.width) + positive_indent,
        static_cast<float>(sz.height),
    };
}

float baseline_cb(YGNodeConstRef node, float /*width*/, float height) {
    auto* ctx = static_cast<const NodeCtx*>(YGNodeGetContext(node));
    if (!ctx || ctx->baseline_px <= 0.0f) return height;
    if (ctx->baseline_px > height) return height;
    return ctx->baseline_px;
}

void layout_blocks_with_yoga(int viewport_width_px,
                             std::span<const BlockLayoutInput> inputs,
                             std::span<Rect> out_bounds,
                             Painter* measurer,
                             std::span<RectF> out_float_bounds) {
    assert(inputs.size() == out_bounds.size());
    assert(out_float_bounds.empty() || out_float_bounds.size() == inputs.size());

    YGConfigRef config = YGConfigNew();
    YGConfigSetPointScaleFactor(config, 0.0f);

    // Synthetic root. Stack children vertically (CSS block flow shape).
    YGNodeRef root = YGNodeNewWithConfig(config);
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
    // Per-node callback contexts live in a parallel vector so the
    // raw `NodeCtx*` pointers we hand to Yoga remain stable for
    // the duration of YGNodeCalculateLayout.
    std::vector<YGNodeRef> nodes;
    std::vector<NodeCtx>   node_ctxs(inputs.size());
    std::vector<AutoMarginOverride> auto_margin_overrides(inputs.size());
    compute_auto_margin_overrides(inputs, auto_margin_overrides);
    nodes.reserve(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        YGNodeRef n = YGNodeNewWithConfig(config);
        const auto* parent_style_for_flex =
            (inputs[i].parent_idx >= 0)
                ? inputs[static_cast<std::size_t>(inputs[i].parent_idx)].style
                : nullptr;
        const bool flex_parent =
            inputs[i].parent_idx >= 0 &&
            !inputs[i].inline_parent &&
            parent_style_for_flex &&
            is_flex_container(parent_style_for_flex->display);
        const auto parent_flex_direction = parent_style_for_flex
            ? parent_style_for_flex->flex_direction
            : ComputedStyle::FlexDirection::Row;
        bool percent_width_indefinite = false;
        if (inputs[i].style && inputs[i].style->width_pct_x100 >= 0 &&
            inputs[i].parent_idx >= 0) {
            const auto parent_idx =
                static_cast<std::size_t>(inputs[i].parent_idx);
            const auto* parent_style = inputs[parent_idx].style;
            if (parent_style &&
                is_flex_container(parent_style->display) &&
                parent_style->flex_direction == ComputedStyle::FlexDirection::Row &&
                parent_style->width <= 0 &&
                parent_style->width_pct_x100 < 0 &&
                parent_style->flex_grow == 0 &&
                parent_style->flex_basis < 0 &&
                parent_style->flex_basis_pct < 0 &&
                inputs[parent_idx].parent_idx >= 0) {
                const auto grandparent_idx =
                    static_cast<std::size_t>(inputs[parent_idx].parent_idx);
                const auto* grandparent_style = inputs[grandparent_idx].style;
                percent_width_indefinite =
                    grandparent_style &&
                    is_flex_container(grandparent_style->display);
            }
        }
        bool percent_height_indefinite = false;
        if (inputs[i].style && inputs[i].style->height_pct >= 0 &&
            inputs[i].parent_idx >= 0) {
            const auto parent_idx =
                static_cast<std::size_t>(inputs[i].parent_idx);
            const auto* parent_style = inputs[parent_idx].style;
            percent_height_indefinite =
                parent_style &&
                parent_style->height <= 0 &&
                parent_style->min_height <= 0 &&
                parent_style->height_pct < 0 &&
                parent_style->flex_basis < 0 &&
                parent_style->flex_basis_pct < 0;
        }
        apply_style(n, *inputs[i].style,
                    inputs[i].intrinsic_w_px,
                    inputs[i].intrinsic_h_px,
                    inputs[i].auto_min_w_px,
                    inputs[i].inline_parent,
                    flex_parent,
                    parent_flex_direction,
                    percent_width_indefinite,
                    percent_height_indefinite,
                    auto_margin_overrides[i].left,
                    auto_margin_overrides[i].right);

        // Wire the measure callback for text-bearing leaves. Yoga
        // will call back during layout with the constraint width;
        // the callback runs nvgTextBoxBounds to compute the actual
        // wrapped rendered size for that width.
        const bool has_children = std::any_of(
            inputs.begin(), inputs.end(),
            [i](const BlockLayoutInput& child) {
                return child.parent_idx == static_cast<int>(i);
            });
        const bool has_text_measure =
            measurer != nullptr && !inputs[i].text.empty() &&
            inputs[i].font != 0 && !has_children;
        const bool has_baseline = inputs[i].baseline_px > 0.0f;
        if (has_text_measure || has_baseline) {
            node_ctxs[i] = NodeCtx{
                measurer,
                inputs[i].text.data(),
                inputs[i].text.size(),
                inputs[i].font,
                inputs[i].baseline_px,
                inputs[i].style ? effective_line_height_mult(*inputs[i].style) : 1.0f,
                inputs[i].letter_spacing_px,
                inputs[i].text_indent_px,
                inputs[i].nowrap,
            };
            YGNodeSetContext(n, &node_ctxs[i]);
        }
        if (has_text_measure) {
            YGNodeSetMeasureFunc(n, measure_text_cb);
        }
        if (has_baseline) {
            YGNodeSetBaselineFunc(n, baseline_cb);
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
        float fdx = 0.0f, fdy = 0.0f;
        const bool viewport_fixed =
            inputs[i].style &&
            inputs[i].style->position == ComputedStyle::Position::Fixed;
        if (inputs[i].parent_idx >= 0 && !viewport_fixed) {
            const auto parent_idx =
                static_cast<std::size_t>(inputs[i].parent_idx);
            const auto& parent_rect = out_bounds[parent_idx];
            dx = parent_rect.x;
            dy = parent_rect.y;
            if (!out_float_bounds.empty()) {
                const auto& parent_float = out_float_bounds[parent_idx];
                fdx = parent_float.x;
                fdy = parent_float.y;
            }
        }
        out_bounds[i] = Rect{
            dx + static_cast<int>(local_x + 0.5f),
            dy + static_cast<int>(local_y + 0.5f),
            static_cast<int>(w + 0.5f),
            static_cast<int>(h + 0.5f),
        };
        if (!out_float_bounds.empty()) {
            out_float_bounds[i] = RectF{
                fdx + local_x,
                fdy + local_y,
                w,
                h,
            };
        }
    }

    // Detach + free all nodes. YGNodeFreeRecursive walks the children;
    // calling it on root cleans up everything in one pass.
    YGNodeFreeRecursive(root);
    YGConfigFree(config);
}

#endif  // AFFINEUI_STUB_BUILD

}  // namespace affineui::detail
