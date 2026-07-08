// document_layout.cpp — part of the AffineUI HTML5 renderer's document implementation.
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

namespace {

#if !defined(AFFINEUI_STUB_BUILD)

}  // namespace (stranded opener above split)
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
lxb_dom_element_t* previous_element_sibling(lxb_dom_node_t* node) {
    for (auto* prev = lxb_dom_node_prev(node); prev;
         prev = lxb_dom_node_prev(prev)) {
        if (prev->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            return lxb_dom_interface_element(prev);
        }
    }
    return nullptr;
}

lxb_dom_element_t* next_element_sibling(lxb_dom_node_t* node) {
    for (auto* next = lxb_dom_node_next(node); next;
         next = lxb_dom_node_next(next)) {
        if (next->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            return lxb_dom_interface_element(next);
        }
    }
    return nullptr;
}

lxb_dom_element_t* parent_element(lxb_dom_element_t* elem) {
    if (!elem) return nullptr;
    for (auto* parent = lxb_dom_node_parent(lxb_dom_interface_node(elem));
         parent; parent = lxb_dom_node_parent(parent)) {
        if (parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            return lxb_dom_interface_element(parent);
        }
    }
    return nullptr;
}
}  // namespace detail
namespace {


bool element_matches_compound(lxb_dom_element_t* elem,
                              const CompoundSelector& compound) {
    if (!elem) return false;
    // Match directly against lexbor's storage. This runs per rule ×
    // element inside detail::collect_blocks (generated-content matching), where
    // the old materialize-everything form (tag/id strings + class token
    // vector + a vector of ALL attributes as string pairs) was the
    // dominant recollect cost in profiles — pure temporary heap churn.
    for (const auto& s : compound.simples) {
        switch (s.kind) {
            case SimpleSelector::Kind::Tag:
                if (detail::tag_view(elem) != s.name) return false;
                break;
            case SimpleSelector::Kind::Id:
                if (detail::attr_view(elem, "id") != s.name) return false;
                break;
            case SimpleSelector::Kind::Class:
                if (!detail::class_tokens_contain(detail::attr_view(elem, "class"),
                                          s.name)) {
                    return false;
                }
                break;
            case SimpleSelector::Kind::Attr:
                if (!detail::has_attr(elem, s.name)) return false;
                if (s.attr_value_set &&
                    detail::attr_view(elem, s.name) != s.value) {
                    return false;
                }
                break;
        }
    }
    return true;
}

bool dom_ancestor_chain_matches(const std::vector<CompoundSelector>& ancestors,
                                lxb_dom_element_t* elem) {
    auto* node = elem;
    for (const auto& compound : ancestors) {
        auto* parent = detail::parent_element(node);
        if (compound.direct_parent) {
            // Child combinator: the immediate parent must match — no skipping.
            if (!parent || !element_matches_compound(parent, compound)) {
                return false;
            }
            node = parent;
            continue;
        }
        while (parent && !element_matches_compound(parent, compound)) {
            parent = detail::parent_element(parent);
        }
        if (!parent) return false;
        node = parent;
    }
    return true;
}

bool generated_rule_matches(const detail::DocumentImpl& impl,
                            const GeneratedContentRule& rule,
                            lxb_dom_element_t* elem,
                            int selector_parent_idx) {
    (void) impl;
    (void) selector_parent_idx;
    if (!element_matches_compound(elem, rule.target)) return false;
    if (!dom_ancestor_chain_matches(rule.ancestors, elem)) return false;
    if (rule.has_previous_adjacent) {
        auto* prev = detail::previous_element_sibling(lxb_dom_interface_node(elem));
        if (!element_matches_compound(prev, rule.previous_adjacent)) {
            return false;
        }
    }
    return true;
}

void append_generated_inline_text(detail::DocumentImpl& impl,
                                  const detail::ResolvedStyle& parent_style,
                                  int parent_idx,
                                  std::string text,
                                  int padding_left,
                                  int padding_right,
                                  bool has_color,
                                  std::uint32_t color_rgba,
                                  GeneratedContentRule::Position position) {
    text = detail::apply_text_transform(std::move(text),
                                parent_style.computed.text_transform);
    if (text.empty()) return;

    const auto id = impl.style_store.acquire_synthetic();
    auto rs = detail::anonymous_text_style(parent_style);
    rs.computed.padding_left = detail::clamp_css_px(padding_left);
    rs.computed.padding_right = detail::clamp_css_px(padding_right);
    if (has_color) rs.animated.color_rgba = color_rgba;

    impl.style_store.computed(id) = rs.computed;
    impl.style_store.animated(id) = rs.animated;
    impl.style_store.dirty(id) &=
        static_cast<std::uint8_t>(~detail::StyleStore::DirtyStyle);

    Block b;
    b.id         = id;
    b.tag        = position == GeneratedContentRule::Position::Before
                     ? "#before"
                     : "#after";
    b.text       = std::move(text);
    b.parent_idx = parent_idx;
    b.generated_color_locked = has_color;
    b.box_shadows = rs.box_shadows;
    b.base_animated = rs.animated;
    b.animation = rs.animation;
    b.animation_epoch = impl.animation_epoch;
    impl.blocks.push_back(std::move(b));
}

void append_generated_box(detail::DocumentImpl& impl,
                          detail::ResolvedStyle rs,
                          int parent_idx,
                          GeneratedContentRule::Position position) {
    const auto id = impl.style_store.acquire_synthetic();
    impl.style_store.computed(id) = rs.computed;
    impl.style_store.animated(id) = rs.animated;
    impl.style_store.dirty(id) &=
        static_cast<std::uint8_t>(~detail::StyleStore::DirtyStyle);

    Block b;
    b.id         = id;
    b.tag        = position == GeneratedContentRule::Position::Before
                     ? "#before"
                     : "#after";
    b.parent_idx = parent_idx;
    b.box_shadows = rs.box_shadows;
    b.base_animated = rs.animated;
    b.animation = rs.animation;
    b.animation_epoch = impl.animation_epoch;
    impl.blocks.push_back(std::move(b));
}

void append_generated_content_for_element(
    detail::DocumentImpl& impl,
    lxb_dom_element_t* elem,
    const detail::ResolvedStyle& elem_style,
    int generated_parent_idx,
    int selector_parent_idx,
    GeneratedContentRule::Position position,
    int& open_synth_idx,
    bool& pending_inline_space) {
    if (!elem || impl.generated_content_rules.empty()) return;

    bool has_content = false;
    bool has_color = false;
    bool has_padding_left = false;
    bool has_padding_right = false;
    std::string content_value;
    std::string display_value;
    std::uint32_t color_rgba = elem_style.animated.color_rgba;
    int padding_left = 0;
    int padding_right = 0;
    auto pseudo_style = impl.resolver
        ? impl.resolver->resolve(nullptr, elem_style)
        : elem_style;

    for (const auto& rule : impl.generated_content_rules) {
        if (rule.position != position) continue;
        if (!generated_rule_matches(impl, rule, elem, selector_parent_idx))
            continue;
        if (rule.decls && impl.resolver) {
            impl.resolver->apply_decl_list(rule.decls, pseudo_style);
        }
        if (!rule.content_value.empty()) {
            has_content = true;
            content_value = rule.content_value;
        }
        if (!rule.display_value.empty()) {
            display_value = rule.display_value;
        }
        if (!rule.color_value.empty()) {
            std::uint32_t parsed = color_rgba;
            if (detail::parse_generated_color(rule.color_value, elem_style, parsed)) {
                has_color = true;
                color_rgba = parsed;
                pseudo_style.animated.color_rgba = parsed;
            }
        }
        if (!rule.background_value.empty()) {
            std::uint32_t parsed = pseudo_style.animated.background_rgba;
            if (detail::parse_generated_color(rule.background_value, elem_style,
                                      parsed)) {
                pseudo_style.animated.background_rgba = parsed;
            }
        }
        if (!rule.background_color_value.empty()) {
            std::uint32_t parsed = pseudo_style.animated.background_rgba;
            if (detail::parse_generated_color(rule.background_color_value, elem_style,
                                      parsed)) {
                pseudo_style.animated.background_rgba = parsed;
            }
        }
        if (!rule.padding_left_value.empty()) {
            has_padding_left = true;
            padding_left =
                detail::parse_generated_length_px(rule.padding_left_value, elem_style);
        }
        if (!rule.padding_right_value.empty()) {
            has_padding_right = true;
            padding_right =
                detail::parse_generated_length_px(rule.padding_right_value, elem_style);
        }
    }

    if (!has_content) return;
    // CSS: `display:none` on the pseudo-element suppresses its box entirely —
    // the cascade winner decides (e.g. the bundle's title-only float rules
    // kill the active-tab accent bars with `--dock-tab:before{display:none}`).
    if (!display_value.empty() &&
        detail::trim_css_ws(detail::substitute_style_vars(display_value, elem_style)) ==
            "none") {
        return;
    }
    if (!detail::generated_content_enabled(content_value, elem_style)) return;
    auto text = detail::generated_content_text(std::move(content_value), elem_style);
    if (text.empty()) {
        append_generated_box(impl, std::move(pseudo_style),
                             generated_parent_idx, position);
        return;
    }

    const int inline_parent_idx =
        detail::ensure_inline_run(impl, elem_style, generated_parent_idx,
                          open_synth_idx);
    if (pending_inline_space) {
        detail::append_anonymous_inline_text(impl, elem_style, inline_parent_idx, " ");
        pending_inline_space = false;
    }
    append_generated_inline_text(
        impl, elem_style, inline_parent_idx, std::move(text),
        has_padding_left ? padding_left : 0,
        has_padding_right ? padding_right : 0,
        has_color, color_rgba, position);
}

}  // namespace

// Forward declaration of a detail:: helper defined later in this TU.
// Placed at namespace scope: `void detail::foo(...)` inside an
// anonymous namespace is ill-formed (MSVC accepts it silently, Clang
// and GCC reject it).
namespace detail {
void apply_pseudo_overlay(DocumentImpl& impl, const Block& block,
                          ResolvedStyle& rs);
}  // namespace detail

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Recursive DFS collector. Walks the DOM, creates one Block per
// block-level element, links it to its parent, and wraps inline text
// runs in synthetic line boxes. Leaf blocks can still own direct text,
// but mixed content such as "text <strong>inline</strong> <button>"
// is represented as anonymous inline text boxes instead of being
// dropped when a block-level child is present.
void collect_blocks(detail::DocumentImpl& impl,
                    lxb_dom_node_t* node,
                    const detail::ResolvedStyle& parent_style,
                    int parent_idx) {
    // Per-recursion-level state: the index of an open synthetic
    // line-box block wrapping a run of inline / inline-block
    // siblings. -1 means we're not currently in such a run; the
    // next inline child opens a fresh line-box, and the next
    // non-inline child resets back to the actual parent.
    int  open_synth_idx = -1;
    bool pending_inline_space = false;
    auto* current_elem = node && node->type == LXB_DOM_NODE_TYPE_ELEMENT
        ? lxb_dom_interface_element(node)
        : nullptr;
    const int current_selector_parent_idx =
        current_elem && parent_idx >= 0
            ? impl.blocks[static_cast<std::size_t>(parent_idx)].parent_idx
            : -1;
    if (current_elem) {
        append_generated_content_for_element(
            impl, current_elem, parent_style, parent_idx,
            current_selector_parent_idx,
            GeneratedContentRule::Position::Before, open_synth_idx,
            pending_inline_space);
    }

    for (auto* child = lxb_dom_node_first_child(node); child;
         child = lxb_dom_node_next(child)) {
        if (detail::node_is_collapsible_whitespace(child)) {
            if (open_synth_idx >= 0) pending_inline_space = true;
            continue;
        }

        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            // Capture source leading/trailing whitespace BEFORE node_text
            // collapse-trims it. Between inline-level boxes CSS keeps one
            // collapsed space â€” so a text node's boundary whitespace is a
            // real inter-inline gap (e.g. "Heading <span class=badge>New"),
            // not something to drop. Only the line-box *edges* trim, which
            // falls out naturally: leading ws at run start (open_synth_idx
            // < 0) is ignored, and a dangling trailing space with no inline
            // sibling after it is never emitted.
            std::size_t rlen = 0;
            lxb_char_t* rraw = lxb_dom_node_text_content(child, &rlen);
            const bool lead_ws  = rraw && rlen && detail::is_html_ws(rraw[0]);
            const bool trail_ws = rraw && rlen && detail::is_html_ws(rraw[rlen - 1]);
            auto text = detail::apply_text_transform(
                detail::node_text(child, parent_style.computed.white_space),
                parent_style.computed.text_transform);
            if (!text.empty()) {
                if (lead_ws && open_synth_idx >= 0) pending_inline_space = true;
                const int inline_parent_idx =
                    detail::ensure_inline_run(impl, parent_style, parent_idx,
                                      open_synth_idx);
                if (pending_inline_space) {
                    detail::append_anonymous_inline_text(impl, parent_style,
                                                 inline_parent_idx, " ");
                    pending_inline_space = false;
                }
                detail::append_anonymous_inline_text(impl, parent_style,
                                             inline_parent_idx,
                                             std::move(text));
                if (trail_ws) pending_inline_space = true;
            }
            continue;
        }

        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* elem = lxb_dom_interface_element(child);
        std::string tag = detail::tag_name(elem);

        if (tag == "head" || tag == "script" || tag == "style" ||
            tag == "meta" || tag == "link"   || tag == "title")
            continue;

        // <option>/<optgroup> are not flow content â€” a <select> renders
        // only its chosen option's text (handled as the select's leaf
        // text below), so don't flatten every option into the box.
        if (tag == "svg")
            continue;

        if (tag == "option" || tag == "optgroup")
            continue;

        if (!detail::is_block_tag(tag)) {
            auto rs_inline = impl.resolver->resolve(elem, parent_style);
            const auto elem_id_attr = detail::attr_string(elem, "id");
            const auto cls_attr = detail::split_classes(detail::attr_string(elem, "class"));
            detail::apply_font_family_fills(impl, tag, elem_id_attr, cls_attr,
                                    parent_idx,
                                    /*state_bits=*/0,
                                    rs_inline);

            using Display = detail::ComputedStyle::Display;
            using CssFloat = detail::ComputedStyle::Float;
            if (rs_inline.computed.display == Display::None) {
                open_synth_idx = -1;
                pending_inline_space = false;
                continue;
            }
            const bool parent_blockifies_inline_children =
                is_flex_container_display(parent_style.computed.display) ||
                parent_style.computed.display == Display::Grid ||
                parent_style.computed.display == Display::InlineGrid;
            const bool flatten_as_inline_text =
                !parent_blockifies_inline_children &&
                rs_inline.computed.css_float == CssFloat::None &&
                rs_inline.computed.display == Display::Inline;

            if (flatten_as_inline_text) {
                append_generated_content_for_element(
                    impl, elem, rs_inline, parent_idx, parent_idx,
                    GeneratedContentRule::Position::Before, open_synth_idx,
                    pending_inline_space);
                auto text = detail::apply_text_transform(
                    detail::node_text(child, rs_inline.computed.white_space),
                    rs_inline.computed.text_transform);
                if (!text.empty()) {
                    const int inline_parent_idx =
                        detail::ensure_inline_run(impl, parent_style, parent_idx,
                                          open_synth_idx);
                    if (pending_inline_space) {
                        detail::append_anonymous_inline_text(impl, parent_style,
                                                     inline_parent_idx, " ");
                        pending_inline_space = false;
                    }
                    detail::append_anonymous_inline_text(impl, rs_inline,
                                                 inline_parent_idx,
                                                 std::move(text));
                }
                append_generated_content_for_element(
                    impl, elem, rs_inline, parent_idx, parent_idx,
                    GeneratedContentRule::Position::After, open_synth_idx,
                    pending_inline_space);
                continue;
            }
        }

        // Resolve this element's style under the parent's resolved
        // style (so inheritance flows correctly down the tree).
        const auto id = impl.style_store.acquire(elem);
        auto rs = impl.resolver->resolve(elem, parent_style);

        // Pseudo-class overlay (:hover, :active) â€” at collect time the
        // bits are preserved from any prior interaction state (they
        // survive reset/acquire). dispatch() re-resolves affected
        // blocks when chains change, so collect-time work is the
        // steady-state path. The block's parent_idx is `parent_idx`
        // (function arg), and impl.blocks already contains everything
        // up to but not including this element â€” exactly what
        // ancestor_chain_matches needs to walk.
        const auto sb_at_collect = impl.style_store.state_bits(id);
        const auto elem_id_attr  = detail::attr_string(elem, "id");
        const auto cls_attr      = detail::split_classes(detail::attr_string(elem, "class"));
        const auto elem_attrs    = detail::element_attrs(elem);

        if (!impl.pseudo_rules.empty()) {
            Block pseudo_block;
            pseudo_block.id = id;
            pseudo_block.tag = tag;
            pseudo_block.elem_id = elem_id_attr;
            pseudo_block.classes = cls_attr;
            pseudo_block.attrs = elem_attrs;
            pseudo_block.parent_idx = parent_idx;
            detail::apply_pseudo_overlay(impl, pseudo_block, rs);
        }

        // Font-family fill overlay. Same selector grammar as pseudo
        // overlay; later rules win (the scan is in attach order, which
        // matches CSS source order).
        std::array<detail::GridTrackHint,
                   detail::kMaxGridTrackHints> grid_columns{};
        std::uint8_t grid_column_count = 0;
        detail::apply_font_family_fills(impl, tag, elem_id_attr, cls_attr,
                                parent_idx, sb_at_collect, rs,
                                &grid_columns, &grid_column_count);
        if (auto value = detail::scan_inline_decl_value(elem, "grid-template-columns");
            !value.empty()) {
            grid_column_count = detail::parse_grid_template_columns(
                std::move(value), grid_columns);
        }
        if (tag == "textarea") {
            detail::apply_user_textarea_size(impl, elem, rs);
        }

        // CSS display:none removes the element and its subtree from layout
        // and paint — but NOT from collection. Hidden subtrees keep their
        // Blocks (Yoga gets YGDisplayNone so they contribute no geometry;
        // paint suppresses the whole subtree) so that toggling `hidden` /
        // display is a pure restyle of retained boxes in BOTH directions.
        // When collection skipped hidden subtrees, every menu/popover reveal
        // was a full-document box rebuild — and that rebuild dropped every
        // OTHER hidden subtree's boxes, so the next reveal rebuilt again:
        // menubar hover-switches cost 3 rebuilds (~50ms) per hop, which
        // dropped fast mouse sweeps. A hidden block-level subtree still
        // breaks any open inline run, same as before.
        if (rs.computed.display == detail::ComputedStyle::Display::None) {
            open_synth_idx = -1;
            pending_inline_space = false;
        }

        impl.style_store.computed(id) = rs.computed;
        impl.style_store.animated(id) = rs.animated;

        if (auto kw = detail::scan_inline_keyword(elem, "cursor"); !kw.empty()) {
            impl.style_store.computed(id).cursor = detail::parse_cursor_keyword(kw);
        }
        if (auto kw = detail::scan_inline_keyword(elem, "resize"); !kw.empty()) {
            const auto [resize, has_resize] = detail::parse_resize_keyword(kw);
            if (has_resize) {
                impl.style_store.computed(id).resize = resize;
            }
        }
        impl.style_store.dirty(id) &=
            static_cast<std::uint8_t>(~detail::StyleStore::DirtyStyle);

        // Inline-run wrapping. Consecutive inline / inline-block
        // siblings get a synthetic flex-row-wrap line-box around
        // them so they flow horizontally instead of stacking. A
        // block-level sibling breaks the run; the next inline
        // sibling opens a fresh line-box.
        //
        // Flex-item blockification (CSS Flexbox Â§4): the children of a
        // flex container are flex items, and an inline-level flex item is
        // blockified. So when the parent establishes a flex formatting
        // context we DON'T group inline children into a line-box â€” each
        // becomes a direct block-level flex item. This is what makes an
        // `<a class="nav-link">` inside a `display:flex` navbar lay out as
        // a flex item rather than collapsing into an inline run.
        using Display = detail::ComputedStyle::Display;
        using CssFloat = detail::ComputedStyle::Float;
        const bool parent_is_flex =
            is_flex_container_display(parent_style.computed.display);
        const bool parent_is_grid =
            parent_style.computed.display == Display::Grid ||
            parent_style.computed.display == Display::InlineGrid;
        const bool parent_blockifies_inline_children =
            parent_is_flex || parent_is_grid;
        const bool child_is_float =
            !parent_blockifies_inline_children &&
            rs.computed.css_float != CssFloat::None;
        const bool child_is_inline =
            !parent_blockifies_inline_children &&
            !child_is_float &&
            (rs.computed.display == Display::Inline ||
             rs.computed.display == Display::InlineBlock ||
             rs.computed.display == Display::InlineFlex ||
             rs.computed.display == Display::InlineGrid);
        int effective_parent_idx;
        if (child_is_inline) {
            detail::ensure_inline_run(impl, parent_style, parent_idx, open_synth_idx);
            if (pending_inline_space) {
                detail::append_anonymous_inline_text(impl, parent_style,
                                             open_synth_idx, " ");
                pending_inline_space = false;
            }
            effective_parent_idx = open_synth_idx;
        } else {
            open_synth_idx       = -1;
            pending_inline_space = false;
            effective_parent_idx = parent_idx;
        }

        const int my_idx = static_cast<int>(impl.blocks.size());
        Block b;
        b.id         = id;
        b.tag        = std::move(tag);
        b.elem_id    = elem_id_attr;
        b.classes    = cls_attr;
        b.attrs      = elem_attrs;
        b.custom_props = rs.custom_props;
        b.box_shadows = rs.box_shadows;
        if (b.tag == "img") {
            b.image_src = detail::attr_string(elem, "src");
        }
        if (b.tag == "input") {
            b.input_type  = detail::attr_string(elem, "type");
            b.role_attr   = detail::attr_string(elem, "role");
            b.is_checked  = detail::has_attr(elem, "checked");
            b.is_disabled = detail::has_attr(elem, "disabled");
        }
        if (b.tag == "textarea") {
            b.text_control = true;
            b.is_disabled = detail::has_attr(elem, "disabled");
            b.placeholder = detail::attr_string(elem, "placeholder");
        } else if (b.tag == "input") {
            b.text_control = detail::input_type_accepts_text(b.input_type);
            b.placeholder = detail::attr_string(elem, "placeholder");
        }
        b.parent_idx = effective_parent_idx;
        if ((rs.computed.display == detail::ComputedStyle::Display::Grid ||
             rs.computed.display == detail::ComputedStyle::Display::InlineGrid) &&
            grid_column_count > 0) {
            b.grid_columns = grid_columns;
            b.grid_column_count = grid_column_count;
        }
        b.base_animated = rs.animated;
        b.animation = rs.animation;
        b.animation_epoch = impl.animation_epoch;
        impl.blocks.push_back(std::move(b));

        // SVG child elements live in SVG's own presentation tree, not the
        // HTML box tree. Keep them out of the HTML style/layout resolver and
        // let the SVG paint helper walk the foreign-content subtree directly.
        if (impl.blocks[static_cast<std::size_t>(my_idx)].tag == "svg") {
            continue;
        }

        // Recurse â€” children get my_idx as their parent. Track whether
        // any blocks were appended; if not, this block is a leaf and
        // gets the concatenated descendant text.
        // Textarea child text is the control value, not child layout content.
        const std::size_t before = impl.blocks.size();
        if (impl.blocks[static_cast<std::size_t>(my_idx)].tag != "textarea") {
            detail::collect_blocks(impl, child, rs, my_idx);
        }
        const bool is_leaf =
            impl.blocks[static_cast<std::size_t>(my_idx)].tag == "textarea" ||
            (impl.blocks.size() == before);
        if (is_leaf) {
            auto& leaf = impl.blocks[static_cast<std::size_t>(my_idx)];
            if (leaf.tag == "input") {
                if (leaf.text_control) {
                    auto* elem_node = lxb_dom_interface_node(elem);
                    auto live = impl.live_text_values.find(
                        elem_node);
                    leaf.text_value = live != impl.live_text_values.end()
                        ? live->second
                        : detail::attr_string(elem, "value");
                    auto caret = impl.live_text_carets.find(elem_node);
                    leaf.caret_offset = caret != impl.live_text_carets.end()
                        ? std::min(caret->second, leaf.text_value.size())
                        : leaf.text_value.size();
                    auto selection =
                        impl.live_text_selections.find(elem_node);
                    if (selection != impl.live_text_selections.end()) {
                        leaf.selection_anchor =
                            std::min(selection->second.first,
                                     leaf.text_value.size());
                        leaf.selection_focus =
                            std::min(selection->second.second,
                                     leaf.text_value.size());
                    } else {
                        leaf.selection_anchor = leaf.caret_offset;
                        leaf.selection_focus = leaf.caret_offset;
                    }
                    leaf.text = detail::text_control_display_value(leaf, leaf.text_value);
                    if (leaf.text.empty() && !leaf.placeholder.empty()) {
                        leaf.text = leaf.placeholder;
                        leaf.placeholder_visible = true;
                    }
                }
            } else if (leaf.tag == "select") {
                leaf.text = detail::select_display_text(elem);
            } else if (leaf.tag == "textarea") {
                auto* elem_node = lxb_dom_interface_node(elem);
                auto live = impl.live_text_values.find(
                    elem_node);
                leaf.text_value = live != impl.live_text_values.end()
                    ? live->second
                    : detail::node_text(child, rs.computed.white_space);
                auto caret = impl.live_text_carets.find(elem_node);
                leaf.caret_offset = caret != impl.live_text_carets.end()
                    ? std::min(caret->second, leaf.text_value.size())
                    : leaf.text_value.size();
                auto selection = impl.live_text_selections.find(elem_node);
                if (selection != impl.live_text_selections.end()) {
                    leaf.selection_anchor =
                        std::min(selection->second.first,
                                 leaf.text_value.size());
                    leaf.selection_focus =
                        std::min(selection->second.second,
                                 leaf.text_value.size());
                } else {
                    leaf.selection_anchor = leaf.caret_offset;
                    leaf.selection_focus = leaf.caret_offset;
                }
                leaf.text = leaf.text_value;
                if (leaf.text.empty() && !leaf.placeholder.empty()) {
                    leaf.text = leaf.placeholder;
                    leaf.placeholder_visible = true;
                }
            } else if (leaf.tag != "img") {
                leaf.text = detail::apply_text_transform(
                    detail::node_text(child, rs.computed.white_space),
                    rs.computed.text_transform);
            }
        }
    }

    if (current_elem) {
        append_generated_content_for_element(
            impl, current_elem, parent_style, parent_idx,
            current_selector_parent_idx,
            GeneratedContentRule::Position::After, open_synth_idx,
            pending_inline_space);
    }
}
}  // namespace detail
namespace {


// Resolve table column widths so cells in the same column line up across
// rows, then pin each cell's content width via intrinsic_w_px. Runs on the
// *natural* (first-pass) layout: `natural[i].w` is the width Yoga gave
// block i with cells free to size to content. Returns true if any table
// was found, so the caller re-runs layout with the pinned widths.
//
// Scope: no rowspan/colspan (column index = a cell's position in its row);
// border-collapse is approximated (each cell keeps its own borders). Both
// match what the current corpus needs; widen later if a test requires it.
bool assign_table_column_widths(std::vector<detail::BlockLayoutInput>& inputs,
                                const std::vector<Rect>& natural) {
    using D = detail::ComputedStyle::Display;
    const int n = static_cast<int>(inputs.size());
    bool found = false;

    for (int t = 0; t < n; ++t) {
        if (!inputs[t].style || inputs[t].style->display != D::Table) continue;
        found = true;

        // Rows = TableRow children of the table, plus TableRow children of
        // any TableRowGroup (thead/tbody/tfoot) child of the table.
        std::vector<int> rows;
        for (int c = t + 1; c < n; ++c) {
            const auto* cs = inputs[c].style;
            if (!cs || inputs[c].parent_idx != t) continue;
            if (cs->display == D::TableRow) {
                rows.push_back(c);
            } else if (cs->display == D::TableRowGroup) {
                for (int r = c + 1; r < n; ++r) {
                    if (inputs[r].parent_idx == c && inputs[r].style &&
                        inputs[r].style->display == D::TableRow) {
                        rows.push_back(r);
                    }
                }
            }
        }
        if (rows.empty()) continue;

        // Per-column width = widest natural cell in that column. Collect the
        // cells per row so we can pin them after.
        std::vector<int> colw;
        std::vector<std::vector<int>> row_cells;
        row_cells.reserve(rows.size());
        for (int r : rows) {
            std::vector<int> cells;
            for (int c = r + 1; c < n; ++c) {
                if (inputs[c].parent_idx == r && inputs[c].style &&
                    inputs[c].style->display == D::TableCell) {
                    cells.push_back(c);
                }
            }
            for (std::size_t j = 0; j < cells.size(); ++j) {
                const int w = natural[static_cast<std::size_t>(cells[j])].w;
                if (j >= colw.size()) colw.resize(j + 1, 0);
                if (w > colw[j]) colw[j] = w;
            }
            row_cells.push_back(std::move(cells));
        }
        const int ncols = static_cast<int>(colw.size());
        if (ncols == 0) continue;

        // Scale columns to fill the table's resolved content width
        // (proportional to their natural widths). Using the first-pass
        // laid-out table width (natural[t].w) handles every case
        // uniformly: explicit px, percentage (e.g. Bootstrap's
        // width:100%), and auto (where it equals the natural content sum,
        // so the scale is a no-op).
        const auto& tcs = *inputs[t].style;
        const int table_w = natural[static_cast<std::size_t>(t)].w
                          - tcs.padding_left - tcs.padding_right
                          - tcs.used_border_left() - tcs.used_border_right();
        long long sum = 0;
        for (int w : colw) sum += w;
        if (table_w > 0 && sum > 0) {
            int assigned = 0;
            for (int j = 0; j < ncols; ++j) {
                colw[j] = static_cast<int>(
                    static_cast<long long>(table_w) * colw[j] / sum);
                assigned += colw[j];
            }
            colw[static_cast<std::size_t>(ncols - 1)] += table_w - assigned;
        }

        // Pin each cell so its border box equals the column width and the
        // columns line up. intrinsic_w_px feeds YGNodeStyleSetWidth, whose
        // meaning depends on the cell's box-sizing:
        //   border-box (Bootstrap's `*{box-sizing:border-box}`): the width
        //     IS the border box â€” pin colw[j] directly. (Subtracting
        //     padding+border here is the bug that left every cell ~18px too
        //     narrow, summing to a phantom empty column on the table's right.)
        //   content-box: Yoga adds padding+border back â€” pin the content box.
        using BoxSizing = detail::ComputedStyle::BoxSizing;
        for (const auto& cells : row_cells) {
            for (std::size_t j = 0; j < cells.size(); ++j) {
                const auto& ccs = *inputs[static_cast<std::size_t>(cells[j])].style;
                int w = (ccs.box_sizing == BoxSizing::BorderBox)
                    ? colw[j]
                    : colw[j] - ccs.padding_left - ccs.padding_right
                              - ccs.used_border_left() - ccs.used_border_right();
                if (w < 0) w = 0;
                inputs[static_cast<std::size_t>(cells[j])].intrinsic_w_px = w;
            }
        }
    }
    return found;
}

#endif  // !AFFINEUI_STUB_BUILD

}  // namespace

void Document::layout(int viewport_width, int viewport_height,
                      Painter* measurer) {
#if !defined(AFFINEUI_STUB_BUILD)
    detail::TraceSpan layout_span("layout");
    // §8.2 ensure-layout gate: a geometry consumer is forcing layout while
    // a view batch is still open (P4 — e.g. find_element_rect's hidden
    // relayout reached from a builder measuring mid-build). Settle the
    // batch's recorded work first: laying out the un-settled block tree
    // would walk blocks whose elements the batch already destroyed.
    if (impl_->view_batch_active) detail::settle_view_batch(*impl_);
    detail::debug_validate_attr_lists(*impl_, "layout-entry");
#endif
    // Layout delegates to Yoga via src/layout/yoga_adapter. Text
    // leaves get a Yoga measure callback that calls nvgTextBoxBounds
    // â€” Yoga asks "given width W, what height?" and we return the
    // *actually rendered* wrapped bbox. No metric heuristics; the
    // top/bottom padding ends up symmetric for free because the
    // content area matches what the painter will draw into.
    //
    // Page gutter is driven by body's CSS padding. The UA stylesheet
    // keeps body padding at the browser-compatible zero default;
    // demos or applications that want a gutter author it explicitly.

#if !defined(AFFINEUI_STUB_BUILD)
    // Viewport-dependent cascade: this is the one call site that knows
    // the real CSS viewport. Rebuild the parsed HTML/CSS attachment graph
    // only when the active @media set changes. Ordinary resize ticks still
    // need fresh computed styles for vw/vh/calc(), but they can be collected
    // from the current live DOM; reparsing from impl_->html would lose live
    // attribute/text mutations and makes interactive resizing much heavier.
    if (!impl_->html.empty() &&
        (viewport_width != impl_->media_viewport_width_px ||
         viewport_height != impl_->media_viewport_height_px)) {
        const bool width_changed =
            viewport_width != impl_->media_viewport_width_px;
        const bool height_changed =
            viewport_height != impl_->media_viewport_height_px;
        const bool first_viewport =
            impl_->media_viewport_width_px <= 0 ||
            impl_->media_viewport_height_px <= 0;
        const auto next_media_sig =
            detail::media_match_signature(*impl_, viewport_width);
        const bool media_set_changed =
            first_viewport ||
            next_media_sig != impl_->media_match_signature;
        const auto viewport_dependency =
            impl_->resolver ? impl_->resolver->viewport_dependency()
                            : detail::ViewportDependency{true, true};
        const bool computed_style_viewport_changed =
            first_viewport ||
            (width_changed && viewport_dependency.width) ||
            (height_changed && viewport_dependency.height);
        impl_->media_viewport_width_px = viewport_width;
        impl_->media_viewport_height_px = viewport_height;
        if (media_set_changed) {
            if (first_viewport) {
                detail::attach_matching_media_blocks_for_viewport(*impl_);
            } else {
                // Changing an already-attached media set requires detaching old
                // stylesheet matches. Keep this conservative until the Lexbor
                // stylesheet replacement path is hardened.
                const std::string html = impl_->html;
                set_html(html);
            }
        } else if (computed_style_viewport_changed) {
            impl_->resolver = detail::make_lexbor_resolver(
                impl_->doc, impl_->media_viewport_width_px,
                impl_->media_viewport_height_px);
            detail::recollect_blocks_from_current_dom(*impl_);
            impl_->media_match_signature = next_media_sig;
        } else {
            // A resize can change available layout width without changing any
            // computed CSS values. Keep the existing block/style tree and only
            // rerun Yoga below. This is the hot path for Bootstrap dashboards:
            // media queries stay in the same bucket and their active rules use
            // vh but not vw, so horizontal resize does not need a full cascade.
            if (impl_->resolver) {
                impl_->resolver->set_viewport(impl_->media_viewport_width_px,
                                              impl_->media_viewport_height_px);
            }
            impl_->media_match_signature = next_media_sig;
        }
    }
#endif

#if !defined(AFFINEUI_STUB_BUILD)
    if (measurer != nullptr) {
        impl_->last_measurer = measurer;
        detail::ensure_font_faces_registered(*impl_, *measurer);
    }
#endif

    int pad_l = 0, pad_t = 0, pad_r = 0, pad_b = 0;
#if !defined(AFFINEUI_STUB_BUILD)
    if (impl_->doc && impl_->resolver) {
        if (auto* body = lxb_html_document_body_element(impl_->doc); body) {
            const auto rs = impl_->resolver->resolve(
                lxb_dom_interface_element(lxb_dom_interface_node(body)),
                impl_->root_style);
            pad_l = rs.computed.padding_left;
            pad_t = rs.computed.padding_top;
            pad_r = rs.computed.padding_right;
            pad_b = rs.computed.padding_bottom;
        }
    }
#endif

    std::vector<std::vector<int>> child_indices(impl_->blocks.size());
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        const int parent = impl_->blocks[i].parent_idx;
        if (parent >= 0) child_indices[static_cast<std::size_t>(parent)]
            .push_back(static_cast<int>(i));
    }

    std::vector<float> block_baselines(impl_->blocks.size(), 0.0f);
    if (measurer != nullptr) {
        for (int i = static_cast<int>(impl_->blocks.size()) - 1; i >= 0; --i) {
            const auto& b = impl_->blocks[static_cast<std::size_t>(i)];
            const auto& cs = impl_->style_store.computed(b.id);

            if (!b.text.empty()) {
                const auto font = measurer->resolve_font(
                    impl_->style_store.font_family_of(cs.font_id),
                    cs.font_size_px, cs.font_weight, cs.font_style != 0);
                const auto tm = measurer->text_metrics(font);
                const float css_line_h = std::max(
                    static_cast<float>(cs.font_size_px) *
                        detail::effective_line_height_mult(cs),
                    1.0f);
                if (tm.ascender > 0.0f && tm.line_height > 0.0f) {
                    const float leading = css_line_h - tm.line_height;
                    block_baselines[static_cast<std::size_t>(i)] =
                        static_cast<float>(cs.used_border_top() + cs.padding_top) +
                        leading * 0.5f + tm.ascender;
                } else {
                    block_baselines[static_cast<std::size_t>(i)] =
                        static_cast<float>(cs.used_border_top() + cs.padding_top) +
                        css_line_h;
                }
                continue;
            }

            const auto& kids = child_indices[static_cast<std::size_t>(i)];
            if (b.synthetic) {
                float baseline = 0.0f;
                for (const int child : kids) {
                    baseline = std::max(
                        baseline,
                        block_baselines[static_cast<std::size_t>(child)]);
                }
                block_baselines[static_cast<std::size_t>(i)] = baseline;
                continue;
            }

            using D = detail::ComputedStyle::Display;
            if (cs.display == D::InlineBlock || cs.display == D::Inline ||
                cs.display == D::TableCell) {
                for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                    const float child_baseline =
                        block_baselines[static_cast<std::size_t>(*it)];
                    if (child_baseline > 0.0f) {
                        block_baselines[static_cast<std::size_t>(i)] =
                            static_cast<float>(cs.used_border_top() + cs.padding_top) +
                            child_baseline;
                        break;
                    }
                }
            }
        }
    }

    std::vector<detail::ComputedStyle> layout_styles;
    layout_styles.reserve(impl_->blocks.size());
    for (const auto& b : impl_->blocks) {
        layout_styles.push_back(impl_->style_store.computed(b.id));
    }

    // Browsers size content boxes to the EXACT glyph advance — no slack.
    // We used to add 4px here so NanoVG's wrap pass (float advance sums)
    // couldn't wrap the final glyph of a tightly-sized label, but that
    // inflated every content-sized control and pushed its ink ~2px left of
    // center (menubar buttons measured 34px where Chrome says 30). The
    // wrap robustness now lives where the problem is: draw_text_box and
    // measure_text_box give nvgTextBreakLines a half-pixel epsilon, so a
    // label measured to fit still lays out on one line.
    auto text_advance_slack = [](const Block&) { return 0; };

    detail::collapse_block_flow_vertical_margins(child_indices, impl_->blocks,
                                         layout_styles);

    std::vector<detail::BlockLayoutInput> inputs;
    inputs.reserve(impl_->blocks.size());
    for (std::size_t bi = 0; bi < impl_->blocks.size(); ++bi) {
        auto& b = impl_->blocks[bi];
        const auto& cs = layout_styles[bi];
        detail::BlockLayoutInput in{};
        in.style          = &cs;
        in.parent_idx     = b.parent_idx;
        in.intrinsic_w_px = 0;  // let parent stretch on cross axis
        in.inline_parent  =
            b.parent_idx >= 0 &&
            impl_->blocks[static_cast<std::size_t>(b.parent_idx)].synthetic;
        in.baseline_px    = block_baselines[bi];
        if ((cs.display == detail::ComputedStyle::Display::Grid ||
             cs.display == detail::ComputedStyle::Display::InlineGrid) &&
            b.grid_column_count > 0) {
            in.grid_columns = b.grid_columns;
            in.grid_column_count = b.grid_column_count;
        }

        if (!b.image_src.empty()) {
            if (measurer != nullptr) {
                const auto image = measurer->load_image(b.image_src);
                const auto sz = measurer->image_size(image);
                if (sz.width > 0 && sz.height > 0) {
                    if (cs.width > 0 && cs.height <= 0) {
                        in.intrinsic_h_px =
                            std::max(1, (sz.height * cs.width) / sz.width);
                    } else if (cs.height > 0 && cs.width <= 0) {
                        in.intrinsic_w_px =
                            std::max(1, (sz.width * cs.height) / sz.height);
                    } else if (cs.width <= 0 && cs.height <= 0) {
                        in.intrinsic_w_px = sz.width;
                        in.intrinsic_h_px = sz.height;
                    }
                }
            }
            inputs.push_back(in);
            continue;
        }

        // Container blocks (no direct text â€” wrap child blocks) leave
        // intrinsic_h at 0 so Yoga sizes them from their children's
        // resolved heights + their own padding/border.
        if (b.tag == "textarea" && b.text_control) {
            if (measurer != nullptr) {
                in.font = measurer->resolve_font(
                    impl_->style_store.font_family_of(cs.font_id),
                    cs.font_size_px, cs.font_weight, cs.font_style != 0);
                const int ch =
                    std::max(1, measurer->measure_text(in.font, "0"));
                const int rows = detail::block_attr_int(b, "rows", 2, 1, 1000);
                const int cols = detail::block_attr_int(b, "cols", 20, 1, 1000);
                const int control_w =
                    ch * cols + cs.padding_left + cs.padding_right +
                    cs.used_border_left() + cs.used_border_right();
                const int line_h = std::max(
                    1, static_cast<int>(std::ceil(
                           detail::resolved_line_height_px(
                               cs, measurer->text_metrics(in.font)
                                       .line_height))));
                const int control_h =
                    line_h * rows + cs.padding_top + cs.padding_bottom +
                    cs.used_border_top() + cs.used_border_bottom();
                if (cs.width < 0 && cs.width_pct_x100 < 0) {
                    in.intrinsic_w_px = control_w;
                }
                if (cs.height < 0 && cs.height_pct < 0) {
                    in.intrinsic_h_px = control_h;
                }
            } else {
                const int ch = std::max(1, static_cast<int>(cs.font_size_px) / 2);
                const int cols = detail::block_attr_int(b, "cols", 20, 1, 1000);
                if (cs.width < 0 && cs.width_pct_x100 < 0) {
                    in.intrinsic_w_px =
                        ch * cols + cs.padding_left + cs.padding_right +
                        cs.used_border_left() + cs.used_border_right();
                }
                in.intrinsic_h_px =
                    cs.font_size_px * detail::block_attr_int(b, "rows", 2, 1, 1000) +
                    cs.padding_top + cs.padding_bottom +
                    cs.used_border_top() + cs.used_border_bottom();
            }
            inputs.push_back(in);
            continue;
        }

        if (b.text.empty()) {
            in.intrinsic_h_px = 0;
            inputs.push_back(in);
            continue;
        }

        // Text-bearing leaf. Hand it the text + a font handle; the adapter
        // wires a Yoga measure callback that runs the live painter's
        // nvgTextBoxBounds per constraint width — or, painterless (headless /
        // server-side layout), estimated glyph metrics so text still OCCUPIES
        // space (content-sized boxes must never collapse to zero).
        {
            in.font = measurer != nullptr
                ? measurer->resolve_font(
                      impl_->style_store.font_family_of(cs.font_id),
                      cs.font_size_px, cs.font_weight, cs.font_style != 0)
                : 0;  // font ids need a rasterizer; the adapter estimates
            in.text = b.text;
            in.letter_spacing_px = static_cast<float>(cs.letter_spacing_x100) / 100.0f;
            const auto text_width = [&](std::string_view t) {
                if (measurer != nullptr) {
                    return std::max(1, measurer->measure_text(in.font, t));
                }
                return std::max(
                    1, static_cast<int>(static_cast<float>(t.size()) *
                                        static_cast<float>(cs.font_size_px) *
                                        0.52f));
            };
            if (cs.min_width < 0 &&
                b.parent_idx >= 0 &&
                static_cast<std::size_t>(b.parent_idx) < impl_->blocks.size()) {
                const auto& parent_style =
                    layout_styles[static_cast<std::size_t>(b.parent_idx)];
                if (detail::is_flex_container_display(parent_style.display) &&
                (parent_style.flex_direction ==
                         detail::ComputedStyle::FlexDirection::Row ||
                     parent_style.flex_direction ==
                         detail::ComputedStyle::FlexDirection::RowReverse)) {
                    in.auto_min_w_px =
                        text_width(b.text)
                        + text_advance_slack(b)
                        + cs.padding_left + cs.padding_right
                        + cs.used_border_left() + cs.used_border_right();
                }
            }
            const bool in_synthetic_inline_parent =
                b.parent_idx >= 0 &&
                static_cast<std::size_t>(b.parent_idx) < impl_->blocks.size() &&
                impl_->blocks[static_cast<std::size_t>(b.parent_idx)].synthetic;
            const bool first_in_synthetic_inline_parent =
                in_synthetic_inline_parent &&
                !child_indices[static_cast<std::size_t>(b.parent_idx)].empty() &&
                child_indices[static_cast<std::size_t>(b.parent_idx)].front() ==
                    static_cast<int>(bi);
            if ((!in_synthetic_inline_parent || first_in_synthetic_inline_parent) &&
                !cs.text_indent_is_pct) {
                in.text_indent_px = static_cast<float>(cs.text_indent_value);
            }
            if (b.text_control &&
                b.input_type != "checkbox" && b.input_type != "radio") {
                const int ch = text_width("0");
                in.intrinsic_w_px = ch * 20
                    + cs.padding_left + cs.padding_right
                    + cs.used_border_left() + cs.used_border_right();
            }
            using WS = detail::ComputedStyle::WhiteSpace;
            in.nowrap = (cs.white_space == WS::Nowrap ||
                         cs.white_space == WS::Pre);
            // Leave intrinsic_h_px = 0 — the measure callback supplies
            // the height instead.
        }
        inputs.push_back(in);
    }

    if (measurer != nullptr) {
        std::vector<int> min_content_w(impl_->blocks.size(), 0);
        for (std::size_t ri = impl_->blocks.size(); ri-- > 0; ) {
            const auto& b = impl_->blocks[ri];
            const auto& cs = layout_styles[ri];
            int content_w = 0;

            if (!b.text.empty() && inputs[ri].font != 0) {
                content_w = std::max(
                    content_w,
                    std::max(1, measurer->measure_text(inputs[ri].font, b.text))
                    + text_advance_slack(b));
            }

            const auto& kids = child_indices[ri];
            if (!kids.empty()) {
                int children_w = 0;
                const bool row_flex =
                    detail::is_flex_container_display(cs.display) &&
                    (cs.flex_direction ==
                         detail::ComputedStyle::FlexDirection::Row ||
                     cs.flex_direction ==
                         detail::ComputedStyle::FlexDirection::RowReverse);
                if (row_flex && cs.flex_wrap == detail::ComputedStyle::FlexWrap::NoWrap) {
                    for (const int child : kids) {
                        children_w += min_content_w[static_cast<std::size_t>(child)];
                    }
                    if (kids.size() > 1 && cs.column_gap > 0) {
                        children_w += static_cast<int>(kids.size() - 1) * cs.column_gap;
                    }
                } else {
                    for (const int child : kids) {
                        children_w = std::max(
                            children_w,
                            min_content_w[static_cast<std::size_t>(child)]);
                    }
                }
                content_w = std::max(content_w, children_w);
            }

            if (content_w > 0) {
                min_content_w[ri] =
                    content_w + cs.padding_left + cs.padding_right +
                    cs.used_border_left() + cs.used_border_right();
            }
        }

        for (std::size_t bi = 0; bi < impl_->blocks.size(); ++bi) {
            const auto& cs = layout_styles[bi];
            if (cs.min_width >= 0 || min_content_w[bi] <= 0 ||
                impl_->blocks[bi].parent_idx < 0) {
                continue;
            }
            const auto parent_idx =
                static_cast<std::size_t>(impl_->blocks[bi].parent_idx);
            const auto& parent_style = layout_styles[parent_idx];
            const bool row_flex_parent =
                detail::is_flex_container_display(parent_style.display) &&
                (parent_style.flex_direction ==
                     detail::ComputedStyle::FlexDirection::Row ||
                 parent_style.flex_direction ==
                     detail::ComputedStyle::FlexDirection::RowReverse);
            if (row_flex_parent) {
                inputs[bi].auto_min_w_px =
                    std::max(inputs[bi].auto_min_w_px, min_content_w[bi]);
            }
        }
    }

    std::vector<Rect> out(impl_->blocks.size());
    std::vector<RectF> out_f(impl_->blocks.size());
    // Yoga's root has no per-block padding of its own. We bake body's
    // padding in by shrinking the viewport handed to Yoga and
    // shifting frames back out below. Cleaner future: a real Box
    // tree where body is its own Yoga node.
    const int inner_w = viewport_width - pad_l - pad_r;
    detail::layout_blocks_with_yoga(
        inner_w, viewport_height, inputs, out, measurer, out_f,
        &impl_->root_style.computed);

    // Table column alignment. Yoga lays each row out independently, so
    // cells in column N of different rows wouldn't line up. Using the
    // natural cell widths from the pass above, resolve one width per
    // column (the widest cell, scaled to any explicit table width), pin
    // every cell to it via intrinsic_w_px, and re-run layout so columns
    // align. No-op (returns false) when the document has no tables.
    if (assign_table_column_widths(inputs, out)) {
        detail::layout_blocks_with_yoga(
            inner_w, viewport_height, inputs, out, measurer, out_f,
            &impl_->root_style.computed);
    }

    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        out[i].x += pad_l;
        out[i].y += pad_t;
        out_f[i].x += static_cast<float>(pad_l);
        out_f[i].y += static_cast<float>(pad_t);
        impl_->blocks[i].bounds = out[i];
        impl_->blocks[i].bounds_f = out_f[i];
    }
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        const auto& cs = layout_styles[i];
        if (cs.css_float == detail::ComputedStyle::Float::None ||
            impl_->blocks[i].parent_idx < 0) {
            continue;
        }

        const auto& parent =
            impl_->blocks[static_cast<std::size_t>(impl_->blocks[i].parent_idx)];
        const auto& parent_style =
            layout_styles[static_cast<std::size_t>(impl_->blocks[i].parent_idx)];
        auto& r = out[i];
        if (cs.width > 0) {
            r.w = cs.box_sizing == detail::ComputedStyle::BoxSizing::BorderBox
                ? cs.width
                : cs.width + cs.padding_left + cs.padding_right +
                    cs.used_border_left() + cs.used_border_right();
        }
        if (cs.height > 0) {
            r.h = cs.box_sizing == detail::ComputedStyle::BoxSizing::BorderBox
                ? cs.height
                : cs.height + cs.padding_top + cs.padding_bottom +
                    cs.used_border_top() + cs.used_border_bottom();
        }
        const int parent_left = parent.bounds.x + parent_style.used_border_left() +
                                parent_style.padding_left;
        const int parent_right = parent.bounds.x + parent.bounds.w -
                                 parent_style.used_border_right() -
                                 parent_style.padding_right;
        if (cs.css_float == detail::ComputedStyle::Float::Right) {
            r.x = parent_right - r.w - cs.margin_right;
        } else {
            r.x = parent_left + cs.margin_left;
        }
        r.y = parent.bounds.y + parent_style.used_border_top() +
              parent_style.padding_top + cs.margin_top;
        impl_->blocks[i].bounds = r;
        impl_->blocks[i].bounds_f = to_float(r);
    }

#if !defined(AFFINEUI_STUB_BUILD)
    if (detail::update_dcs_vec_compression(*impl_, child_indices, layout_styles)) {
        layout(viewport_width, viewport_height, measurer);
        return;
    }
    static const bool layout_dump =
        std::getenv("AFFINEUI_LAYOUT_DUMP") != nullptr;
    if (layout_dump) {
        for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
            const auto& b = impl_->blocks[i];
            const auto& cs = layout_styles[i];
            std::string cls;
            for (const auto& c : b.classes) { cls += c; cls += ' '; }
            std::fprintf(stderr,
                         "[blk %2zu p=%2d] <%s> cls='%s' bounds=%d,%d %dx%d "
                         "h=%d minh=%d disp=%d pos=%d flexdir=%d\n",
                         i, b.parent_idx, b.tag.c_str(), cls.c_str(),
                         b.bounds.x, b.bounds.y, b.bounds.w, b.bounds.h,
                         cs.height, cs.min_height,
                         static_cast<int>(cs.display),
                         static_cast<int>(cs.position),
                         static_cast<int>(cs.flex_direction));
        }
    }
#endif

    auto block_is_fixed_position = [&](std::size_t i) {
        return layout_styles[i].position ==
               detail::ComputedStyle::Position::Fixed;
    };
    auto block_is_in_fixed_subtree = [&](std::size_t i) {
        int idx = static_cast<int>(i);
        while (idx >= 0) {
            if (block_is_fixed_position(static_cast<std::size_t>(idx))) {
                return true;
            }
            idx = impl_->blocks[static_cast<std::size_t>(idx)].parent_idx;
        }
        return false;
    };

    int max_bottom = 0;
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        if (block_is_in_fixed_subtree(i)) continue;
        const auto& b = impl_->blocks[i];
        const int bottom = b.bounds.y + b.bounds.h;
        if (bottom > max_bottom) max_bottom = bottom;
    }
    // content_size = max(natural body height, viewport floor). The
    // floor ensures body's background fills the visible window even
    // when natural content is shorter.
    const int natural_h = max_bottom + pad_b;
    impl_->content_size = Size{viewport_width,
                               std::max(natural_h, viewport_height)};

    // Compute per-block content_h = max(descendant bottom edge) - own top.
    // Used by the scroll path: how far the user can scroll before the
    // last descendant clears the visible region. Iterate children in
    // doc order; each parent gets the max bottom of all its
    // descendants (transitive: child's content already reflects its
    // own descendants).
    for (auto& b : impl_->blocks) b.content_h = b.bounds.h;
    for (std::size_t i = impl_->blocks.size(); i-- > 0; ) {
        if (block_is_fixed_position(i)) continue;
        const auto& child = impl_->blocks[i];
        if (child.parent_idx < 0) continue;
        auto& parent = impl_->blocks[static_cast<std::size_t>(child.parent_idx)];
        const int child_bottom_in_parent =
            (child.bounds.y - parent.bounds.y) + child.content_h;
        if (child_bottom_in_parent > parent.content_h)
            parent.content_h = child_bottom_in_parent;
    }
    // Textarea leaves scroll their VALUE text (UA overflow:auto), so their
    // content height is the wrapped text height — they have no child blocks
    // for the pass above to measure.
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        auto& block = impl_->blocks[i];
        if (block.tag != "textarea" || !block.text_control ||
            block.text.empty()) {
            continue;
        }
        const auto& tcs = layout_styles[i];
        const int inner_w = std::max(
            1, block.bounds.w - tcs.used_border_left() -
                   tcs.used_border_right() - tcs.padding_left -
                   tcs.padding_right);
        const float line_mult = detail::effective_line_height_mult(tcs);
        const float letter_px =
            static_cast<float>(tcs.letter_spacing_x100) / 100.0f;
        float text_h = 0.0f;
        if (measurer != nullptr) {
            const auto font = measurer->resolve_font(
                impl_->style_store.font_family_of(tcs.font_id),
                tcs.font_size_px, tcs.font_weight, tcs.font_style != 0);
            text_h = static_cast<float>(
                measurer
                    ->measure_text_box(font, block.text,
                                       static_cast<float>(inner_w), line_mult,
                                       letter_px)
                    .height);
        } else {
            const float fs = static_cast<float>(tcs.font_size_px);
            const float advance = fs * 0.52f + std::max(0.0f, letter_px);
            const float natural =
                static_cast<float>(block.text.size()) * advance;
            const float lines = std::max(
                1.0f, std::ceil(natural / static_cast<float>(inner_w)));
            text_h = lines * fs *
                     (line_mult > 0.0f ? line_mult : detail::kNormalLineHeight);
        }
        block.content_h = std::max(
            block.content_h,
            tcs.used_border_top() + tcs.padding_top +
                static_cast<int>(std::ceil(text_h)) + tcs.padding_bottom +
                tcs.used_border_bottom());
    }
    using Overflow = detail::ComputedStyle::Overflow;
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        auto& block = impl_->blocks[i];
        const auto ov = layout_styles[i].overflow_y;
        if (ov == Overflow::Scroll || ov == Overflow::Auto) {
            const int max_scroll = std::max(0, block.content_h - block.bounds.h);
            block.scroll_y = std::clamp(block.scroll_y, 0, max_scroll);
        } else {
            block.scroll_y = 0;
        }
    }

#if !defined(AFFINEUI_STUB_BUILD)
    if (measurer != nullptr) {
        for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
            auto& b = impl_->blocks[i];
            if (!b.text_control || b.placeholder_visible) continue;
            const auto g = detail::text_control_geometry(
                *impl_, static_cast<int>(i), *measurer);
            detail::ensure_text_layout_entry(
                *impl_, static_cast<int>(i), g, b, *measurer);
        }
    }
#endif

#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->pending_dirty_roots.empty()) {
        for (const int root_idx : impl_->pending_dirty_roots) {
            if (root_idx >= 0 &&
                root_idx < static_cast<int>(impl_->blocks.size())) {
                detail::add_dirty_rect(*impl_, detail::subtree_visual_rect(*impl_, root_idx));
            }
        }
        impl_->pending_dirty_roots.clear();
    }
    detail::dock_trace_state_if_changed(*impl_, "layout");
#endif
}


#if !defined(AFFINEUI_STUB_BUILD)
float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

float solve_cubic_bezier(float x1, float y1, float x2, float y2, float x) {
    auto sample = [](float a1, float a2, float t) {
        const float inv = 1.0f - t;
        return 3.0f * inv * inv * t * a1 +
               3.0f * inv * t * t * a2 +
               t * t * t;
    };
    auto derivative = [](float a1, float a2, float t) {
        const float inv = 1.0f - t;
        return 3.0f * inv * inv * a1 +
               6.0f * inv * t * (a2 - a1) +
               3.0f * t * t * (1.0f - a2);
    };

    float t = clamp01(x);
    for (int i = 0; i < 8; ++i) {
        const float dx = sample(x1, x2, t) - x;
        const float d = derivative(x1, x2, t);
        if (std::fabs(d) < 1e-5f) break;
        t = clamp01(t - dx / d);
    }
    return sample(y1, y2, t);
}

float ease_animation_progress(detail::ResolvedStyle::CssAnimation::Timing timing,
                              float t) {
    t = clamp01(t);
    switch (timing) {
        case detail::ResolvedStyle::CssAnimation::Timing::Linear:
            return t;
        case detail::ResolvedStyle::CssAnimation::Timing::EaseIn:
            return solve_cubic_bezier(0.42f, 0.0f, 1.0f, 1.0f, t);
        case detail::ResolvedStyle::CssAnimation::Timing::EaseOut:
            return solve_cubic_bezier(0.0f, 0.0f, 0.58f, 1.0f, t);
        case detail::ResolvedStyle::CssAnimation::Timing::EaseInOut:
            return solve_cubic_bezier(0.42f, 0.0f, 0.58f, 1.0f, t);
        case detail::ResolvedStyle::CssAnimation::Timing::StepStart:
            return t > 0.0f ? 1.0f : 0.0f;
        case detail::ResolvedStyle::CssAnimation::Timing::StepEnd:
            return t >= 1.0f ? 1.0f : 0.0f;
        case detail::ResolvedStyle::CssAnimation::Timing::Ease:
        default:
            return solve_cubic_bezier(0.25f, 0.1f, 0.25f, 1.0f, t);
    }
}

bool animation_progress_at(const detail::ResolvedStyle::CssAnimation& anim,
                           double elapsed_s,
                           float& out_t,
                           bool& out_applies) {
    out_t = 0.0f;
    out_applies = false;
    if (!anim.active || anim.duration_s <= 0.0f || anim.name_hash == 0) {
        return false;
    }

    const bool paused =
        anim.play_state == detail::ResolvedStyle::CssAnimation::PlayState::Paused;
    const double timeline_s = paused ? 0.0 : elapsed_s;
    const double active_s = timeline_s - static_cast<double>(anim.delay_s);
    const bool fills_back =
        anim.fill_mode == detail::ResolvedStyle::CssAnimation::FillMode::Backwards ||
        anim.fill_mode == detail::ResolvedStyle::CssAnimation::FillMode::Both;
    const bool fills_forward =
        anim.fill_mode == detail::ResolvedStyle::CssAnimation::FillMode::Forwards ||
        anim.fill_mode == detail::ResolvedStyle::CssAnimation::FillMode::Both;
    auto directed = [&](float local, long long iteration) {
        using Direction = detail::ResolvedStyle::CssAnimation::Direction;
        const bool odd = (iteration & 1ll) != 0;
        switch (anim.direction) {
            case Direction::Reverse:
                return 1.0f - local;
            case Direction::Alternate:
                return odd ? 1.0f - local : local;
            case Direction::AlternateReverse:
                return odd ? local : 1.0f - local;
            case Direction::Normal:
            default:
                return local;
        }
    };
    if (active_s < 0.0) {
        if (fills_back) {
            out_applies = true;
            out_t = ease_animation_progress(anim.timing, directed(0.0f, 0));
        }
        return !paused;
    }

    const bool infinite = anim.iteration_count == 0.0f;
    const double total_s = static_cast<double>(anim.duration_s) *
                           static_cast<double>(anim.iteration_count);
    if (!infinite && active_s >= total_s) {
        if (fills_forward) {
            const double count = std::max(0.0, static_cast<double>(anim.iteration_count));
            const double whole = std::floor(count);
            const double frac = count - whole;
            const bool fractional = frac > 1e-6;
            const long long final_iteration = fractional
                ? static_cast<long long>(whole)
                : std::max<long long>(0, static_cast<long long>(whole) - 1);
            const float local = fractional ? static_cast<float>(frac) : 1.0f;
            out_applies = true;
            out_t = ease_animation_progress(anim.timing,
                                            directed(local, final_iteration));
        }
        return false;
    }

    const double duration = static_cast<double>(anim.duration_s);
    double iteration = std::floor(active_s / duration);
    float local = static_cast<float>((active_s - iteration * duration) / duration);
    out_applies = true;
    out_t = ease_animation_progress(
        anim.timing, directed(local, static_cast<long long>(iteration)));
    return !paused;
}

detail::AnimatedStyle animated_at_keyframe(
    detail::DocumentImpl& impl,
    const Block& block,
    const KeyframeStep* step) {
    if (!step || !step->decls || !impl.resolver) return block.base_animated;
    detail::ResolvedStyle rs;
    rs.computed = impl.style_store.computed(block.id);
    rs.animated = block.base_animated;
    impl.resolver->apply_decl_list(step->decls, rs);
    return rs.animated;
}

float lerp_float(float a, float b, float t) {
    return a + (b - a) * t;
}

detail::AnimatedStyle interpolate_animated(detail::AnimatedStyle a,
                                           const detail::AnimatedStyle& b,
                                           float t) {
    a.opacity  = lerp_float(a.opacity,  b.opacity,  t);
    a.tx       = lerp_float(a.tx,       b.tx,       t);
    a.ty       = lerp_float(a.ty,       b.ty,       t);
    a.tx_pct   = lerp_float(a.tx_pct,   b.tx_pct,   t);
    a.ty_pct   = lerp_float(a.ty_pct,   b.ty_pct,   t);
    a.scale_x  = lerp_float(a.scale_x,  b.scale_x,  t);
    a.scale_y  = lerp_float(a.scale_y,  b.scale_y,  t);
    a.rotation = lerp_float(a.rotation, b.rotation, t);
    return a;
}

detail::AnimatedStyle sample_keyframes(detail::DocumentImpl& impl,
                                       const Block& block,
                                       float t) {
    const auto* kf = detail::find_keyframes(impl, block.animation.name_hash);
    if (!kf || kf->steps.empty()) return block.base_animated;

    const KeyframeStep* left = nullptr;
    const KeyframeStep* right = nullptr;
    for (const auto& step : kf->steps) {
        if (step.offset <= t + 1e-6f) left = &step;
        if (right == nullptr && step.offset >= t - 1e-6f) right = &step;
    }

    const float left_offset = left ? left->offset : 0.0f;
    const float right_offset = right ? right->offset : 1.0f;
    const auto left_style = animated_at_keyframe(impl, block, left);
    if (!right || std::abs(right_offset - left_offset) < 1e-6f) {
        return left_style;
    }
    const auto right_style = animated_at_keyframe(impl, block, right);
    const float local = clamp01((t - left_offset) / (right_offset - left_offset));
    return interpolate_animated(left_style, right_style, local);
}

Mat2x3 local_transform_for(const detail::AnimatedStyle& an, const RectF& r) {
    if (an.tx == 0.0f && an.ty == 0.0f &&
        an.tx_pct == 0.0f && an.ty_pct == 0.0f &&
        an.scale_x == 1.0f && an.scale_y == 1.0f &&
        an.rotation == 0.0f) {
        return Mat2x3::identity();
    }
    const float ox = r.x + an.origin_x + r.w * an.origin_x_pct * 0.01f;
    const float oy = r.y + an.origin_y + r.h * an.origin_y_pct * 0.01f;
    const float tx = an.tx + r.w * an.tx_pct * 0.01f;
    const float ty = an.ty + r.h * an.ty_pct * 0.01f;
    return Mat2x3::translate(-ox, -oy)
        .then(Mat2x3::scale(an.scale_x, an.scale_y))
        .then(Mat2x3::rotate(an.rotation))
        .then(Mat2x3::translate(tx, ty))
        .then(Mat2x3::translate(ox, oy));
}

Mat2x3 effective_transform_for(const detail::DocumentImpl& impl, int idx) {
    std::vector<int> chain;
    for (int cur = idx; cur >= 0; ) {
        chain.push_back(cur);
        cur = impl.blocks[static_cast<std::size_t>(cur)].parent_idx;
    }
    Mat2x3 combined = Mat2x3::identity();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto& b = impl.blocks[static_cast<std::size_t>(*it)];
        const int dy = detail::scroll_offset_y_for(impl.blocks, impl.style_store, *it);
        const RectF eff{
            b.bounds_f.x,
            b.bounds_f.y - static_cast<float>(dy),
            b.bounds_f.w,
            b.bounds_f.h,
        };
        combined = local_transform_for(
            impl.style_store.animated(b.id), eff).then(combined);
    }
    return combined;
}
#endif
}  // namespace affineui
