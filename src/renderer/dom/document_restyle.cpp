// document_restyle.cpp — part of the AffineUI HTML5 renderer's document implementation.
//
// Split out of the former monolithic document.cpp. Shared document-internal
// types (Block, detail::DocumentImpl, the CSS selector/rule/@media/keyframe
// side tables) live in internal/document_impl.h; cross-file helpers are
// declared there in namespace affineui::detail::doc.

#include "affineui/document.h"

#include "affineui/memory.h"
#include "affineui/painter.h"
#include "affineui/themes.h"
#include "affineui/view.h"
#include "framework/imm/imm_runtime.h"
#include "renderer/style/animated_style.h"
#include "renderer/style/computed_style.h"
#include "core/diag.h"
#include "renderer/dom/document_impl.h"
#include "renderer/style/element_id.h"
#include "core/embed_log.h"
#include "renderer/style/style_resolver.h"
#include "renderer/style/style_store.h"
#include "renderer/layout/yoga_adapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if !defined(AFFINEUI_STUB_BUILD)
#    include <lexbor/css/css.h>
#    include <lexbor/css/declaration.h>
#    include <lexbor/dom/dom.h>
#    include <lexbor/dom/interfaces/attr.h>
#    include <lexbor/html/html.h>
#endif

namespace affineui {

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Build the ancestor chain (deepest â†’ root) for the block at `idx`.
// Walks parent_idx, which collect_blocks set up. Empty when idx == -1.
std::vector<int> build_hover_chain(const std::vector<Block>& blocks, int idx) {
    std::vector<int> chain;
    while (idx >= 0) {
        chain.push_back(idx);
        idx = blocks[static_cast<std::size_t>(idx)].parent_idx;
    }
    return chain;
}
}  // namespace detail
namespace {

// Look up the resolved style of `block_idx`'s parent. Returns the
// document's root style when there is no parent (top-level body).
// Anonymous/synthetic boxes are transparent to inheritance: they are not
// elements, never carry custom properties, and the collect pass resolves
// children against the DOM parent chain that skips them — so a restyle
// must hop past them too, or every var() under an anonymous run resolves
// against an empty custom-prop set (checked boxes losing their accent
// fill after a live aria flip was exactly this).
detail::ResolvedStyle parent_resolved(const detail::DocumentImpl& impl,
                                      int block_idx) {
    int p = impl.blocks[static_cast<std::size_t>(block_idx)].parent_idx;
    while (p >= 0 &&
           impl.style_store.element_of(
               impl.blocks[static_cast<std::size_t>(p)].id) == nullptr) {
        p = impl.blocks[static_cast<std::size_t>(p)].parent_idx;
    }
    if (p < 0) return impl.root_style;
    const auto pid = impl.blocks[static_cast<std::size_t>(p)].id;
    detail::ResolvedStyle rs;
    rs.computed = impl.style_store.computed(pid);
    rs.animated = impl.style_store.animated(pid);
    rs.custom_props = impl.blocks[static_cast<std::size_t>(p)].custom_props;
    return rs;
}

// Apply currently-active pseudo-class overlays (per the block's
// state_bits) on top of `rs`. Shared by detail::restyle_block (dispatch
// path) and the equivalent collect-time path inline above.
// True if `block` is in an explicit selected/checked state (aria-selected /
// aria-checked = "true"). Such a state is styled by an attribute rule that, in
// decius, has specificity equal to (and source order after) the matching
// `:hover` rule — so it wins the cascade. The pseudo overlay (which can't see
// per-property cascade provenance) approximates that by not letting :hover
// repaint a selected/checked element. (A full specificity-aware overlay is a
// deeper renderer change; this fixes the common selection-vs-hover case.)
bool block_is_selected_state(const Block& block) {
    for (const auto& a : block.attrs) {
        if ((a.first == "aria-selected" || a.first == "aria-checked") &&
            a.second == "true")
            return true;
    }
    return false;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void apply_pseudo_overlay(detail::DocumentImpl& impl, const Block& block,
                          detail::ResolvedStyle& rs) {
    const bool selected = block_is_selected_state(block);
    for (const auto& pr : impl.pseudo_rules) {
        const std::uint8_t bit = detail::pseudo_state_bit(pr.pseudo);
        if (bit == 0) continue;
        // Don't let :hover repaint an explicitly selected/checked element.
        if (pr.pseudo == PseudoRule::Pseudo::Hover && selected) continue;
        if (!detail::compound_matches(pr.target, block.tag, block.elem_id,
                              block.classes, &block.attrs)) continue;
        if (!detail::ancestor_chain_matches(pr.ancestors, block.parent_idx,
                                    impl.blocks)) continue;
        const bool state_matches = pr.state_on_target
            ? detail::block_has_state(impl, block, pr.state_target, bit)
            : detail::ancestor_has_state(impl, block.parent_idx, pr.state_target, bit);
        if (!state_matches) continue;
        impl.resolver->apply_decl_list(pr.decls, rs);
    }
}
}  // namespace detail
namespace {

bool same_animation(const detail::ResolvedStyle::CssAnimation& a,
                    const detail::ResolvedStyle::CssAnimation& b) {
    return a.name_hash == b.name_hash
        && a.duration_s == b.duration_s
        && a.delay_s == b.delay_s
        && a.iteration_count == b.iteration_count
        && a.timing == b.timing
        && a.direction == b.direction
        && a.fill_mode == b.fill_mode
        && a.play_state == b.play_state
        && a.active == b.active;
}

bool animation_candidate(const detail::ResolvedStyle::CssAnimation& a) {
    return a.active && a.name_hash != 0;
}

bool computed_change_needs_layout(const detail::ComputedStyle& a,
                                  const detail::ComputedStyle& b) {
    return a.margin_top != b.margin_top ||
        a.margin_right != b.margin_right ||
        a.margin_bottom != b.margin_bottom ||
        a.margin_left != b.margin_left ||
        a.padding_top != b.padding_top ||
        a.padding_right != b.padding_right ||
        a.padding_bottom != b.padding_bottom ||
        a.padding_left != b.padding_left ||
        a.border_top != b.border_top ||
        a.border_right != b.border_right ||
        a.border_bottom != b.border_bottom ||
        a.border_left != b.border_left ||
        a.border_style != b.border_style ||
        a.border_style_sides != b.border_style_sides ||
        a.border_collapse != b.border_collapse ||
        a.height_pct != b.height_pct ||
        a.width != b.width ||
        a.height != b.height ||
        a.min_width != b.min_width ||
        a.max_width != b.max_width ||
        a.min_height != b.min_height ||
        a.max_height != b.max_height ||
        a.inset_top != b.inset_top ||
        a.inset_right != b.inset_right ||
        a.inset_bottom != b.inset_bottom ||
        a.inset_left != b.inset_left ||
        a.font_size_px != b.font_size_px ||
        a.font_weight != b.font_weight ||
        a.line_height_x100 != b.line_height_x100 ||
        a.letter_spacing_x100 != b.letter_spacing_x100 ||
        a.text_indent_value != b.text_indent_value ||
        a.white_space != b.white_space ||
        a.text_transform != b.text_transform ||
        a.text_align != b.text_align ||
        a.list_style_type != b.list_style_type ||
        a.display != b.display ||
        a.position != b.position ||
        a.flex_direction != b.flex_direction ||
        a.font_style != b.font_style ||
        a.vertical_align != b.vertical_align ||
        a.box_sizing != b.box_sizing ||
        a.css_float != b.css_float ||
        a.text_indent_is_pct != b.text_indent_is_pct ||
        a.inset_has.top != b.inset_has.top ||
        a.inset_has.right != b.inset_has.right ||
        a.inset_has.bottom != b.inset_has.bottom ||
        a.inset_has.left != b.inset_has.left ||
        a.inset_has.top_pct != b.inset_has.top_pct ||
        a.inset_has.right_pct != b.inset_has.right_pct ||
        a.inset_has.bottom_pct != b.inset_has.bottom_pct ||
        a.inset_has.left_pct != b.inset_has.left_pct ||
        a.margin_auto.left != b.margin_auto.left ||
        a.margin_auto.right != b.margin_auto.right ||
        a.justify_content != b.justify_content ||
        a.align_items != b.align_items ||
        a.align_self_bits != b.align_self_bits ||
        a.flex_wrap != b.flex_wrap ||
        a.flex_basis_pct != b.flex_basis_pct ||
        a.row_gap != b.row_gap ||
        a.column_gap != b.column_gap ||
        a.flex_grow != b.flex_grow ||
        a.flex_shrink != b.flex_shrink ||
        a.flex_basis != b.flex_basis ||
        a.font_id != b.font_id ||
        a.width_pct_x100 != b.width_pct_x100;
}

detail::ComputedStyle::JustifyContent
inline_justify_for_text_align(detail::ComputedStyle::TextAlign align) {
    using JC = detail::ComputedStyle::JustifyContent;
    switch (align) {
        case detail::ComputedStyle::TextAlign::Center: return JC::Center;
        case detail::ComputedStyle::TextAlign::Right:  return JC::End;
        case detail::ComputedStyle::TextAlign::Left:
        case detail::ComputedStyle::TextAlign::Justify:
        default:                                      return JC::Start;
    }
}

bool restyle_synthetic_block(detail::DocumentImpl& impl, int idx) {
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    auto parent = parent_resolved(impl, idx);
    const auto old_computed = impl.style_store.computed(block.id);

    if (block.synthetic) {
        auto next_computed = old_computed;
        next_computed.font_size_px         = parent.computed.font_size_px;
        next_computed.font_weight          = parent.computed.font_weight;
        next_computed.font_style           = parent.computed.font_style;
        next_computed.line_height_x100     = parent.computed.line_height_x100;
        next_computed.font_id              = parent.computed.font_id;
        next_computed.cursor               = parent.computed.cursor;
        next_computed.text_align           = parent.computed.text_align;
        next_computed.letter_spacing_x100  = parent.computed.letter_spacing_x100;
        next_computed.text_indent_value    = parent.computed.text_indent_value;
        next_computed.text_indent_is_pct   = parent.computed.text_indent_is_pct;
        next_computed.white_space          = parent.computed.white_space;
        next_computed.text_transform       = parent.computed.text_transform;
        next_computed.text_decoration_line =
            parent.computed.text_decoration_line;
        next_computed.justify_content =
            inline_justify_for_text_align(parent.computed.text_align);

        auto next_animated = impl.style_store.animated(block.id);
        next_animated.color_rgba = parent.animated.color_rgba;
        next_animated.text_decoration_rgba =
            parent.animated.text_decoration_rgba;

        impl.style_store.computed(block.id) = next_computed;
        impl.style_store.animated(block.id) = next_animated;
        block.custom_props = parent.custom_props;
        block.base_animated = next_animated;
        return computed_change_needs_layout(old_computed, next_computed);
    }

    if (block.tag == "#text") {
        auto rs = detail::anonymous_text_style(parent);
        impl.style_store.computed(block.id) = rs.computed;
        impl.style_store.animated(block.id) = rs.animated;
        block.custom_props = rs.custom_props;
        block.box_shadows = rs.box_shadows;
        block.base_animated = rs.animated;
        block.animation = rs.animation;
        return computed_change_needs_layout(old_computed, rs.computed);
    }

    // Generated ::before/::after TEXT inherits like anonymous text — a
    // checked box turning its `color` over must recolor its icon glyph —
    // but two properties were baked in from the generated rule itself at
    // collect time and must survive the refresh: the rule's padding, and
    // the rule's own color when it specified one (generated_color_locked).
    // Contentless generated BOXES (append_generated_box) stay untouched:
    // their whole computed style is rule declarations, nothing inherited.
    if ((block.tag == "#before" || block.tag == "#after") &&
        !block.text.empty()) {
        auto rs = detail::anonymous_text_style(parent);
        rs.computed.padding_left  = old_computed.padding_left;
        rs.computed.padding_right = old_computed.padding_right;
        if (block.generated_color_locked) {
            rs.animated.color_rgba =
                impl.style_store.animated(block.id).color_rgba;
        }
        impl.style_store.computed(block.id) = rs.computed;
        impl.style_store.animated(block.id) = rs.animated;
        block.custom_props = rs.custom_props;
        block.base_animated = rs.animated;
        return computed_change_needs_layout(old_computed, rs.computed);
    }

    return false;
}

bool absolute_geometry_change_can_stay_local(detail::ComputedStyle a,
                                             detail::ComputedStyle b) {
    using Position = detail::ComputedStyle::Position;
    if (a.position != b.position ||
        (b.position != Position::Absolute && b.position != Position::Fixed)) {
        return false;
    }

    a.width = b.width = 0;
    a.height = b.height = 0;
    a.height_pct = b.height_pct = -1;
    a.width_pct_x100 = b.width_pct_x100 = -1;
    a.inset_top = b.inset_top = 0;
    a.inset_right = b.inset_right = 0;
    a.inset_bottom = b.inset_bottom = 0;
    a.inset_left = b.inset_left = 0;
    a.inset_has.top = b.inset_has.top = 0;
    a.inset_has.right = b.inset_has.right = 0;
    a.inset_has.bottom = b.inset_has.bottom = 0;
    a.inset_has.left = b.inset_has.left = 0;
    a.inset_has.top_pct = b.inset_has.top_pct = 0;
    a.inset_has.right_pct = b.inset_has.right_pct = 0;
    a.inset_has.bottom_pct = b.inset_has.bottom_pct = 0;
    a.inset_has.left_pct = b.inset_has.left_pct = 0;

    return !computed_change_needs_layout(a, b);
}

int resolve_position_edge(std::int16_t value, bool is_pct, int basis) {
    if (!is_pct) return static_cast<int>(value);
    return static_cast<int>(
        std::lround(static_cast<double>(basis) *
                    (static_cast<double>(value) / 10000.0)));
}

int resolved_outer_length(std::int16_t px,
                          std::int16_t pct_x100,
                          int basis,
                          int fallback,
                          const detail::ComputedStyle& cs,
                          bool horizontal) {
    int length = fallback;
    if (pct_x100 >= 0) {
        length = resolve_position_edge(pct_x100, true, basis);
    } else if (px >= 0) {
        length = static_cast<int>(px);
    }
    if (cs.box_sizing == detail::ComputedStyle::BoxSizing::ContentBox &&
        (pct_x100 >= 0 || px >= 0)) {
        if (horizontal) {
            length += cs.padding_left + cs.padding_right +
                      cs.used_border_left() + cs.used_border_right();
        } else {
            length += cs.padding_top + cs.padding_bottom +
                      cs.used_border_top() + cs.used_border_bottom();
        }
    }
    return std::max(0, length);
}

void translate_subtree_bounds(std::vector<Block>& blocks,
                              int root_idx,
                              int dx,
                              int dy) {
    if (dx == 0 && dy == 0) return;
    for (int idx = root_idx; idx < static_cast<int>(blocks.size()); ++idx) {
        bool in_subtree = false;
        for (int cur = idx; cur >= 0; ) {
            if (cur == root_idx) {
                in_subtree = true;
                break;
            }
            cur = blocks[static_cast<std::size_t>(cur)].parent_idx;
        }
        if (!in_subtree) continue;
        auto& block = blocks[static_cast<std::size_t>(idx)];
        block.bounds.x += dx;
        block.bounds.y += dy;
        block.bounds_f.x += static_cast<float>(dx);
        block.bounds_f.y += static_cast<float>(dy);
    }
}

bool update_absolute_geometry(detail::DocumentImpl& impl,
                              int idx,
                              const detail::ComputedStyle& cs) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return false;
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (block.parent_idx < 0 ||
        block.parent_idx >= static_cast<int>(impl.blocks.size())) {
        return false;
    }

    const auto& parent = impl.blocks[static_cast<std::size_t>(block.parent_idx)];
    const auto& parent_cs = impl.style_store.computed(parent.id);
    Rect containing{
        parent.bounds.x + parent_cs.used_border_left(),
        parent.bounds.y + parent_cs.used_border_top(),
        std::max(0, parent.bounds.w - parent_cs.used_border_left() -
                         parent_cs.used_border_right()),
        std::max(0, parent.bounds.h - parent_cs.used_border_top() -
                         parent_cs.used_border_bottom())
    };
    if (cs.position == detail::ComputedStyle::Position::Fixed &&
        impl.media_viewport_width_px > 0) {
        const int viewport_h =
            impl.media_viewport_height_px > 0
                ? impl.media_viewport_height_px
                : std::max(impl.content_size.height, parent.bounds.h);
        containing = Rect{0, 0, impl.media_viewport_width_px,
                          std::max(0, viewport_h)};
    }

    const auto resolve_horizontal_inset = [&](std::int16_t value,
                                              bool is_pct) {
        return resolve_position_edge(value, is_pct, containing.w);
    };
    const auto resolve_vertical_inset = [&](std::int16_t value,
                                            bool is_pct) {
        return resolve_position_edge(value, is_pct, containing.h);
    };

    Rect next = block.bounds;
    next.w = resolved_outer_length(cs.width, cs.width_pct_x100, containing.w,
                                   next.w, cs, true);
    next.h = resolved_outer_length(
        cs.height,
        cs.height_pct >= 0 ? static_cast<std::int16_t>(cs.height_pct * 100)
                           : static_cast<std::int16_t>(-1),
        containing.h, next.h, cs, false);

    const bool explicit_width = cs.width >= 0 || cs.width_pct_x100 >= 0;
    if (!explicit_width && cs.inset_has.left && cs.inset_has.right) {
        const int left = resolve_horizontal_inset(
            cs.inset_left, cs.inset_has.left_pct);
        const int right = resolve_horizontal_inset(
            cs.inset_right, cs.inset_has.right_pct);
        next.w = std::max(0, containing.w - left - right);
    }
    const bool explicit_height = cs.height >= 0 || cs.height_pct >= 0;
    if (!explicit_height && cs.inset_has.top && cs.inset_has.bottom) {
        const int top = resolve_vertical_inset(
            cs.inset_top, cs.inset_has.top_pct);
        const int bottom = resolve_vertical_inset(
            cs.inset_bottom, cs.inset_has.bottom_pct);
        next.h = std::max(0, containing.h - top - bottom);
    }

    if (cs.inset_has.left) {
        next.x = containing.x +
                 resolve_horizontal_inset(cs.inset_left,
                                          cs.inset_has.left_pct);
    } else if (cs.inset_has.right) {
        next.x = containing.x + containing.w -
                 resolve_horizontal_inset(cs.inset_right,
                                          cs.inset_has.right_pct) -
                 next.w;
    }

    if (cs.inset_has.top) {
        next.y = containing.y +
                 resolve_vertical_inset(cs.inset_top,
                                        cs.inset_has.top_pct);
    } else if (cs.inset_has.bottom) {
        next.y = containing.y + containing.h -
                 resolve_vertical_inset(cs.inset_bottom,
                                        cs.inset_has.bottom_pct) -
                 next.h;
    }

    const int dx = next.x - block.bounds.x;
    const int dy = next.y - block.bounds.y;
    translate_subtree_bounds(impl.blocks, idx, dx, dy);
    block.bounds.w = next.w;
    block.bounds.h = next.h;
    block.bounds_f.w = static_cast<float>(next.w);
    block.bounds_f.h = static_cast<float>(next.h);
    return true;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Re-resolve one block's style, applying any active pseudo overlays.
// Returns true if the computed layout fields changed and the caller
// must schedule layout. Paint-only changes can stay inside the block's
// visual rect and avoid a layout walk.
bool restyle_block(detail::DocumentImpl& impl, int idx) {
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    auto* elem  = impl.style_store.element_of(block.id);
    if (!elem) return restyle_synthetic_block(impl, idx);
    auto parent = parent_resolved(impl, idx);
    auto rs     = impl.resolver->resolve(elem, parent);
    detail::apply_pseudo_overlay(impl, block, rs);
    // Re-apply font-family fills too: restyle_block runs on the dispatch
    // path (hover/active/focus toggles), and pseudo-scoped fills gate on
    // the matching state bit being set.
    const auto sb_rs = impl.style_store.state_bits(block.id);
    std::array<detail::GridTrackHint,
               detail::kMaxGridTrackHints> grid_columns{};
    std::uint8_t grid_column_count = 0;
    detail::apply_font_family_fills(impl, block.tag, block.elem_id, block.classes,
                            block.parent_idx, sb_rs, rs,
                            &grid_columns, &grid_column_count);
    if (auto value = detail::scan_inline_decl_value(elem, "grid-template-columns");
        !value.empty()) {
        grid_column_count = detail::parse_grid_template_columns(
            std::move(value), grid_columns);
    }
    if (rs.computed.display != detail::ComputedStyle::Display::Grid &&
        rs.computed.display != detail::ComputedStyle::Display::InlineGrid) {
        grid_column_count = 0;
    }
    if (block.tag == "textarea") {
        detail::apply_user_textarea_size(impl, elem, rs);
    }
    if (auto kw = detail::scan_inline_keyword(elem, "cursor"); !kw.empty()) {
        rs.computed.cursor = detail::parse_cursor_keyword(kw);
    }
    if (auto kw = detail::scan_inline_keyword(elem, "resize"); !kw.empty()) {
        const auto [resize, has_resize] = detail::parse_resize_keyword(kw);
        if (has_resize) {
            rs.computed.resize = resize;
        }
    }
    const auto old_animation = block.animation;
    const auto old_computed = impl.style_store.computed(block.id);
    const bool grid_template_changed =
        !detail::same_grid_track_hints(block.grid_columns,
                               block.grid_column_count,
                               grid_columns,
                               grid_column_count);
    const bool needs_layout =
        computed_change_needs_layout(old_computed, rs.computed) ||
        grid_template_changed;
    impl.style_store.computed(block.id) = rs.computed;
    impl.style_store.animated(block.id) = rs.animated;
    block.custom_props = rs.custom_props;
    block.box_shadows = rs.box_shadows;
    block.gradient_stops = rs.gradient_stops;
    block.overlay_gradient = rs.overlay_gradient;
    block.grid_columns = grid_columns;
    block.grid_column_count = grid_column_count;
    block.base_animated = rs.animated;
    block.animation = rs.animation;
    bool local_absolute_geometry_update = false;
    if (needs_layout &&
        absolute_geometry_change_can_stay_local(old_computed, rs.computed)) {
        local_absolute_geometry_update =
            update_absolute_geometry(impl, idx, rs.computed);
    }
    if (!same_animation(old_animation, block.animation)) {
        const bool old_candidate = animation_candidate(old_animation);
        const bool new_candidate = animation_candidate(block.animation);
        if (old_candidate && !new_candidate && impl.animation_candidate_count > 0) {
            --impl.animation_candidate_count;
        } else if (!old_candidate && new_candidate) {
            ++impl.animation_candidate_count;
        }
        block.animation_epoch = std::chrono::steady_clock::now();
    }
    return needs_layout && !local_absolute_geometry_update;
}

bool is_descendant_of_or_self(const std::vector<Block>& blocks,
                              int idx,
                              int root_idx) {
    for (int cur = idx; cur >= 0; ) {
        if (cur == root_idx) return true;
        cur = blocks[static_cast<std::size_t>(cur)].parent_idx;
    }
    return false;
}

bool restyle_subtree(detail::DocumentImpl& impl, int root_idx) {
    if (root_idx < 0 || root_idx >= static_cast<int>(impl.blocks.size()))
        return false;
    bool needs_layout = false;
    // DFS append order makes the subtree contiguous — stop at its end
    // rather than testing ancestry against every later block.
    for (int idx = root_idx; idx < static_cast<int>(impl.blocks.size()); ++idx) {
        if (!detail::is_descendant_of_or_self(impl.blocks, idx, root_idx)) break;
        needs_layout = detail::restyle_block(impl, idx) || needs_layout;
    }
    return needs_layout;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool restyle_all_blocks(detail::DocumentImpl& impl) {
    bool needs_layout = false;
    for (int idx = 0; idx < static_cast<int>(impl.blocks.size()); ++idx) {
        needs_layout = detail::restyle_block(impl, idx) || needs_layout;
    }
    return needs_layout;
}
}  // namespace detail
namespace {


bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

bool ascii_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ca = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] + 32) : a[i];
        if (ca != b[i]) return false;
    }
    return true;
}

// True when EVERY declaration in an inline style text is "descendant
// inert": a non-inherited box/paint property that cannot change any
// descendant's computed style. Inline styles never affect selector
// matching, so a style write whose old AND new text pass this check only
// needs the element's own block restyled — not its whole subtree (a
// splitter drag was restyling a 465-block pane at mouse-move rate for a
// flex-basis change). Fails closed: custom properties (`--x` inherits),
// inherited text properties, `display` (subtree reveal semantics), and
// anything unrecognized all fall back to the subtree walk.
bool inline_style_is_descendant_inert(std::string_view css) {
    static constexpr std::string_view kInert[] = {
        "flex", "flex-grow", "flex-shrink", "flex-basis",
        "width", "height", "min-width", "min-height",
        "max-width", "max-height",
        "left", "top", "right", "bottom", "inset",
        "position", "z-index", "order", "align-self",
        "margin", "margin-top", "margin-right", "margin-bottom",
        "margin-left",
        "padding", "padding-top", "padding-right", "padding-bottom",
        "padding-left",
        "overflow", "overflow-x", "overflow-y",
        "opacity", "transform", "pointer-events",
        "background", "background-color",
        "border", "border-top", "border-right", "border-bottom",
        "border-left", "border-color", "border-width", "border-radius",
        "box-shadow", "gap",
    };
    std::size_t pos = 0;
    while (pos < css.size()) {
        const std::size_t end = css.find(';', pos);
        const std::string_view decl = css.substr(
            pos, end == std::string_view::npos ? css.size() - pos
                                               : end - pos);
        pos = end == std::string_view::npos ? css.size() : end + 1;
        const std::size_t colon = decl.find(':');
        const std::string_view prop = detail::trim_css_ws(
            colon == std::string_view::npos ? decl : decl.substr(0, colon));
        if (prop.empty()) continue;  // stray ';' or trailing whitespace
        if (colon == std::string_view::npos) return false;  // malformed
        bool known = false;
        for (const auto inert : kInert) {
            if (ascii_iequals(prop, inert)) { known = true; break; }
        }
        if (!known) return false;
    }
    return true;
}

bool attribute_can_affect_selector_matching(std::string_view name) {
    // Inline style changes are parsed as declarations on the element and
    // SVG geometry attrs (e.g. path "d") should stay on the cheap paint path.
    // These are the live attributes our framework/CSS selectors commonly
    // depend on. The long-term version is a selector attribute-dependency
    // index, but this keeps hot control drags from forcing a full cascade.
    return name == "class" || name == "id" || name == "type" ||
           name == "hidden" ||
           name == "role" || name == "checked" || name == "disabled" ||
           starts_with(name, "aria-") || starts_with(name, "data-");
}

#if !defined(AFFINEUI_STUB_BUILD)
bool selector_simple_depends_on_attribute(const lxb_css_selector_t* sel,
                                          std::string_view name) {
    if (!sel) return false;
    switch (sel->type) {
        case LXB_CSS_SELECTOR_TYPE_CLASS:
            return name == "class";
        case LXB_CSS_SELECTOR_TYPE_ID:
            return name == "id";
        case LXB_CSS_SELECTOR_TYPE_ATTRIBUTE:
            return std::string_view(
                       reinterpret_cast<const char*>(sel->name.data),
                       sel->name.length) == name;
        default:
            return false;
    }
}

bool stylesheet_dependencies_stay_in_mutated_subtree(
    const detail::DocumentImpl& impl,
    std::string_view name) {
    if (auto it = impl.attr_subtree_local_cache.find(std::string(name));
        it != impl.attr_subtree_local_cache.end()) {
        return it->second;
    }
    const bool local = [&] {
    for (auto* sst : impl.sheets) {
        if (!sst || !sst->root) continue;
        auto* rule_list = lxb_css_rule_list(sst->root);
        if (!rule_list) continue;
        for (auto* r = rule_list->first; r != nullptr; r = r->next) {
            if (r->type != LXB_CSS_RULE_STYLE) continue;
            auto* style = lxb_css_rule_style(r);
            if (!style) continue;
            for (auto* sl = style->selector; sl != nullptr; sl = sl->next) {
                bool selector_mentions_attr = false;
                bool selector_is_subtree_local = true;
                for (auto* sel = sl->first; sel != nullptr; sel = sel->next) {
                    if (sel->combinator ==
                            LXB_CSS_SELECTOR_COMBINATOR_SIBLING ||
                        sel->combinator ==
                            LXB_CSS_SELECTOR_COMBINATOR_FOLLOWING ||
                        sel->combinator ==
                            LXB_CSS_SELECTOR_COMBINATOR_CELL) {
                        selector_is_subtree_local = false;
                    }
                    selector_mentions_attr =
                        selector_mentions_attr ||
                        selector_simple_depends_on_attribute(sel, name);
                }
                if (selector_mentions_attr && !selector_is_subtree_local) {
                    return false;
                }
            }
        }
    }
    return true;
    }();
    impl.attr_subtree_local_cache.emplace(std::string(name), local);
    return local;
}

// True when every stylesheet mention of `name` sits in the SUBJECT compound
// of its selector — the rightmost compound, the one that receives the
// declarations. A write to such an attribute can only change which rules
// match THE MUTATED ELEMENT itself: no rule keys a descendant's or
// sibling's styling on it. The expensive lexbor rematch can then be scoped
// to that single element instead of re-running selector matching over the
// whole dirty subtree. All decius `[hidden]`/`[aria-expanded]` rules are
// subject-position, which turns a menu toggle's rematch from
// O(menu subtree × rules) into O(rules). Cached per attribute name;
// invalidated with the locality cache whenever sheets change.
bool attribute_matches_confined_to_subject(const detail::DocumentImpl& impl,
                                           std::string_view name) {
    if (auto it = impl.attr_subject_confined_cache.find(std::string(name));
        it != impl.attr_subject_confined_cache.end()) {
        return it->second;
    }
    const bool confined = [&] {
        for (auto* sst : impl.sheets) {
            if (!sst || !sst->root) continue;
            auto* rule_list = lxb_css_rule_list(sst->root);
            if (!rule_list) continue;
            for (auto* r = rule_list->first; r != nullptr; r = r->next) {
                if (r->type != LXB_CSS_RULE_STYLE) continue;
                auto* style = lxb_css_rule_style(r);
                if (!style) continue;
                for (auto* sl = style->selector; sl != nullptr; sl = sl->next) {
                    // The subject compound starts at the last simple whose
                    // combinator relates it to a PRECEDING compound (anything
                    // but CLOSE, which chains simples within one compound).
                    const lxb_css_selector_t* subject_start = sl->first;
                    for (auto* sel = sl->first; sel != nullptr;
                         sel = sel->next) {
                        if (sel != sl->first &&
                            sel->combinator !=
                                LXB_CSS_SELECTOR_COMBINATOR_CLOSE) {
                            subject_start = sel;
                        }
                    }
                    for (auto* sel = sl->first;
                         sel != nullptr && sel != subject_start;
                         sel = sel->next) {
                        if (selector_simple_depends_on_attribute(sel, name)) {
                            return false;
                        }
                    }
                }
            }
        }
        return true;
    }();
    impl.attr_subject_confined_cache.emplace(std::string(name), confined);
    return confined;
}

bool simple_selector_depends_on_attribute(const SimpleSelector& simple,
                                          std::string_view name) {
    switch (simple.kind) {
        case SimpleSelector::Kind::Class: return name == "class";
        case SimpleSelector::Kind::Id: return name == "id";
        case SimpleSelector::Kind::Attr: return simple.name == name;
        case SimpleSelector::Kind::Tag: return false;
    }
    return false;
}

bool compound_depends_on_attribute(const CompoundSelector& compound,
                                   std::string_view name) {
    return std::any_of(compound.simples.begin(), compound.simples.end(),
                       [name](const SimpleSelector& simple) {
                           return simple_selector_depends_on_attribute(simple,
                                                                       name);
                       });
}

// Could `elem` match `compound` if we ignore the simples that key off the
// attribute being mutated? The mutated attribute's own simples can flip
// either way, so they are excluded; the compound's OTHER static parts
// (tag/class/id/other attrs) must already match for the mutation to be able
// to change this compound's verdict on this element.
bool element_could_match_compound_modulo_attribute(
    lxb_dom_element_t* elem,
    const CompoundSelector& compound,
    std::string_view name) {
    if (!elem) return false;
    const auto tag = detail::tag_name(elem);
    const auto elem_id = detail::attr_string(elem, "id");
    const auto classes = detail::split_classes(detail::attr_string(elem, "class"));
    for (const auto& s : compound.simples) {
        if (simple_selector_depends_on_attribute(s, name)) continue;
        switch (s.kind) {
            case SimpleSelector::Kind::Tag:
                if (s.name != tag) return false;
                break;
            case SimpleSelector::Kind::Id:
                if (s.name != elem_id) return false;
                break;
            case SimpleSelector::Kind::Class:
                if (std::find(classes.begin(), classes.end(), s.name) ==
                    classes.end()) return false;
                break;
            case SimpleSelector::Kind::Attr:
                if (!detail::has_attr(elem, s.name)) return false;
                if (s.attr_value_set && detail::attr_string(elem, s.name) != s.value)
                    return false;
                break;
        }
    }
    return true;
}

// For a class mutation we know exactly which tokens changed; a compound's
// match can only TOGGLE when one of its class simples is among them. A
// compound depending on `class` through some *unchanged* token matches (or
// fails) identically before and after the write. Non-class dependencies
// (id / attr selectors) stay conservative: any dependent simple counts.
bool compound_generated_dependency_toggled(
    const CompoundSelector& compound,
    std::string_view name,
    const std::vector<std::string>& changed_classes) {
    for (const auto& s : compound.simples) {
        if (!simple_selector_depends_on_attribute(s, name)) continue;
        if (s.kind == SimpleSelector::Kind::Class && name == "class") {
            if (std::find(changed_classes.begin(), changed_classes.end(),
                          s.name) != changed_classes.end()) {
                return true;
            }
            continue;  // dependent, but this token didn't change
        }
        return true;
    }
    return false;
}

// Does mutating `name` ON THIS ELEMENT possibly change which ::before/::after
// rules match somewhere? A rule is only affected when a compound that mentions
// the attribute could match this element with the attribute factored out —
// the subject compound (elem is the pseudo's owner), an ancestor compound
// (elem gates a descendant's pseudo), or the previous-adjacent compound (elem
// gates its next sibling's pseudo). Without the element test, every
// aria-expanded write recollected the whole document because tree rows have
// `[aria-expanded]::before` chevrons — even when the mutated element was a
// menubar trigger those rules can never match, which turned every menu
// hover-switch into two full box rebuilds (and, by dropping the still-hidden
// menus' retained boxes, forced a third rebuild on the next reveal).
// `old_value`/`new_value` are the attribute values around the write: for
// class they narrow "depends on class" to "one of the toggled tokens appears
// in the rule" — without that, any class write on any element whose tag/
// classes fit a chevroned rule recollected the whole document, which made
// per-frame widget class toggles (VU meters, step buttons) cost a full box
// rebuild each.
bool generated_content_depends_on_attribute(
    const detail::DocumentImpl& impl,
    std::string_view name,
    lxb_dom_element_t* elem,
    std::string_view old_value,
    std::string_view new_value) {
    std::vector<std::string> changed_classes;
    if (name == "class") {
        const auto oldc = detail::split_classes(old_value);
        const auto newc = detail::split_classes(new_value);
        for (const auto& c : oldc) {
            if (std::find(newc.begin(), newc.end(), c) == newc.end()) {
                changed_classes.push_back(c);
            }
        }
        for (const auto& c : newc) {
            if (std::find(oldc.begin(), oldc.end(), c) == oldc.end()) {
                changed_classes.push_back(c);
            }
        }
        if (changed_classes.empty()) return false;
    }
    for (const auto& rule : impl.generated_content_rules) {
        if (compound_generated_dependency_toggled(rule.target, name,
                                                  changed_classes) &&
            element_could_match_compound_modulo_attribute(elem, rule.target,
                                                          name)) {
            return true;
        }
        if (rule.has_previous_adjacent &&
            compound_generated_dependency_toggled(rule.previous_adjacent,
                                                  name, changed_classes) &&
            element_could_match_compound_modulo_attribute(
                elem, rule.previous_adjacent, name)) {
            return true;
        }
        for (const auto& ancestor : rule.ancestors) {
            if (compound_generated_dependency_toggled(ancestor, name,
                                                      changed_classes) &&
                element_could_match_compound_modulo_attribute(elem, ancestor,
                                                              name)) {
                return true;
            }
        }
    }
    return false;
}

#endif

void reset_dynamic_block_state(detail::DocumentImpl& impl) {
    impl.hovered_idx = -1;
    impl.active_idx = -1;
    impl.focused_idx = -1;
    impl.hovered_chain.clear();
    impl.active_chain.clear();
    impl.pending_dirty_roots.clear();
    impl.content_size = Size{0, 0};
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void recollect_blocks_from_current_dom(detail::DocumentImpl& impl) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl.doc) return;

    const auto previous_scroll =
        detail::snapshot_scroll_state(impl, /*include_elements=*/true);
    // Pseudo-state (:hover/:active/:focus) must survive the rebuild: the
    // collect pass consults state bits when applying pseudo overlays, and a
    // hover-revealed subtree (e.g. a submenu opened by
    // `.item:hover > .sub{display:block}`) collapses back to display:none if
    // the bits vanish mid-recollect. Snapshot by element, replay after reset.
    std::vector<std::pair<lxb_dom_element_t*, std::uint8_t>> live_state;
    impl.style_store.each([&](detail::ElementId id,
                              const detail::ComputedStyle&,
                              const detail::AnimatedStyle&) {
        const auto bits = impl.style_store.state_bits(id);
        if (bits == 0) return;
        if (auto* elem = impl.style_store.element_of(id)) {
            live_state.emplace_back(elem, bits);
        }
    });
    impl.blocks.clear();
    impl.style_store.reset();
    for (const auto& [elem, bits] : live_state) {
        impl.style_store.state_bits(impl.style_store.acquire(elem)) = bits;
    }
    impl.animation_candidate_count = 0;
    if (impl.resolver) impl.resolver->clear();

    impl.root_style                       = detail::ResolvedStyle{};
    impl.root_style.animated.color_rgba   = 0xDCDCE6FFu;
    impl.root_style.computed.font_size_px = 16;
    impl.root_style.computed.font_weight  = 400;

    auto* body = lxb_html_document_body_element(impl.doc);
    if (body && impl.resolver) {
        detail::ResolvedStyle html_style = impl.root_style;
        auto* body_node = lxb_dom_interface_node(body);
        if (body_node->parent != nullptr &&
            body_node->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            html_style = impl.resolver->resolve(
                lxb_dom_interface_element(body_node->parent), impl.root_style);
        }
        auto* body_elem = lxb_dom_interface_element(body_node);
        impl.root_style = impl.resolver->resolve(body_elem, html_style);
        detail::apply_font_family_fills(impl, "body", detail::attr_string(body_elem, "id"),
                                detail::split_classes(detail::attr_string(body_elem, "class")),
                                /*parent_idx=*/-1,
                                /*state_bits=*/0,
                                impl.root_style);
    }

    detail::collect_blocks(impl,
                   body ? lxb_dom_interface_node(body)
                        : lxb_dom_interface_node(impl.doc),
                   impl.root_style,
                   /*parent_idx=*/-1);
    detail::restore_scroll_state(impl, previous_scroll);
    for (const auto& block : impl.blocks) {
        if (block.animation.active && block.animation.name_hash != 0) {
            ++impl.animation_candidate_count;
        }
    }

    reset_dynamic_block_state(impl);
    // Rebuild the :hover/:active chains from the replayed state bits so the
    // next refresh_pseudo_chain diff can CLEAR them when the pointer moves —
    // an empty chain beside live bits would leave hover styling stuck on.
    for (std::size_t i = 0; i < impl.blocks.size(); ++i) {
        const auto bits = impl.style_store.state_bits(impl.blocks[i].id);
        if (bits & kHoverStateBit) {
            impl.hovered_chain.push_back(static_cast<int>(i));
        }
        if (bits & kActiveStateBit) {
            impl.active_chain.push_back(static_cast<int>(i));
        }
    }
    impl.paint_dirty = true;
#else
    (void)impl;
#endif
}
}  // namespace detail
namespace {

lxb_status_t rematch_stylesheet_matches(lxb_dom_node_t* node) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!node) return LXB_STATUS_OK;

    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && node->ns == LXB_NS_HTML) {
        const auto status = lxb_html_document_element_styles_rematch(
            lxb_html_interface_element(node));
        if (status != LXB_STATUS_OK) return status;
    }

    for (auto* child = lxb_dom_node_first_child(node);
         child != nullptr; child = lxb_dom_node_next(child)) {
        const auto status = rematch_stylesheet_matches(child);
        if (status != LXB_STATUS_OK) return status;
    }
#else
    (void)node;
#endif
    return LXB_STATUS_OK;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool rematch_stylesheet_matches_for_subtree(detail::DocumentImpl& impl,
                                            int root_idx) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl.doc) return false;

    lxb_dom_node_t* root_node = nullptr;
    if (root_idx >= 0 && root_idx < static_cast<int>(impl.blocks.size())) {
        auto* elem = impl.style_store.element_of(
            impl.blocks[static_cast<std::size_t>(root_idx)].id);
        if (elem) root_node = lxb_dom_interface_node(elem);
    }

    if (!root_node) {
        auto* body = lxb_html_document_body_element(impl.doc);
        root_node = body ? lxb_dom_interface_node(body)
                         : lxb_dom_interface_node(impl.doc);
    }

    return rematch_stylesheet_matches(root_node) == LXB_STATUS_OK;
#else
    (void)impl;
    (void)root_idx;
    return false;
#endif
}
}  // namespace detail
namespace {


bool rect_valid(const Rect& r) {
    return r.w > 0 && r.h > 0;
}

Rect union_rect(const Rect& a, const Rect& b) {
    if (!rect_valid(a)) return b;
    if (!rect_valid(b)) return a;
    const int x0 = std::min(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w);
    const int y1 = std::max(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void add_dirty_rect(detail::DocumentImpl& impl, const Rect& r) {
    if (!rect_valid(r)) return;
    impl.dirty_rects.push_back(r);
}
}  // namespace detail
namespace {

Rect shadow_extent(const Rect& base,
                   const detail::BoxShadowLayer& layer) {
    if (layer.inset || (layer.rgba & 0xFFu) == 0) return base;
    if (layer.blur == 0 && layer.spread == 0 &&
        layer.offset_x == 0 && layer.offset_y == 0) {
        return base;
    }
    const int out = std::max(0, static_cast<int>(layer.blur)) +
                    std::max(0, static_cast<int>(layer.spread));
    return Rect{
        base.x + static_cast<int>(layer.offset_x) - out,
        base.y + static_cast<int>(layer.offset_y) - out,
        base.w + out * 2,
        base.h + out * 2,
    };
}

Rect transform_visual_rect(const Rect& r, const Mat2x3& m) {
    if (!rect_valid(r) || m.is_identity()) return r;
    const Vec2 p0 = m.apply(Vec2{static_cast<float>(r.x),
                                 static_cast<float>(r.y)});
    const Vec2 p1 = m.apply(Vec2{static_cast<float>(r.x + r.w),
                                 static_cast<float>(r.y)});
    const Vec2 p2 = m.apply(Vec2{static_cast<float>(r.x),
                                 static_cast<float>(r.y + r.h)});
    const Vec2 p3 = m.apply(Vec2{static_cast<float>(r.x + r.w),
                                 static_cast<float>(r.y + r.h)});
    const float min_x = std::min({p0.x, p1.x, p2.x, p3.x});
    const float min_y = std::min({p0.y, p1.y, p2.y, p3.y});
    const float max_x = std::max({p0.x, p1.x, p2.x, p3.x});
    const float max_y = std::max({p0.y, p1.y, p2.y, p3.y});
    const int x0 = static_cast<int>(std::floor(min_x)) - 1;
    const int y0 = static_cast<int>(std::floor(min_y)) - 1;
    const int x1 = static_cast<int>(std::ceil(max_x)) + 1;
    const int y1 = static_cast<int>(std::ceil(max_y)) + 1;
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

Rect transform_border_rect(const Rect& r, const Mat2x3& m) {
    if (!rect_valid(r) || m.is_identity()) return r;
    const Vec2 p0 = m.apply(Vec2{static_cast<float>(r.x),
                                 static_cast<float>(r.y)});
    const Vec2 p1 = m.apply(Vec2{static_cast<float>(r.x + r.w),
                                 static_cast<float>(r.y)});
    const Vec2 p2 = m.apply(Vec2{static_cast<float>(r.x),
                                 static_cast<float>(r.y + r.h)});
    const Vec2 p3 = m.apply(Vec2{static_cast<float>(r.x + r.w),
                                 static_cast<float>(r.y + r.h)});
    const float min_x = std::min({p0.x, p1.x, p2.x, p3.x});
    const float min_y = std::min({p0.y, p1.y, p2.y, p3.y});
    const float max_x = std::max({p0.x, p1.x, p2.x, p3.x});
    const float max_y = std::max({p0.y, p1.y, p2.y, p3.y});
    const int x0 = static_cast<int>(std::floor(min_x));
    const int y0 = static_cast<int>(std::floor(min_y));
    const int x1 = static_cast<int>(std::ceil(max_x));
    const int y1 = static_cast<int>(std::ceil(max_y));
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
Rect block_border_visual_rect(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return {};
    const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
    const int dy = detail::scroll_offset_y_for(impl.blocks, impl.style_store, idx);
    const Rect base{b.bounds.x, b.bounds.y - dy, b.bounds.w, b.bounds.h};
    return transform_border_rect(base, effective_transform_for(impl, idx));
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
Rect block_visual_rect(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return {};
    const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
    const int dy = detail::scroll_offset_y_for(impl.blocks, impl.style_store, idx);
    const Rect base{b.bounds.x, b.bounds.y - dy, b.bounds.w, b.bounds.h};
    Rect out = base;
    if (b.box_shadows) {
        for (const auto& layer : *b.box_shadows) {
            out = union_rect(out, shadow_extent(base, layer));
        }
    } else {
        const auto& an = impl.style_store.animated(b.id);
        detail::BoxShadowLayer layer{};
        layer.rgba = an.shadow_rgba;
        layer.offset_x = an.shadow_offset_x;
        layer.offset_y = an.shadow_offset_y;
        layer.blur = an.shadow_blur;
        layer.spread = an.shadow_spread;
        layer.inset = an.shadow_inset;
        out = union_rect(out, shadow_extent(base, layer));
    }
#if !defined(AFFINEUI_STUB_BUILD)
    out = transform_visual_rect(out, effective_transform_for(impl, idx));
#endif
    return out;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool vertical_scrollbar_geometry(const detail::DocumentImpl& impl,
                                 int idx,
                                 ScrollbarGeometry& out) {
    if (!detail::block_is_scrollable_y(impl, idx)) return false;
    const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
    const Rect box = detail::block_border_visual_rect(impl, idx);
    constexpr int kTrackWidth = 6;
    constexpr int kTrackPad = 2;
    if (box.w <= kTrackWidth + kTrackPad * 2 ||
        box.h <= kTrackPad * 2 + 1) {
        return false;
    }

    const int track_h = box.h - 2 * kTrackPad;
    const float ratio =
        static_cast<float>(b.bounds.h) / static_cast<float>(b.content_h);
    const int natural_thumb_h = static_cast<int>(
        std::round(static_cast<float>(track_h) * ratio));
    const int thumb_h = std::clamp(
        std::max(24, natural_thumb_h), 1, std::max(1, track_h));
    const int scroll_range = std::max(1, b.content_h - b.bounds.h);
    const int thumb_travel = std::max(0, track_h - thumb_h);
    const int thumb_y_off = thumb_travel == 0
        ? 0
        : static_cast<int>(
            std::round(static_cast<float>(thumb_travel) *
                       static_cast<float>(b.scroll_y) /
                       static_cast<float>(scroll_range)));

    out.track = Rect{
        box.x + box.w - kTrackWidth - kTrackPad,
        box.y + kTrackPad,
        kTrackWidth,
        track_h,
    };
    out.thumb = Rect{
        out.track.x,
        out.track.y + thumb_y_off,
        out.track.w,
        thumb_h,
    };
    out.scroll_range = scroll_range;
    out.thumb_travel = thumb_travel;
    return true;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool set_block_scroll_y(detail::DocumentImpl& impl, int idx, int scroll_y) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return false;
    if (!detail::block_is_scrollable_y(impl, idx)) return false;
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    const int max_scroll = std::max(0, block.content_h - block.bounds.h);
    const int next = std::clamp(scroll_y, 0, max_scroll);
    if (next == block.scroll_y) return false;

    detail::add_dirty_rect(impl, detail::block_visual_rect(impl, idx));
    block.scroll_y = next;
    detail::add_dirty_rect(impl, detail::block_visual_rect(impl, idx));

    // A virtual list re-windows its rows from the container's live scroll
    // offset. Emit a scroll-change so the app rebuilds the view: the next build
    // reads this offset back (via the scroll provider) and renders the rows now
    // under the viewport. Keyed by the container's widget name; the value is the
    // new pixel offset. Only virtual-list containers opt in (data-aui-virtual),
    // so ordinary scroll boxes cost nothing.
    if (auto* elem = detail::element_for_block(impl, idx)) {
        if (detail::has_attr(elem, "data-aui-virtual")) {
            detail::emit_widget_scroll(impl, elem, next);
        }
    }
    return true;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool scrollbar_scroll_from_thumb_y(detail::DocumentImpl& impl,
                                   int idx,
                                   int thumb_y) {
    ScrollbarGeometry geometry{};
    if (!detail::vertical_scrollbar_geometry(impl, idx, geometry)) return false;
    if (geometry.thumb_travel <= 0) return false;
    const int track_relative = std::clamp(
        thumb_y - geometry.track.y, 0, geometry.thumb_travel);
    const int next = static_cast<int>(
        std::round(static_cast<double>(track_relative) *
                   static_cast<double>(geometry.scroll_range) /
                   static_cast<double>(geometry.thumb_travel)));
    return detail::set_block_scroll_y(impl, idx, next);
}

bool find_vertical_scrollbar_at(const detail::DocumentImpl& impl,
                                Point point,
                                int& out_idx,
                                ScrollbarGeometry& out) {
    for (std::size_t i = impl.blocks.size(); i-- > 0; ) {
        ScrollbarGeometry geometry{};
        if (!detail::vertical_scrollbar_geometry(
                impl, static_cast<int>(i), geometry)) {
            continue;
        }
        if (detail::rect_contains(geometry.track, point.x, point.y)) {
            out_idx = static_cast<int>(i);
            out = geometry;
            return true;
        }
    }
    return false;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
Rect subtree_visual_rect(const detail::DocumentImpl& impl, int root_idx) {
    Rect out{};
    if (root_idx < 0 || root_idx >= static_cast<int>(impl.blocks.size()))
        return out;
    // Blocks are appended in DFS order (collect recurses a whole child
    // subtree before the next sibling), so a subtree is a CONTIGUOUS range:
    // the first non-descendant after the root ends it. Scanning on to the
    // end of the vector made every subtree walk O(document), which is what
    // priced attribute writes on large documents (menu toggles most of all).
    for (int idx = root_idx; idx < static_cast<int>(impl.blocks.size()); ++idx) {
        if (!detail::is_descendant_of_or_self(impl.blocks, idx, root_idx)) break;
        out = union_rect(out, detail::block_visual_rect(impl, idx));
    }
    return out;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
Rect document_visual_rect(const detail::DocumentImpl& impl) {
    Rect out{};
    for (int idx = 0; idx < static_cast<int>(impl.blocks.size()); ++idx) {
        out = union_rect(out, detail::block_visual_rect(impl, idx));
    }
    return out;
}
}  // namespace detail
namespace {


int find_block_by_elem_id(const detail::DocumentImpl& impl,
                          std::string_view elem_id) {
    if (elem_id.empty()) return -1;
    for (std::size_t i = 0; i < impl.blocks.size(); ++i) {
        if (impl.blocks[i].elem_id == elem_id) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
int block_index_for_exact_element(const detail::DocumentImpl& impl,
                                  lxb_dom_element_t* elem) {
    if (!elem) return -1;
    for (std::size_t i = 0; i < impl.blocks.size(); ++i) {
        if (impl.style_store.element_of(impl.blocks[i].id) == elem) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

lxb_dom_element_t* element_for_block(detail::DocumentImpl& impl, int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return nullptr;
    return impl.style_store.element_of(
        impl.blocks[static_cast<std::size_t>(idx)].id);
}

const lxb_dom_element_t* element_for_block(const detail::DocumentImpl& impl,
                                           int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return nullptr;
    return impl.style_store.element_of(
        impl.blocks[static_cast<std::size_t>(idx)].id);
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
lxb_dom_element_t* element_for_block_or_ancestor(detail::DocumentImpl& impl,
                                                 int idx) {
    for (int cur = idx;
         cur >= 0 && cur < static_cast<int>(impl.blocks.size());
         cur = impl.blocks[static_cast<std::size_t>(cur)].parent_idx) {
        if (auto* elem = detail::element_for_block(impl, cur)) return elem;
    }
    return nullptr;
}
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
lxb_dom_element_t* find_dom_element_by_id(lxb_dom_node_t* root,
                                          std::string_view elem_id) {
    if (!root || elem_id.empty()) return nullptr;
    if (root->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        auto* elem = lxb_dom_interface_element(root);
        if (detail::has_attr(elem, "id") && detail::attr_string(elem, "id") == elem_id) {
            return elem;
        }
    }
    for (auto* child = lxb_dom_node_first_child(root);
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (auto* found = detail::find_dom_element_by_id(child, elem_id)) {
            return found;
        }
    }
    return nullptr;
}

lxb_dom_element_t* find_dom_element_by_id(detail::DocumentImpl& impl,
                                          std::string_view elem_id) {
    const int block_idx = find_block_by_elem_id(impl, elem_id);
    if (block_idx >= 0) {
        return impl.style_store.element_of(
            impl.blocks[static_cast<std::size_t>(block_idx)].id);
    }
    if (!impl.doc) return nullptr;
    auto* body = lxb_html_document_body_element(impl.doc);
    auto* root = body ? lxb_dom_interface_node(body)
                      : lxb_dom_interface_node(impl.doc);
    return detail::find_dom_element_by_id(root, elem_id);
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
int block_index_for_element_or_ancestor(const detail::DocumentImpl& impl,
                                        lxb_dom_element_t* elem) {
    if (!elem) return -1;
    for (auto* node = lxb_dom_interface_node(elem);
         node != nullptr; node = node->parent) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* cur_elem = lxb_dom_interface_element(node);
        for (std::size_t i = 0; i < impl.blocks.size(); ++i) {
            if (impl.style_store.element_of(impl.blocks[i].id) == cur_elem) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

void refresh_block_metadata_from_element(Block& block,
                                         lxb_dom_element_t* elem) {
    if (!elem) return;
    block.elem_id = detail::attr_string(elem, "id");
    block.classes = detail::split_classes(detail::attr_string(elem, "class"));
    block.attrs = detail::element_attrs(elem);
    if (block.tag == "img") {
        block.image_src = detail::attr_string(elem, "src");
    }
    if (block.tag == "input" || block.tag == "textarea") {
        block.placeholder = detail::attr_string(elem, "placeholder");
    }
    if (block.tag == "input") {
        block.input_type  = detail::attr_string(elem, "type");
        block.role_attr   = detail::attr_string(elem, "role");
        block.is_checked  = detail::has_attr(elem, "checked");
        block.is_disabled = detail::has_attr(elem, "disabled");
        block.text_control = detail::input_type_accepts_text(block.input_type);
    } else if (block.tag == "textarea") {
        block.is_disabled = detail::has_attr(elem, "disabled");
        block.text_control = true;
    }
}

bool element_has_element_child(lxb_dom_element_t* elem) {
    if (!elem) return false;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) return true;
    }
    return false;
}
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void mark_live_mutation_dirty(detail::DocumentImpl& impl,
                              int dirty_root_idx,
                              const Rect& old_rect,
                              bool needs_layout) {
    const auto dirty_count_before = impl.dirty_rects.size();
    detail::add_dirty_rect(impl, old_rect);
    if (needs_layout) {
        if (dirty_root_idx >= 0) {
            impl.pending_dirty_roots.push_back(dirty_root_idx);
        }
        impl.content_size = Size{0, 0};
    } else if (dirty_root_idx >= 0) {
        detail::add_dirty_rect(impl, detail::subtree_visual_rect(impl, dirty_root_idx));
    }
    if (impl.dirty_rects.size() == dirty_count_before && !needs_layout) {
        impl.paint_dirty = true;
    }
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
lxb_dom_element_t* first_descendant_with_class(lxb_dom_element_t* elem,
                                               std::string_view cls) {
    if (!elem) return nullptr;
    const auto classes = detail::split_classes(detail::attr_string(elem, "class"));
    if (std::find(classes.begin(), classes.end(), cls) != classes.end()) {
        return elem;
    }
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (auto* found = detail::first_descendant_with_class(
                lxb_dom_interface_element(child), cls)) {
            return found;
        }
    }
    return nullptr;
}
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
lxb_dom_element_t* nearest_ancestor_with_class(lxb_dom_element_t* elem,
                                               std::string_view cls) {
    for (auto* current = elem; current != nullptr;
         current = detail::parent_element(current)) {
        const auto classes = detail::split_classes(detail::attr_string(current, "class"));
        if (std::find(classes.begin(), classes.end(), cls) != classes.end()) {
            return current;
        }
    }
    return nullptr;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
lxb_dom_element_t* first_descendant_input(lxb_dom_element_t* elem) {
    if (!elem) return nullptr;
    if (detail::tag_name(elem) == "input") return elem;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (auto* found = detail::first_descendant_input(
                lxb_dom_interface_element(child))) {
            return found;
        }
    }
    return nullptr;
}

lxb_dom_element_t* first_descendant_tag(lxb_dom_element_t* elem,
                                        std::string_view tag) {
    if (!elem) return nullptr;
    if (detail::tag_name(elem) == tag) return elem;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (auto* found = detail::first_descendant_tag(
                lxb_dom_interface_element(child), tag)) {
            return found;
        }
    }
    return nullptr;
}

lxb_dom_element_t* nearest_checkbox_wrapper(lxb_dom_element_t* elem) {
    lxb_dom_element_t* decius_candidate = nullptr;
    for (auto* node = elem ? lxb_dom_interface_node(elem) : nullptr;
         node != nullptr; node = node->parent) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* candidate = lxb_dom_interface_element(node);
        const auto classes = detail::split_classes(detail::attr_string(candidate, "class"));
        const auto widget = detail::attr_string(candidate, "data-aui-widget");
        const bool checkbox_widget = widget == "checkbox";
        const bool decius_widget =
            std::find(classes.begin(), classes.end(), "dcs-check") !=
                classes.end() ||
            std::find(classes.begin(), classes.end(), "dcs-radio") !=
                classes.end() ||
            std::find(classes.begin(), classes.end(), "dcs-switch") !=
                classes.end();
        if (checkbox_widget) return candidate;
        if (decius_widget && decius_candidate == nullptr) {
            decius_candidate = candidate;
        }
    }
    return decius_candidate != nullptr ? decius_candidate : elem;
}

// Box collection omits `display:none` subtrees entirely (no Block — see the
// Display::None `continue` in the collection pass), so a subtree that was
// hidden has no boxes at all. A selector-affecting mutation (e.g. removing a
// `.collapsed` class keyed by `.collapsed > .body{display:none}`) can flip
// such a subtree back to visible — but restyling the *existing* blocks can
// never recreate the missing boxes; only a fresh box collection can. Detect
// exactly that case so we recollect when (and only when) a hidden subtree
// became visible, rather than on every class/aria/data tick (which would wreck
// slider-drag and toggle perf). Run AFTER restyle so parent computed styles
// (which the child inherits from) are already up to date.
//
// Boundary insight: a hidden subtree's root is a DOM child of a still-visible
// element (which keeps its Block). So we only resolve DOM children that have no
// Block of their own — in the common no-display-change case there are none.
bool selector_mutation_reveals_hidden_subtree(detail::DocumentImpl& impl,
                                              int root_idx) {
#if !defined(AFFINEUI_STUB_BUILD)
    using Display = detail::ComputedStyle::Display;
    // root_idx < 0: the mutated element has no blocked ancestor — a
    // BODY-LEVEL subtree (top-level menus/popovers live directly under
    // body and collect no boxes while [hidden]). Body's element children
    // map to the top-level blocks, so a blockless child that now
    // resolves visible is exactly a reveal.
    if (root_idx < 0) {
        if (!impl.resolver || !impl.doc) return false;
        auto* body = lxb_html_document_body_element(impl.doc);
        if (!body) return false;
        for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(body));
             c != nullptr; c = lxb_dom_node_next(c)) {
            if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (c->ns != LXB_NS_HTML) continue;
            auto* child = lxb_dom_interface_element(c);
            if (detail::tag_view(child) == "svg") continue;  // paints, never boxes
            if (detail::block_index_for_exact_element(impl, child) >= 0) continue;
            if (impl.resolver->resolve(child, impl.root_style)
                    .computed.display != Display::None) {
                return true;
            }
        }
        return false;
    }
    if (root_idx >= static_cast<int>(impl.blocks.size())) {
        return false;
    }
    // DFS append order makes the subtree a contiguous block range, and a
    // child element's block (when it has one) always lives inside its
    // parent's range — so one pass over the range yields the complete
    // "has a block" set for every child we'll probe. The per-child
    // detail::block_index_for_exact_element() this replaces scanned the WHOLE
    // document per child, which dominated hidden-toggle dispatch.
    int subtree_end = root_idx;
    std::unordered_set<const lxb_dom_element_t*> blocked_elems;
    for (int idx = root_idx; idx < static_cast<int>(impl.blocks.size());
         ++idx) {
        if (!detail::is_descendant_of_or_self(impl.blocks, idx, root_idx)) break;
        subtree_end = idx + 1;
        if (auto* e = impl.style_store.element_of(
                impl.blocks[static_cast<std::size_t>(idx)].id)) {
            blocked_elems.insert(e);
        }
    }
    for (int idx = root_idx; idx < subtree_end; ++idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        auto* elem = impl.style_store.element_of(block.id);
        if (!elem) continue;
        detail::ResolvedStyle parent_rs;
        parent_rs.computed = impl.style_store.computed(block.id);
        parent_rs.animated = impl.style_store.animated(block.id);
        parent_rs.custom_props = block.custom_props;
        for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
             c != nullptr; c = lxb_dom_node_next(c)) {
            if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            // Only HTML elements go through the HTML style resolver (foreign
            // SVG/MathML elements would crash lxb_html_element_style_walk) — and
            // their box participation is handled by their own subtree pass.
            if (c->ns != LXB_NS_HTML) continue;
            auto* child = lxb_dom_interface_element(c);
            // Inline <svg> never collects boxes — it paints through its
            // parent's block (paint_direct_child_svgs). View-built svg
            // (knob rings/arcs, LCD digits) is HTML-namespace, so without
            // this tag check every such widget reads as a permanently
            // "revealed" hidden subtree and each attr write on it forces
            // a full recollect (measured: 42 recollects per knob drag).
            if (detail::tag_view(child) == "svg") continue;
            if (blocked_elems.count(child) != 0) continue;
            // No Block for this child — it resolved to display:none when boxes
            // were last collected. If it now resolves visible, a hidden subtree
            // needs its boxes (re)created.
            if (impl.resolver &&
                impl.resolver->resolve(child, parent_rs).computed.display !=
                    Display::None) {
                if (std::getenv("AFFINEUI_MENU_TRACE") != nullptr) {
                    std::fprintf(stderr,
                                 "[reveal] blockless child <%s> cls='%s' "
                                 "under blk %d\n",
                                 detail::tag_name(child).c_str(),
                                 detail::attr_string(child, "class").c_str(), idx);
                }
                return true;
            }
        }
    }
#else
    (void) impl;
    (void) root_idx;
#endif
    return false;
}
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// :hover/:active twin of selector_mutation_reveals_hidden_subtree: a pseudo
// rule like `.dcs-menu__item--has-sub:hover > .dcs-menu__sub{display:block}`
// can reveal a subtree that has NO boxes (collection skipped it at
// display:none). Restyling existing blocks can't create the missing boxes —
// only a recollect can. The hidden child's visibility comes from the pseudo
// OVERLAY, so unlike the attribute-path detector we must resolve the child
// AND apply the overlay (with the just-updated state bits) before checking
// display. Only blockless children are resolved, so the steady-state cost of
// hover moves is ~zero.
bool pseudo_state_reveals_hidden_subtree(detail::DocumentImpl& impl,
                                         int root_idx) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (impl.pseudo_rules.empty()) return false;
    if (root_idx < 0 || root_idx >= static_cast<int>(impl.blocks.size())) {
        return false;
    }
    if (!impl.resolver) return false;
    using Display = detail::ComputedStyle::Display;
    // DFS append order: the subtree is contiguous, break at its end.
    for (int idx = root_idx; idx < static_cast<int>(impl.blocks.size());
         ++idx) {
        if (!detail::is_descendant_of_or_self(impl.blocks, idx, root_idx)) break;
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        auto* elem = impl.style_store.element_of(block.id);
        if (!elem) continue;
        detail::ResolvedStyle parent_rs;
        parent_rs.computed = impl.style_store.computed(block.id);
        parent_rs.animated = impl.style_store.animated(block.id);
        parent_rs.custom_props = block.custom_props;
        for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
             c != nullptr; c = lxb_dom_node_next(c)) {
            if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (c->ns != LXB_NS_HTML) continue;
            auto* child = lxb_dom_interface_element(c);
            if (detail::tag_view(child) == "svg") continue;  // paints, never boxes
            if (detail::block_index_for_exact_element(impl, child) >= 0) continue;
            auto rs = impl.resolver->resolve(child, parent_rs);
            Block pseudo_block;
            // acquire (not lookup): overlay state checks index state_bits by
            // id, and a never-collected child has no slot yet.
            pseudo_block.id = impl.style_store.acquire(child);
            pseudo_block.tag = detail::tag_name(child);
            pseudo_block.elem_id = detail::attr_string(child, "id");
            pseudo_block.classes = detail::split_classes(detail::attr_string(child, "class"));
            pseudo_block.attrs = detail::element_attrs(child);
            pseudo_block.parent_idx = idx;
            detail::apply_pseudo_overlay(impl, pseudo_block, rs);
            if (rs.computed.display != Display::None) return true;
        }
    }
#else
    (void) impl;
    (void) root_idx;
#endif
    return false;
}
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// TEMP DEBUG (AFFINEUI_ATTR_CHECK=1): sweep every block's element and
// verify the lexbor attr-list invariants (first/last nullness agree, the
// chain's prev/next links are consistent, every attr owns back to the
// element, and the walk terminates at last_attr). Prints the first
// corrupt element so the corruption WINDOW can be bracketed by call site.
void debug_validate_attr_lists(detail::DocumentImpl& impl, const char* where) {
    static const bool on = std::getenv("AFFINEUI_ATTR_CHECK") != nullptr;
    if (!on) return;
    std::size_t corrupt = 0;
    for (std::size_t i = 0; i < impl.blocks.size(); ++i) {
        auto* e = impl.style_store.element_of(impl.blocks[i].id);
        if (!e) continue;
        const char* what = nullptr;
        if ((e->first_attr == nullptr) != (e->last_attr == nullptr)) {
            what = "first/last nullness mismatch";
        } else {
            std::size_t n = 0;
            lxb_dom_attr_t* prev = nullptr;
            lxb_dom_attr_t* a = e->first_attr;
            for (; a != nullptr; a = a->next) {
                if (a->prev != prev) { what = "prev link broken"; break; }
                if (a->owner != e) { what = "owner mismatch"; break; }
                prev = a;
                if (++n > 64) { what = "chain too long/cyclic"; break; }
            }
            if (!what && prev != e->last_attr) what = "last_attr mismatch";
        }
        if (what) {
            ++corrupt;
            if (corrupt > 3) continue;  // summary line reports the total
            std::string cls;
            for (const auto& c : impl.blocks[i].classes) {
                cls += c;
                cls += ' ';
            }
            std::fprintf(stderr,
                         "[attrcheck:%s] CORRUPT block=%zu tag=%s id=%s "
                         "cls=%s elem=%p ntype=%d nlocal=%u first=%p "
                         "last=%p: %s\n",
                         where, i, impl.blocks[i].tag.c_str(),
                         impl.blocks[i].elem_id.c_str(), cls.c_str(),
                         static_cast<void*>(e),
                         static_cast<int>(lxb_dom_interface_node(e)->type),
                         static_cast<unsigned>(
                             lxb_dom_interface_node(e)->local_name),
                         static_cast<void*>(e->first_attr),
                         static_cast<void*>(e->last_attr), what);
            std::size_t k = 0;
            for (auto* a = e->first_attr; a != nullptr && k < 8;
                 a = a->next, ++k) {
                std::fprintf(stderr,
                             "    attr[%zu]=%p name=%u owner=%p%s prev=%p "
                             "next=%p value=%.32s\n",
                             k, static_cast<void*>(a),
                             static_cast<unsigned>(a->node.local_name),
                             static_cast<void*>(a->owner),
                             a->owner == e ? "(self)" : "(OTHER)",
                             static_cast<void*>(a->prev),
                             static_cast<void*>(a->next),
                             a->value && a->value->data
                                 ? reinterpret_cast<const char*>(
                                       a->value->data)
                                 : "<null>");
            }
            std::fflush(stderr);
        }
    }
    std::fprintf(stderr, "[attrcheck:%s] swept %zu blocks, %zu corrupt\n",
                 where, impl.blocks.size(), corrupt);
    std::fflush(stderr);
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool is_body_element(detail::DocumentImpl& impl, lxb_dom_element_t* elem) {
    auto* body = lxb_html_document_body_element(impl.doc);
    return body != nullptr && elem == lxb_dom_interface_element(body);
}

bool set_attribute_on_element(detail::DocumentImpl& impl,
                              lxb_dom_element_t* elem,
                              std::string_view name,
                              std::string_view value) {
    if (!elem || name.empty()) return false;
    const bool already_present = detail::has_attr(elem, name);
    const std::string old_value =
        already_present ? detail::attr_string(elem, name) : std::string();
    if (already_present && old_value == value) return false;
    detail::MutationTraceTimer trace_timer{"set", name};

    // Sub-phase lap clock for the PRE-branch section (block lookups,
    // predicates, dirty-rect snapshot, generated-content scan): the branch
    // phases below are already traced, so an expensive write with NO
    // breakdown line means the time hides up here.
    const bool mt_on = detail::MutationTraceTimer::enabled();
    auto mt_last = mt_on ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{};
    auto mt_lap = [&]() -> double {
        if (!mt_on) return 0.0;
        const auto now = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(now - mt_last).count();
        mt_last = now;
        return ms;
    };

    const int target_idx = detail::block_index_for_exact_element(impl, elem);
    const int dirty_root_idx =
        target_idx >= 0 ? target_idx
                        : detail::block_index_for_element_or_ancestor(impl, elem);
    const double mt_idx_ms = mt_lap();
    const bool selector_affecting =
        attribute_can_affect_selector_matching(name);
    const bool subtree_local_selectors =
        !selector_affecting ||
        stylesheet_dependencies_stay_in_mutated_subtree(impl, name);
    const double mt_pred_ms = mt_lap();
    const int mutation_dirty_root_idx =
        selector_affecting && !subtree_local_selectors && target_idx >= 0 &&
                impl.blocks[static_cast<std::size_t>(target_idx)].parent_idx >= 0
            ? impl.blocks[static_cast<std::size_t>(target_idx)].parent_idx
            : dirty_root_idx;
    const Rect old_rect = mutation_dirty_root_idx >= 0
                              ? detail::subtree_visual_rect(impl, mutation_dirty_root_idx)
                              : detail::document_visual_rect(impl);
    const double mt_rect_ms = mt_lap();
    const bool recollect_generated_subtree =
        selector_affecting &&
        generated_content_depends_on_attribute(impl, name, elem, old_value,
                                               value);
    const double mt_gen_ms = mt_lap();
    if (mt_on && mt_idx_ms + mt_pred_ms + mt_rect_ms + mt_gen_ms >= 2.0) {
        std::fprintf(stderr,
                     "[attr]   set '%s' PRE idx=%.2f pred=%.2f rect=%.2f "
                     "gen=%.2f ms\n",
                     std::string(name).c_str(), mt_idx_ms, mt_pred_ms,
                     mt_rect_ms, mt_gen_ms);
    }

    if (!lxb_dom_element_set_attribute(elem, detail::as_lxb(name), name.size(),
                                       detail::as_lxb(value), value.size())) {
        return false;
    }
    const double mt_lxb_ms = mt_lap();
    if (mt_on && mt_lxb_ms >= 2.0) {
        std::fprintf(stderr, "[attr]   set '%s' LXB set_attribute=%.2f ms\n",
                     std::string(name).c_str(), mt_lxb_ms);
    }

    // Batched contract: inside a view batch EVERY attr write does only the
    // raw mutation + O(1) bookkeeping; end_view_mutations settles every
    // recorded root with ONE rematch/resolver-clear/restyle/reveal pass.
    // This includes non-selector-affecting attrs (style etc.): their old
    // immediate restyle_subtree walked blocks whose elements THIS batch may
    // already have destroyed — restyle inside the window is never safe.
    if (impl.view_batch_active) {
        if (recollect_generated_subtree) {
            impl.view_structure_dirty = true;  // structural settle supersedes
            return true;
        }
        bool needs_subtree_rematch = false;
        if (selector_affecting) {
            needs_subtree_rematch = true;
            if (detail::is_body_element(impl, elem)) {
                // Body-level flip: the root_style baseline (inherited custom
                // properties) must re-resolve at settle.
                impl.view_batch_root_style_dirty = true;
            }
            if (!detail::is_body_element(impl, elem) &&
                lxb_dom_interface_node(elem)->ns == LXB_NS_HTML &&
                attribute_matches_confined_to_subject(impl, name)) {
                // Element-local rematch is cheap — run it now so batch end
                // only re-matches subtrees for attrs whose rules escape the
                // subject.
                (void) mt_lap();
                (void) lxb_html_document_element_styles_rematch(
                    lxb_html_interface_element(lxb_dom_interface_node(elem)));
                const double mt_el_rematch_ms = mt_lap();
                if (mt_on && mt_el_rematch_ms >= 2.0) {
                    std::fprintf(stderr,
                                 "[attr]   set '%s' BATCH element-rematch"
                                 "=%.2f ms\n",
                                 std::string(name).c_str(), mt_el_rematch_ms);
                }
                needs_subtree_rematch = false;
            }
        }
        bool force_layout = false;
        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            detail::refresh_block_metadata_from_element(block, elem);
            if (block.tag == "img" && name == "src") force_layout = true;
        }
        impl.view_batch_attr_roots.push_back(
            {mutation_dirty_root_idx, old_rect, needs_subtree_rematch,
             force_layout});
        return true;
    }

    bool needs_layout = false;
    if (selector_affecting) {
        auto tp = std::chrono::steady_clock::now();
        const auto phase = [&tp] {
            const auto now = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(now - tp).count();
            tp = now;
            return ms;
        };
        // When the attribute only ever appears in subject compounds, no
        // other element's match set can change — rematch just this element
        // instead of the whole dirty subtree (the dominant cost of menu
        // hidden-toggles on large documents).
        if (!detail::is_body_element(impl, elem) &&
            lxb_dom_interface_node(elem)->ns == LXB_NS_HTML &&
            attribute_matches_confined_to_subject(impl, name)) {
            if (lxb_html_document_element_styles_rematch(
                    lxb_html_interface_element(lxb_dom_interface_node(elem)))
                != LXB_STATUS_OK) {
                return false;
            }
        } else if (!detail::rematch_stylesheet_matches_for_subtree(
                       impl, mutation_dirty_root_idx)) {
            return false;
        }
        const double rematch_ms = phase();
        if (impl.resolver) impl.resolver->clear();
        if (detail::is_body_element(impl, elem)) {
            // Body-level flip: re-resolve the root_style baseline so every
            // descendant restyles against the NEW custom properties.
            detail::refresh_root_style(impl);
        }

        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            detail::refresh_block_metadata_from_element(block, elem);
        }

        // NOTE: `hidden` used to force the whole-document recollect here.
        // HIDING needs no box rebuild — the retained boxes restyle to
        // display:none, exactly like a hover-CSS cascade closing — and
        // UN-hiding is caught by the reveal check below, which recollects
        // only when the subtree's boxes were never created. The
        // unconditional rebuild (blocks + style store + resolver cache,
        // whole document) made every menu open/close a multi-frame stall,
        // so menubar hover-follow skipped triggers under fast sweeps.
        if (recollect_generated_subtree) {
            detail::recollect_blocks_from_current_dom(impl);
            detail::mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                     /*needs_layout=*/true);
            return true;
        }

        const double clear_ms = phase();
        needs_layout = mutation_dirty_root_idx >= 0
                           ? detail::restyle_subtree(impl, mutation_dirty_root_idx)
                           : detail::restyle_all_blocks(impl);
        const double restyle_ms = phase();
        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            if (block.tag == "img" && name == "src") needs_layout = true;
        }
        // If this mutation revealed a previously display:none subtree, the box
        // tree is missing those boxes (collection skips hidden subtrees) and
        // restyle alone can't recreate them — recollect so they reappear.
        // (A negative root means the element has no blocked ancestor — the
        // detector then checks body-level children, where top-level
        // menus/popovers live.)
        phase();
        if (detail::selector_mutation_reveals_hidden_subtree(impl,
                                                     mutation_dirty_root_idx)) {
            detail::recollect_blocks_from_current_dom(impl);
            needs_layout = true;
        }
        const double reveal_ms = phase();
        if (detail::MutationTraceTimer::enabled() &&
            rematch_ms + clear_ms + restyle_ms + reveal_ms >= 1.0) {
            std::fprintf(stderr,
                         "[attr]   set '%s' root=%d rematch=%.2f clear=%.2f "
                         "restyle=%.2f reveal=%.2f\n",
                         std::string(name).c_str(), mutation_dirty_root_idx,
                         rematch_ms, clear_ms, restyle_ms, reveal_ms);
        }
        detail::mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                 needs_layout);
        return true;
    }

    if (target_idx >= 0) {
        if (impl.resolver) impl.resolver->invalidate(elem);
        auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
        detail::refresh_block_metadata_from_element(block, elem);
        (void) mt_lap();  // reset the lap so the print isolates the restyle
        // A style write that is descendant-inert on BOTH sides (old and new
        // text: only non-inherited box/paint props, no `--x`, no `display`)
        // cannot change any descendant's computed style — restyle just this
        // block. Splitter/float/ghost gestures live on this path at
        // mouse-move rate.
        const bool style_local =
            name == "style" &&
            inline_style_is_descendant_inert(old_value) &&
            inline_style_is_descendant_inert(value);
        needs_layout = style_local
                           ? detail::restyle_block(impl, target_idx)
                           : detail::restyle_subtree(impl, target_idx);
        const double tail_restyle_ms = mt_lap();
        if (mt_on && tail_restyle_ms >= 1.0) {
            std::fprintf(stderr,
                         "[attr]   set '%s' TAIL restyle_%s(%d)=%.2f ms\n",
                         std::string(name).c_str(),
                         style_local ? "block" : "subtree", target_idx,
                         tail_restyle_ms);
        }
        if (block.tag == "img" && name == "src") needs_layout = true;
    }
    detail::mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                             needs_layout);
    return true;
}

bool remove_attribute_on_element(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* elem,
                                 std::string_view name) {
    if (!elem || name.empty() || !detail::has_attr(elem, name)) return false;
    const std::string old_value = detail::attr_string(elem, name);
    detail::MutationTraceTimer trace_timer{"remove", name};

    const int target_idx = detail::block_index_for_exact_element(impl, elem);
    const int dirty_root_idx =
        target_idx >= 0 ? target_idx
                        : detail::block_index_for_element_or_ancestor(impl, elem);
    const bool selector_affecting =
        attribute_can_affect_selector_matching(name);
    const bool subtree_local_selectors =
        !selector_affecting ||
        stylesheet_dependencies_stay_in_mutated_subtree(impl, name);
    const int mutation_dirty_root_idx =
        selector_affecting && !subtree_local_selectors && target_idx >= 0 &&
                impl.blocks[static_cast<std::size_t>(target_idx)].parent_idx >= 0
            ? impl.blocks[static_cast<std::size_t>(target_idx)].parent_idx
            : dirty_root_idx;
    const Rect old_rect = mutation_dirty_root_idx >= 0
                              ? detail::subtree_visual_rect(impl, mutation_dirty_root_idx)
                              : detail::document_visual_rect(impl);
    const bool recollect_generated_subtree =
        selector_affecting &&
        generated_content_depends_on_attribute(impl, name, elem, old_value,
                                               {});

    if (lxb_dom_element_remove_attribute(elem, detail::as_lxb(name), name.size())
            != LXB_STATUS_OK) {
        return false;
    }

    // Batched contract — mirrors set_attribute_on_element above (ALL attrs
    // defer to the settle; per-op restyle inside the window is never safe).
    if (impl.view_batch_active) {
        if (recollect_generated_subtree) {
            impl.view_structure_dirty = true;  // structural settle supersedes
            return true;
        }
        bool needs_subtree_rematch = false;
        if (selector_affecting) {
            needs_subtree_rematch = true;
            if (detail::is_body_element(impl, elem)) {
                // Body-level flip: the root_style baseline (inherited custom
                // properties) must re-resolve at settle.
                impl.view_batch_root_style_dirty = true;
            }
            if (!detail::is_body_element(impl, elem) &&
                lxb_dom_interface_node(elem)->ns == LXB_NS_HTML &&
                attribute_matches_confined_to_subject(impl, name)) {
                (void) lxb_html_document_element_styles_rematch(
                    lxb_html_interface_element(lxb_dom_interface_node(elem)));
                needs_subtree_rematch = false;
            }
        }
        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            detail::refresh_block_metadata_from_element(block, elem);
        }
        impl.view_batch_attr_roots.push_back(
            {mutation_dirty_root_idx, old_rect, needs_subtree_rematch,
             /*force_layout=*/false});
        return true;
    }

    bool needs_layout = false;
    if (selector_affecting) {
        auto tp = std::chrono::steady_clock::now();
        const auto phase = [&tp] {
            const auto now = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(now - tp).count();
            tp = now;
            return ms;
        };
        // Subject-confined attribute: rematch only the mutated element
        // (see set_attribute_on_element).
        if (!detail::is_body_element(impl, elem) &&
            lxb_dom_interface_node(elem)->ns == LXB_NS_HTML &&
            attribute_matches_confined_to_subject(impl, name)) {
            if (lxb_html_document_element_styles_rematch(
                    lxb_html_interface_element(lxb_dom_interface_node(elem)))
                != LXB_STATUS_OK) {
                return false;
            }
        } else if (!detail::rematch_stylesheet_matches_for_subtree(
                       impl, mutation_dirty_root_idx)) {
            return false;
        }
        const double rematch_ms = phase();
        if (impl.resolver) impl.resolver->clear();
        if (detail::is_body_element(impl, elem)) {
            // Body-level flip: re-resolve the root_style baseline so every
            // descendant restyles against the NEW custom properties.
            detail::refresh_root_style(impl);
        }

        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            detail::refresh_block_metadata_from_element(block, elem);
        }

        // Same policy as set_attribute_on_element: no unconditional box
        // rebuild for `hidden` — restyle the retained boxes, and let the
        // reveal check below recollect only when this removal exposes a
        // subtree whose boxes were never created (a menu's first open).
        if (recollect_generated_subtree) {
            detail::recollect_blocks_from_current_dom(impl);
            detail::mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                     /*needs_layout=*/true);
            return true;
        }

        phase();
        needs_layout = mutation_dirty_root_idx >= 0
                           ? detail::restyle_subtree(impl, mutation_dirty_root_idx)
                           : detail::restyle_all_blocks(impl);
        const double restyle_ms = phase();
        // Removing an attribute can reveal a previously display:none
        // subtree ([hidden] most of all) whose boxes were never collected;
        // restyle can't create boxes, only a recollect can. This mirrors
        // the reveal check on the set_attribute path.
        phase();
        if (detail::selector_mutation_reveals_hidden_subtree(impl,
                                                     mutation_dirty_root_idx)) {
            detail::recollect_blocks_from_current_dom(impl);
            needs_layout = true;
        }
        const double reveal_ms = phase();
        if (detail::MutationTraceTimer::enabled() &&
            rematch_ms + restyle_ms + reveal_ms >= 1.0) {
            std::fprintf(stderr,
                         "[attr]   remove '%s' root=%d rematch=%.2f "
                         "restyle=%.2f reveal=%.2f\n",
                         std::string(name).c_str(), mutation_dirty_root_idx,
                         rematch_ms, restyle_ms, reveal_ms);
        }
        detail::mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                 needs_layout);
        return true;
    }

    if (target_idx >= 0) {
        if (impl.resolver) impl.resolver->invalidate(elem);
        auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
        detail::refresh_block_metadata_from_element(block, elem);
        needs_layout = detail::restyle_subtree(impl, target_idx);
    }
    detail::mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                             needs_layout);
    return true;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool set_text_on_element(detail::DocumentImpl& impl,
                         lxb_dom_element_t* elem,
                         std::string_view text) {
    if (!elem) return false;
    auto* node = lxb_dom_interface_node(elem);
    if (detail::node_text(node) == text) return false;

    const int target_idx = detail::block_index_for_exact_element(impl, elem);
    if (target_idx < 0) return false;
    const Rect old_rect = detail::subtree_visual_rect(impl, target_idx);
    if (lxb_dom_node_text_content_set(node, detail::as_lxb(text), text.size())
            != LXB_STATUS_OK) {
        return false;
    }
    auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
    // Where does this element's text PAINT? collect_blocks stores element
    // text in an ANONYMOUS "#text" child block (under a synthetic line-box
    // run) when the subtree was collected structurally; simple leaves carry
    // it on the element block itself. Writing the element block while a
    // "#text" descendant exists leaves that descendant's stale glyphs
    // painting beneath every later value (T11: affinetools' live counter
    // drew old+new text stacked) — update the run that actually paints.
    int lone_text_idx = -1;
    bool lone_text_shape = true;
    for (std::size_t i = static_cast<std::size_t>(target_idx) + 1;
         i < impl.blocks.size(); ++i) {
        // Preorder: target's subtree is contiguous; stop at the first block
        // whose ancestry climbs past target_idx without hitting it.
        int a = impl.blocks[i].parent_idx;
        while (a > target_idx) {
            a = impl.blocks[static_cast<std::size_t>(a)].parent_idx;
        }
        if (a != target_idx) break;
        const Block& d = impl.blocks[i];
        if (d.tag == "#text") {
            if (lone_text_idx >= 0) {
                lone_text_shape = false;  // several runs → mixed content
                break;
            }
            lone_text_idx = static_cast<int>(i);
        } else if (!d.synthetic) {
            lone_text_shape = false;  // real element children → mixed
            break;
        }
    }
    if (lone_text_shape && lone_text_idx >= 0) {
        impl.blocks[static_cast<std::size_t>(lone_text_idx)].text =
            std::string(text);
        block.text.clear();  // the parent copy must never double-paint
    } else {
        block.text = std::string(text);
    }
    // An absolutely-positioned text LEAF is layout-isolated: its box
    // takes no part in sibling flow, its width comes from insets/props
    // (not from the text), and text paints straight from block.text +
    // bounds each frame. Live per-move label updates (knob values) then
    // cost a repaint, not a document relayout — the relayout was a
    // measured 5.6 ms on EVERY knob-drag frame of the synth. Any real
    // size change is trued up by the next genuine layout pass.
    const auto& cs = impl.style_store.computed(block.id);
    const bool has_child_block =
        target_idx + 1 < static_cast<int>(impl.blocks.size()) &&
        impl.blocks[static_cast<std::size_t>(target_idx) + 1].parent_idx ==
            target_idx;
    const bool layout_isolated =
        !has_child_block &&
        (cs.position == detail::ComputedStyle::Position::Absolute ||
         cs.position == detail::ComputedStyle::Position::Fixed);
    detail::mark_live_mutation_dirty(impl, target_idx, old_rect,
                             /*needs_layout=*/!layout_isolated);
    return true;
}

double element_attr_double(lxb_dom_element_t* elem,
                           std::string_view name,
                           double fallback) {
    if (!elem || !detail::has_attr(elem, name)) return fallback;
    const auto value = detail::attr_string(elem, name);
    if (value.empty()) return fallback;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    return end == value.c_str() ? fallback : parsed;
}

bool element_attr_true(lxb_dom_element_t* elem, std::string_view name) {
    if (!elem || !detail::has_attr(elem, name)) return false;
    const auto value = detail::attr_string(elem, name);
    return value.empty() || value == "true" || value == "checked" ||
           value == "1";
}
}  // namespace detail
namespace {


std::string widget_event_name(lxb_dom_element_t* elem) {
    // The value-bearing element is often an inner, deliberately unnamed
    // part of a compound widget — a dcs-combo's <input>, say — while the
    // app bound on_change to the NAMED widget node. Resolve like a
    // bubbling DOM change event: the nearest self-or-ancestor carrying a
    // widget name (before this walk an inner-element change was silently
    // dropped, so bubbling only turns "lost" into "delivered").
    for (auto* cur = elem; cur != nullptr;
         cur = detail::parent_element(cur)) {
        if (auto name = detail::attr_string(cur, "data-aui-name");
            !name.empty()) {
            return name;
        }
        if (auto id = detail::attr_string(cur, "id"); !id.empty()) {
            return id;
        }
    }
    return {};
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void emit_widget_change(detail::DocumentImpl& impl,
                        lxb_dom_element_t* elem,
                        std::string_view value,
                        bool live) {
    auto name = widget_event_name(elem);
    if (name.empty()) return;
    impl.changed_widgets.push_back({std::move(name), std::string(value), live});
}
void emit_widget_scroll(detail::DocumentImpl& impl,
                        lxb_dom_element_t* elem,
                        std::int64_t offset) {
    auto name = widget_event_name(elem);
    if (name.empty()) return;
    impl.scrolled_widgets.push_back({std::move(name), std::to_string(offset)});
}
}  // namespace detail

// Forward declarations of detail:: helpers defined later in this TU.
// Placed at namespace scope: putting `void detail::foo(...)` inside an
// anonymous namespace is ill-formed (Clang / GCC reject; MSVC accepts
// silently). Keeping only the arg types qualified is fine — we're
// already in namespace affineui.
namespace detail {
void set_live_text_value(DocumentImpl& impl,
                         int idx,
                         Block& block,
                         std::string value);
void set_live_text_state(DocumentImpl& impl,
                         int idx,
                         Block& block,
                         std::string value,
                         std::size_t caret);
std::string emitted_text_control_value(const Block& block);
}  // namespace detail

namespace {

std::string decius_slider_fill_style(double min, double max, double value,
                                     bool bipolar) {
    const double p = detail::normalized_control_value(value, min, max);
    if (!bipolar) return "width:" + detail::percent_string(p);
    const double start = std::min(0.5, p);
    const double width = std::abs(p - 0.5);
    return "left:" + detail::percent_string(start) + ";right:auto;width:" +
           detail::percent_string(width);
}

std::string decius_slider_thumb_style(double min, double max, double value) {
    return "left:" + detail::percent_string(detail::normalized_control_value(value, min, max));
}

// Replace/append one declaration in an inline style string, preserving
// every other declaration (a fader's inline height must survive the
// drag rewriting --pos).
std::string style_with_decl(std::string_view existing,
                            std::string_view prop,
                            std::string_view value) {
    std::string out;
    std::size_t pos = 0;
    while (pos < existing.size()) {
        auto end = existing.find(';', pos);
        if (end == std::string_view::npos) end = existing.size();
        const auto decl = detail::trim_css_ws(existing.substr(pos, end - pos));
        if (!decl.empty()) {
            const auto colon = decl.find(':');
            const auto name =
                detail::trim_css_ws(colon == std::string_view::npos
                                ? decl
                                : decl.substr(0, colon));
            if (name != prop) {
                out.append(decl);
                out.push_back(';');
            }
        }
        pos = end + 1;
    }
    out.append(prop);
    out.push_back(':');
    out.append(value);
    return out;
}

std::string decius_fader_style(double min, double max, double value) {
    const double p = 1.0 - detail::normalized_control_value(value, min, max);
    return "--pos:" + detail::percent_string(p);
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
double decius_knob_angle(double min, double max, double value) {
    return -135.0 + detail::normalized_control_value(value, min, max) * 270.0;
}
}  // namespace detail
namespace {

std::pair<double, double> decius_knob_ring_point(double deg) {
    constexpr double r = 10.5;
    constexpr double pi = 3.14159265358979323846;
    const double rad = deg * pi / 180.0;
    return {12.0 + r * std::cos(rad), 12.0 + r * std::sin(rad)};
}

std::string decius_knob_arc_path(double min, double max, double value,
                                 bool bipolar) {
    const double p = detail::normalized_control_value(value, min, max);
    const double sweep_degrees = bipolar ? (p - 0.5) * 270.0 : p * 270.0;
    if (std::abs(sweep_degrees) <= 0.5) return {};

    const double start_degrees = bipolar ? -90.0 : -225.0;
    const double end_degrees = start_degrees + sweep_degrees;
    const auto [x0, y0] = decius_knob_ring_point(start_degrees);
    const auto [x1, y1] = decius_knob_ring_point(end_degrees);
    const int large = std::abs(sweep_degrees) > 180.0 ? 1 : 0;
    const int sweep = end_degrees >= start_degrees ? 1 : 0;

    return "M " + detail::compact_number(x0) + " " + detail::compact_number(y0) +
           " A 10.5 10.5 0 " + std::to_string(large) + " " +
           std::to_string(sweep) + " " + detail::compact_number(x1) + " " +
           detail::compact_number(y1);
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
double value_from_x(const Rect& bounds, int x, double min, double max) {
    const double t = bounds.w > 0
        ? std::clamp((static_cast<double>(x) - bounds.x) / bounds.w,
                     0.0, 1.0)
        : 0.0;
    return min + t * (max - min);
}

double value_from_y(const Rect& bounds, int y, double min, double max) {
    const double t = bounds.h > 0
        ? std::clamp((static_cast<double>(y) - bounds.y) / bounds.h,
                     0.0, 1.0)
        : 0.0;
    return min + (1.0 - t) * (max - min);
}
}  // namespace detail
namespace {


constexpr int kLiveDragThresholdPx = 3;
constexpr int kTextareaResizeGripPx = 16;

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool pointer_moved_past_threshold(const Event& ev,
                                  const detail::DocumentImpl::LiveControlDrag& drag) {
    return std::abs(ev.pos.x - drag.start_x) >= kLiveDragThresholdPx ||
           std::abs(ev.pos.y - drag.start_y) >= kLiveDragThresholdPx;
}
}  // namespace detail
namespace {


bool textarea_resize_axes(detail::ComputedStyle::Resize resize,
                          bool& resize_x,
                          bool& resize_y) {
    using R = detail::ComputedStyle::Resize;
    resize_x = resize == R::Both || resize == R::Horizontal;
    resize_y = resize == R::Both || resize == R::Vertical;
    return resize_x || resize_y;
}

Rect textarea_resize_grip_rect(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return {};
    const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (block.tag != "textarea" || block.is_disabled) return {};
    bool resize_x = false;
    bool resize_y = false;
    const auto& cs = impl.style_store.computed(block.id);
    if (!textarea_resize_axes(cs.resize, resize_x, resize_y)) return {};
    const Rect bounds = detail::block_border_visual_rect(impl, idx);
    const int grip = std::min(kTextareaResizeGripPx,
                              std::max(1, std::min(bounds.w, bounds.h)));
    return Rect{bounds.x + bounds.w - grip, bounds.y + bounds.h - grip,
                grip, grip};
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool point_in_textarea_resize_grip(const detail::DocumentImpl& impl,
                                   int idx,
                                   Point point,
                                   bool& resize_x,
                                   bool& resize_y) {
    resize_x = false;
    resize_y = false;
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return false;
    const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (block.tag != "textarea" || block.is_disabled) return false;
    const auto& cs = impl.style_store.computed(block.id);
    if (!textarea_resize_axes(cs.resize, resize_x, resize_y)) return false;
    return detail::rect_contains(textarea_resize_grip_rect(impl, idx),
                         point.x, point.y);
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool update_live_control_value(detail::DocumentImpl& impl,
                               lxb_dom_element_t* elem,
                               LiveControlKind kind,
                               double min,
                               double max,
                               double value,
                               bool bipolar,
                               bool emit_live_change) {
    if (!elem) return false;
    if (max <= min) max = min + 1.0;
    double clamped = std::clamp(value, min, max);
    // Step snapping (three.js/decius.js parity: value = round(v/step)*step).
    // This is what makes a numeric field an INTEGER editor vs a FLOAT
    // one — the single dcs-combo primitive, parameterised by data-step:
    // step 1 snaps to whole numbers, 0.01 (the default) keeps two
    // decimals. A zero/absent step means "no snapping" (free float).
    auto* combo = detail::nearest_ancestor_with_class(elem, "dcs-combo");
    if (!combo && detail::has_attr(elem, "data-dcs-combo")) combo = elem;
    const double step = detail::element_attr_double(
        elem, "step",
        detail::element_attr_double(
            elem, "data-step",
            detail::element_attr_double(combo, "data-step", 0.0)));
    int decimals = 2;
    if (step > 0.0) {
        clamped = std::clamp(std::round(clamped / step) * step, min, max);
        // Decimal places to show = those the step itself needs (step 1 →
        // 0, 0.1 → 1, 0.01 → 2), so an integer editor prints integers. The
        // cap of 9 matches float32 significant-digit precision — a step
        // finer than a nanounit can't be rendered distinctly anyway, so we
        // stop rather than print noise digits.
        decimals = 0;
        double frac = step;
        while (decimals < 9 &&
               std::abs(frac - std::round(frac)) > 1e-12) {
            frac *= 10.0;
            ++decimals;
        }
    }
    const std::string value_text = detail::compact_number(clamped, decimals);

    bool changed = false;
    const bool value_changed =
        detail::set_attribute_on_element(impl, elem, "value", value_text);
    changed = value_changed || changed;
    if (value_changed && kind == LiveControlKind::NumericInput) {
        const int idx = detail::block_index_for_exact_element(impl, elem);
        if (idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(idx)];
            if (block.text_control) {
                detail::set_live_text_value(impl, idx, block, value_text);
            }
        }
        auto* combo = detail::nearest_ancestor_with_class(elem, "dcs-combo");
        if (!combo && detail::has_attr(elem, "data-dcs-combo")) combo = elem;
        if (combo) {
            changed =
                detail::set_attribute_on_element(impl, combo, "data-value", value_text) ||
                changed;
            changed =
                detail::set_attribute_on_element(impl, combo, "aria-valuenow", value_text) ||
                changed;
            const double fill_min =
                detail::element_attr_double(elem, "data-fill-min", min);
            const double fill_max =
                detail::element_attr_double(elem, "data-fill-max", max);
            changed = detail::set_attribute_on_element(
                impl, combo, "style",
                "--fill:" + detail::percent_string(
                    detail::normalized_control_value(clamped, fill_min, fill_max))) ||
                changed;
        }
    }
    if (kind == LiveControlKind::AuiKnob ||
        kind == LiveControlKind::DeciusSlider ||
        kind == LiveControlKind::DeciusFader ||
        kind == LiveControlKind::DeciusKnob) {
        changed =
            detail::set_attribute_on_element(impl, elem, "data-value", value_text) ||
            changed;
    }
    // A scrub in flight is a LIVE change; the gesture's end (mouse up in
    // the dispatch layer) emits the single committed change.
    if (value_changed && emit_live_change) {
        detail::emit_widget_change(impl, elem, value_text, /*live=*/true);
    }

    if (kind == LiveControlKind::DeciusSlider) {
        if (auto* fill = detail::first_descendant_with_class(elem, "dcs-slider__fill")) {
            changed = detail::set_attribute_on_element(
                impl, fill, "style",
                decius_slider_fill_style(min, max, clamped, bipolar)) || changed;
        }
        if (auto* thumb = detail::first_descendant_with_class(elem, "dcs-slider__thumb")) {
            changed = detail::set_attribute_on_element(
                impl, thumb, "style",
                decius_slider_thumb_style(min, max, clamped)) || changed;
        }
    } else if (kind == LiveControlKind::DeciusFader) {
        const double fader_pos =
            1.0 - detail::normalized_control_value(clamped, min, max);
        changed = detail::set_attribute_on_element(
            impl, elem, "style",
            style_with_decl(detail::attr_string(elem, "style"), "--pos",
                            detail::percent_string(fader_pos))) || changed;
    } else if (kind == LiveControlKind::AuiKnob ||
               kind == LiveControlKind::DeciusKnob) {
        // Ring/arc/indicator are UA-painted from data-min/max/value +
        // data-bipolar (document.cpp knob chrome), which the data-value
        // write above already updated — a knob move needs NO SVG path
        // string, no indicator --angle, no per-paint reparse. Only the
        // numeric value label lives in the DOM.
        const char* value_class = kind == LiveControlKind::AuiKnob
            ? "aui-knob__value"
            : "dcs-knob__value";
        if (auto* label = detail::first_descendant_with_class(elem, value_class)) {
            changed = detail::set_text_on_element(impl, label, value_text) || changed;
        }
    }

    return changed;
}
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
LiveControlKind live_control_kind_for_block(const Block& block) {
    if (block.tag == "input" && block.input_type == "range") {
        return LiveControlKind::RangeInput;
    }
    if (block.tag == "input" && block.input_type == "number") {
        return LiveControlKind::NumericInput;
    }
    if (detail::block_has_attr(block, "data-dcs-combo")) {
        return LiveControlKind::NumericInput;
    }
    if (detail::block_has_attr(block, "data-aui-knob")) {
        return LiveControlKind::AuiKnob;
    }
    if (detail::block_has_attr(block, "data-dcs-slider")) {
        return LiveControlKind::DeciusSlider;
    }
    if (detail::block_has_attr(block, "data-dcs-fader")) {
        return LiveControlKind::DeciusFader;
    }
    if (detail::block_has_attr(block, "data-dcs-knob")) {
        return LiveControlKind::DeciusKnob;
    }
    return LiveControlKind::None;
}
}  // namespace detail
namespace {

}  // namespace
}  // namespace affineui
