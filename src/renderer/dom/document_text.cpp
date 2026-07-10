// document_text.cpp — part of the AffineUI HTML5 renderer's document implementation.
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

Document::TransientState Document::capture_transient_state() const {
    TransientState state;
#if !defined(AFFINEUI_STUB_BUILD)
    auto collect = [&](lxb_dom_element_t* elem) {
        if (!elem || detail::has_attr(elem, "hidden")) return;
        const bool popover = detail::class_list_contains(elem, "dcs-popover");
        // Submenu cascades are hover-CSS state, not open layers — capturing
        // one would pin it "open" across a reload.
        const bool menu = (detail::class_list_contains(elem, "dcs-menu") &&
                           !detail::class_list_contains(elem, "dcs-menu__sub")) ||
                          detail::class_list_contains(elem, "aui-select__menu");
        if (!popover && !menu) return;
        const std::string id = detail::attr_string(elem, "id");
        if (id.empty()) return;
        TransientState::Layer layer;
        layer.id = id;
        layer.style = detail::attr_string(elem, "style");
        layer.base_style = detail::attr_string(elem, "data-dcs-base-style");
        layer.target_selector = "#" + id;
        layer.popover = popover;
        layer.menu = menu;
        state.open_layers.push_back(std::move(layer));
    };
    detail::walk_dom_elements(detail::document_dom_root(*impl_), collect);
#endif
    return state;
}

void Document::restore_transient_state(const TransientState& state) {
#if !defined(AFFINEUI_STUB_BUILD)
    for (const auto& layer : state.open_layers) {
        auto* elem = detail::find_dom_element_by_id(*impl_, layer.id);
        if (!elem) continue;
        if (layer.popover && !detail::class_list_contains(elem, "dcs-popover")) {
            continue;
        }
        if (layer.menu && !(detail::class_list_contains(elem, "dcs-menu") ||
                            detail::class_list_contains(elem, "aui-select__menu"))) {
            continue;
        }
        bool changed = false;
        changed = detail::remove_attribute_on_element(*impl_, elem, "hidden") || changed;
        if (!layer.style.empty()) {
            changed = detail::set_attribute_on_element(*impl_, elem, "style",
                                               layer.style) ||
                      changed;
        }
        if (!layer.base_style.empty()) {
            changed = detail::set_attribute_on_element(*impl_, elem,
                                               "data-dcs-base-style",
                                               layer.base_style) ||
                      changed;
        }
        if (auto* trigger =
                detail::find_trigger_for_target(*impl_, layer.target_selector)) {
            changed = detail::set_attribute_on_element(*impl_, trigger,
                                               "aria-expanded", "true") ||
                      changed;
        }
        if (layer.popover) {
            if (auto* field =
                    detail::nearest_ancestor_with_class(elem, "dcs-colorfield")) {
                // A re-opened colorfield picker must show the field's CURRENT
                // value, exactly like a fresh caret-open does. The rebuild
                // re-emits the picker's SV/hue cursors at their defaults, so
                // without this sync a picker kept open across a reload (e.g.
                // right after a pick commits) snaps to a zeroed color even
                // though the committed value is safely in the model.
                changed = detail::sync_dcs_colorfield(
                              *impl_, field, detail::current_dcs_colorfield_hsv(field),
                              /*emit=*/false) ||
                          changed;
            }
        }
        if (changed) {
            impl_->content_size = Size{0, 0};
        }
    }
#else
    (void) state;
#endif
}

#if !defined(AFFINEUI_STUB_BUILD)
namespace {

// Generic chain-refresh helper used by both :hover (chain follows the
// pointer) and :active (chain follows the pressed element). `bit`
// selects which state bit to toggle; `current_chain` is the previous
// chain that we'll diff against and overwrite. Returns true on change.
bool refresh_pseudo_chain(detail::DocumentImpl& impl,
                          std::vector<int>& current_chain,
                          int target_idx,
                          std::uint8_t bit,
                          bool* out_needs_recollect = nullptr) {
    auto new_chain = detail::build_hover_chain(impl.blocks, target_idx);
    if (new_chain == current_chain) return false;

    const auto in = [](int x, const std::vector<int>& v) {
        return std::find(v.begin(), v.end(), x) != v.end();
    };
    std::vector<int> changed_roots;
    // Leaving blocks: clear bit + restyle.
    for (int old_idx : current_chain) {
        if (in(old_idx, new_chain)) continue;
        const auto id = impl.blocks[static_cast<std::size_t>(old_idx)].id;
        impl.style_store.state_bits(id) &= static_cast<std::uint8_t>(~bit);
        changed_roots.push_back(old_idx);
    }
    // Entering blocks: set bit + restyle. If the new state reveals a
    // display:none subtree that has no boxes (a `:hover > .sub` submenu),
    // tell the caller to recollect — restyle alone can't create boxes.
    for (int new_idx : new_chain) {
        if (in(new_idx, current_chain)) continue;
        const auto id = impl.blocks[static_cast<std::size_t>(new_idx)].id;
        impl.style_store.state_bits(id) |= bit;
        changed_roots.push_back(new_idx);
        if (out_needs_recollect && !*out_needs_recollect &&
            detail::pseudo_state_reveals_hidden_subtree(impl, new_idx)) {
            *out_needs_recollect = true;
        }
    }
    for (int root_idx : changed_roots) {
        bool covered_by_ancestor = false;
        for (int other_idx : changed_roots) {
            if (other_idx == root_idx) continue;
            if (detail::is_descendant_of_or_self(impl.blocks, root_idx, other_idx)) {
                covered_by_ancestor = true;
                break;
            }
        }
        if (covered_by_ancestor) continue;
        const Rect old_rect = detail::subtree_visual_rect(impl, root_idx);
        const bool needs_layout = detail::restyle_subtree(impl, root_idx);
        detail::mark_live_mutation_dirty(impl, root_idx, old_rect, needs_layout);
    }
    current_chain = std::move(new_chain);
    return true;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool refresh_hover_chain(detail::DocumentImpl& impl,
                         bool* out_needs_recollect) {
    return refresh_pseudo_chain(impl, impl.hovered_chain,
                                impl.hovered_idx, kHoverStateBit,
                                out_needs_recollect);
}

bool refresh_active_chain(detail::DocumentImpl& impl,
                          bool* out_needs_recollect) {
    return refresh_pseudo_chain(impl, impl.active_chain,
                                impl.active_idx, kActiveStateBit,
                                out_needs_recollect);
}

// Move :focus to `target_idx` (use -1 to clear). Toggles the focus
// state bit on the leaving and entering elements, restyles each so
// the :focus overlay takes effect on the next paint, and records the
// new focused element. Unlike :hover / :active, focus is a single
// element rather than a chain â€” there is no inheritance up the
// ancestor list.
bool set_focus(detail::DocumentImpl& impl, int target_idx) {
    if (target_idx == impl.focused_idx) return false;
    const int old_idx = impl.focused_idx;
    impl.focused_idx  = target_idx;
    if (old_idx >= 0 && old_idx < static_cast<int>(impl.blocks.size())) {
        const Rect old_rect = detail::subtree_visual_rect(impl, old_idx);
        const auto id = impl.blocks[static_cast<std::size_t>(old_idx)].id;
        impl.style_store.state_bits(id) &= static_cast<std::uint8_t>(~kFocusStateBit);
        const bool needs_layout = detail::restyle_block(impl, old_idx);
        detail::mark_live_mutation_dirty(impl, old_idx, old_rect, needs_layout);
    }
    if (target_idx >= 0 && target_idx < static_cast<int>(impl.blocks.size())) {
        const Rect old_rect = detail::subtree_visual_rect(impl, target_idx);
        const auto id = impl.blocks[static_cast<std::size_t>(target_idx)].id;
        impl.style_store.state_bits(id) |= kFocusStateBit;
        const bool needs_layout = detail::restyle_block(impl, target_idx);
        detail::mark_live_mutation_dirty(impl, target_idx, old_rect, needs_layout);
    }
    return true;
}

// Walk up from `idx` looking for the nearest focusable element. A tag
// is focusable if it natively accepts keyboard input today â€” buttons,
// inputs, textareas, selects, and <a href>. Returns -1 when no such
// ancestor exists; callers should treat that as "click outside any
// focusable element â†’ clear focus".
int focusable_ancestor(const detail::DocumentImpl& impl, int idx) {
    while (idx >= 0) {
        const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
        const auto& t = b.tag;
        if (t == "button" || t == "input" || t == "textarea" ||
            t == "select") {
            return idx;
        }
        if (t == "a") {
            auto* elem = impl.style_store.element_of(b.id);
            if (elem && !detail::attr_string(elem, "href").empty()) return idx;
        }
        idx = b.parent_idx;
    }
    return -1;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// True iff this block clips its children (overflow is non-visible).
// CSS overflow: hidden | clip | scroll | auto all clip descendant paint.
bool block_clips_overflow(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0) return false;
    const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
    const auto ov = impl.style_store.computed(b.id).overflow_y;
    using O = detail::ComputedStyle::Overflow;
    return ov == O::Hidden || ov == O::Clip
        || ov == O::Scroll || ov == O::Auto;
}
}  // namespace detail
namespace {

int nearest_fixed_ancestor_or_self(const detail::DocumentImpl& impl, int idx) {
    using P = detail::ComputedStyle::Position;
    while (idx >= 0 && idx < static_cast<int>(impl.blocks.size())) {
        const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
        if (impl.style_store.computed(b.id).position == P::Fixed) {
            return idx;
        }
        idx = b.parent_idx;
    }
    return -1;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
int nearest_clip_ancestor_for_block(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return -1;
    const int fixed_idx = nearest_fixed_ancestor_or_self(impl, idx);
    const int fixed_parent =
        fixed_idx >= 0
            ? impl.blocks[static_cast<std::size_t>(fixed_idx)].parent_idx
            : -2;
    int clip_idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx;
    while (clip_idx >= 0) {
        if (fixed_idx >= 0 && clip_idx == fixed_parent) break;
        if (detail::block_clips_overflow(impl, clip_idx)) return clip_idx;
        clip_idx = impl.blocks[static_cast<std::size_t>(clip_idx)].parent_idx;
    }
    return -1;
}

bool clip_rect_for_block(const detail::DocumentImpl& impl, int idx,
                         Rect& out) {
    // CSS clips a box by the INTERSECTION of the padding boxes of ALL
    // overflow-clipping ancestors — not just the nearest one. The nearest-
    // only version made any element carrying its own clip context (a
    // text-overflow:ellipsis label, an input) exempt from its scroll pane's
    // clip: the self-clip rect rides the scroll offset, so scrolled content
    // painted straight over neighbouring panes. position:fixed still escapes
    // every clip below the fixed boundary, same as the nearest-clip rule.
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return false;
    const int fixed_idx = nearest_fixed_ancestor_or_self(impl, idx);
    const int fixed_parent =
        fixed_idx >= 0
            ? impl.blocks[static_cast<std::size_t>(fixed_idx)].parent_idx
            : -2;
    bool any = false;
    Rect acc{};
    for (int a = impl.blocks[static_cast<std::size_t>(idx)].parent_idx;
         a >= 0;
         a = impl.blocks[static_cast<std::size_t>(a)].parent_idx) {
        if (fixed_idx >= 0 && a == fixed_parent) break;
        if (!detail::block_clips_overflow(impl, a)) continue;
        const auto& cb = impl.blocks[static_cast<std::size_t>(a)];
        const auto& ccs = impl.style_store.computed(cb.id);
        const int dy =
            detail::scroll_offset_y_for(impl.blocks, impl.style_store, a);
        const Rect r{
            cb.bounds.x + ccs.used_border_left(),
            cb.bounds.y - dy + ccs.used_border_top(),
            std::max(0, cb.bounds.w - ccs.used_border_left() -
                            ccs.used_border_right()),
            std::max(0, cb.bounds.h - ccs.used_border_top() -
                            ccs.used_border_bottom()),
        };
        if (!any) {
            acc = r;
            any = true;
        } else {
            const int x0 = std::max(acc.x, r.x);
            const int y0 = std::max(acc.y, r.y);
            const int x1 = std::min(acc.x + acc.w, r.x + r.w);
            const int y1 = std::min(acc.y + acc.h, r.y + r.h);
            acc = Rect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
        }
    }
    if (any) out = acc;
    return any;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// True iff this block accepts scroll input on its Y axis.
bool block_is_scrollable_y(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0) return false;
    const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
    const auto ov = impl.style_store.computed(b.id).overflow_y;
    using O = detail::ComputedStyle::Overflow;
    if (ov != O::Scroll && ov != O::Auto) return false;
    return b.content_h > b.bounds.h;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Find the nearest scrollable-Y ancestor (or self) of `idx`. Returns
// -1 when none exists. Used by wheel routing.
int find_scrollable_y_ancestor(const detail::DocumentImpl& impl, int idx) {
    while (idx >= 0) {
        if (detail::block_is_scrollable_y(impl, idx)) return idx;
        idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx;
    }
    return -1;
}

bool focused_text_control(detail::DocumentImpl& impl, Block*& out) {
    out = nullptr;
    const int idx = impl.focused_idx;
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return false;
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (!block.text_control) return false;
    out = &block;
    return true;
}
}  // namespace detail
namespace {

void remove_last_utf8_codepoint(std::string& text) {
    if (text.empty()) return;
    std::size_t pos = text.size() - 1;
    while (pos > 0 &&
           (static_cast<unsigned char>(text[pos]) & 0xC0u) == 0x80u) {
        --pos;
    }
    text.erase(pos);
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::size_t previous_utf8_boundary(std::string_view text, std::size_t pos) {
    pos = std::min(pos, text.size());
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 &&
           (static_cast<unsigned char>(text[pos]) & 0xC0u) == 0x80u) {
        --pos;
    }
    return pos;
}

std::size_t next_utf8_boundary(std::string_view text, std::size_t pos) {
    pos = std::min(pos, text.size());
    if (pos >= text.size()) return text.size();
    ++pos;
    while (pos < text.size() &&
           (static_cast<unsigned char>(text[pos]) & 0xC0u) == 0x80u) {
        ++pos;
    }
    return pos;
}
}  // namespace detail
namespace {

Painter::TextAlign painter_text_align(const detail::ComputedStyle& cs) {
    switch (cs.text_align) {
        case detail::ComputedStyle::TextAlign::Center:
            return Painter::TextAlign::Center;
        case detail::ComputedStyle::TextAlign::Right:
            return Painter::TextAlign::Right;
        case detail::ComputedStyle::TextAlign::Justify:
            return Painter::TextAlign::Justify;
        case detail::ComputedStyle::TextAlign::Left:
        default:
            return Painter::TextAlign::Left;
    }
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
TextControlGeometry text_control_geometry(const detail::DocumentImpl& impl,
                                          int idx,
                                          Painter& painter) {
    const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    const auto& cs = impl.style_store.computed(block.id);
    const int dy = detail::scroll_offset_y_for(impl.blocks, impl.style_store, idx);
    const Rect eff{block.bounds.x, block.bounds.y - dy,
                   block.bounds.w, block.bounds.h};
    const int used_border_top = cs.used_border_top();
    const int used_border_left = cs.used_border_left();
    const int used_border_right = cs.used_border_right();

    TextControlGeometry g{};
    g.font = painter.resolve_font(impl.style_store.font_family_of(cs.font_id),
                                  cs.font_size_px,
                                  cs.font_weight,
                                  cs.font_style != 0);
    g.text_x = eff.x + used_border_left + cs.padding_left;
    g.text_y = eff.y + used_border_top + cs.padding_top;
    if (block.tag == "textarea") {
        // A textarea scrolls its own value (UA overflow:auto): the text
        // origin shifts by the element's own scroll offset. This is THE
        // geometry both paint and caret/selection hit-mapping consume, so the
        // shift lives here — a caret click on a scrolled textarea must map
        // against exactly what is on screen.
        g.text_y -= block.scroll_y;
    }
    g.content_w = static_cast<float>(
        eff.w - used_border_left - used_border_right -
        cs.padding_left - cs.padding_right);
    g.content_w = std::max(1.0f, g.content_w);
    g.letter_spacing_px = static_cast<float>(cs.letter_spacing_x100) / 100.0f;
    g.line_height_mult = detail::effective_line_height_mult(cs);
    g.align = painter_text_align(cs);
    using WS = detail::ComputedStyle::WhiteSpace;
    g.nowrap = cs.white_space == WS::Nowrap || cs.white_space == WS::Pre;

    const int textarea_idx =
        detail::nearest_block_with_tag(impl.blocks, idx, "textarea");
    if (textarea_idx < 0 &&
        block.tag == "input" &&
        block.input_type != "checkbox" &&
        block.input_type != "radio") {
        // Browsers center a single-line input's line box in the content
        // area (the value sits mid-field no matter how the box was sized);
        // top-anchoring reads visibly high whenever the content box is
        // taller than the line box — e.g. a 15.95px inherited line box in
        // an 18px .dcs-colorfield__hex. Negative deltas (line box taller
        // than the field) also match Chrome: the text stays centered and
        // crops both edges.
        const int used_border_bottom = cs.used_border_bottom();
        const float content_h = static_cast<float>(
            eff.h - used_border_top - used_border_bottom -
            cs.padding_top - cs.padding_bottom);
        const float natural_lh =
            painter.text_metrics(g.font).line_height;
        const float css_lh =
            detail::resolved_line_height_px(cs, natural_lh);
        g.text_y += static_cast<int>(
            std::lround((content_h - css_lh) * 0.5f));
    }

    int indent_px = cs.text_indent_value;
    if (cs.text_indent_is_pct) {
        indent_px = static_cast<int>(std::lround(
            g.content_w * static_cast<float>(cs.text_indent_value) /
            10000.0f));
    }
    if (indent_px != 0) {
        g.text_x += indent_px;
        g.content_w = std::max(
            1.0f, g.content_w - static_cast<float>(indent_px));
    }

    if (g.nowrap && (g.align == Painter::TextAlign::Center ||
                     g.align == Painter::TextAlign::Right)) {
        const std::string display =
            block.placeholder_visible
                ? block.text
                : detail::text_control_display_value(block, block.text_value);
        const Size measured = g.letter_spacing_px == 0.0f
            ? Size{painter.measure_text(g.font, display), 0}
            : painter.measure_text_box(g.font, display, 1e6f,
                                       g.line_height_mult,
                                       g.letter_spacing_px);
        const float slack = g.content_w - static_cast<float>(measured.width);
        if (g.align == Painter::TextAlign::Center) {
            g.text_x += static_cast<int>(std::lround(slack * 0.5f));
        } else {
            g.text_x += static_cast<int>(std::lround(slack));
        }
        g.align = Painter::TextAlign::Left;
    }
    return g;
}
}  // namespace detail
namespace {

std::uint64_t text_layout_signature(const detail::DocumentImpl& impl,
                                    int idx,
                                    const TextControlGeometry& g,
                                    const Block& block) {
    std::uint64_t h = 1469598103934665603ull;
    if (const auto* elem = detail::element_for_block(impl, idx)) {
        const auto* node = lxb_dom_interface_node(
            const_cast<lxb_dom_element_t*>(elem));
        hash_mix(h, node);
    } else {
        hash_mix(h, idx);
    }
    hash_mix_string(h, block.tag);
    hash_mix_string(h, block.input_type);
    hash_mix_string(h, block.text_value);
    const auto& cs = impl.style_store.computed(block.id);
    hash_mix(h, g.font);
    // Positions are part of the identity: consumers read entry.text_x/text_y
    // (the caret hit-mapping maps p.y against entry.text_y). Leaving them out
    // let an entry cached at one position be reused after the element moved
    // or its textarea scrolled — clicks then mapped against stale geometry.
    hash_mix(h, g.text_x);
    hash_mix(h, g.text_y);
    hash_mix(h, g.content_w);
    hash_mix(h, g.letter_spacing_px);
    hash_mix(h, g.line_height_mult);
    hash_mix(h, g.align);
    hash_mix(h, g.nowrap);
    hash_mix(h, cs.white_space);
    return h;
}

float measure_text_advance(Painter& painter,
                           std::uint32_t font,
                           std::string_view text,
                           float line_height_mult,
                           float letter_spacing_px) {
    if (text.empty()) return 0.0f;
    if (letter_spacing_px == 0.0f) {
        return static_cast<float>(painter.measure_text(font, text));
    }
    return static_cast<float>(
        painter.measure_text_box(font, text, 1e6f, line_height_mult,
                                 letter_spacing_px).width);
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
TextLayoutEntry& ensure_text_layout_entry(detail::DocumentImpl& impl,
                                          int idx,
                                          const TextControlGeometry& g,
                                          const Block& block,
                                          Painter& painter) {
    const std::uint64_t signature =
        text_layout_signature(impl, idx, g, block);
    lxb_dom_node_t* node = nullptr;
    if (auto* elem = detail::element_for_block(impl, idx)) {
        node = lxb_dom_interface_node(elem);
    }
    if (auto found = impl.text_layout_cache.find(signature);
        found != impl.text_layout_cache.end() &&
        found->second.signature == signature &&
        !found->second.caret_offsets.empty()) {
        if (node) impl.text_layout_signatures[node] = signature;
        return found->second;
    }
    if (impl.text_layout_cache.size() > 256) {
        impl.text_layout_cache.clear();
        impl.text_layout_signatures.clear();
    }
    auto& entry = impl.text_layout_cache[signature];

    entry = TextLayoutEntry{};
    entry.signature = signature;
    entry.text_x = g.text_x;
    entry.text_y = g.text_y;
    entry.content_w = g.content_w;
    entry.align = g.align;
    entry.nowrap = g.nowrap;
    if (node) impl.text_layout_signatures[node] = signature;
    const auto metrics = painter.text_metrics(g.font);
    entry.natural_line_height =
        metrics.line_height > 0.0f
            ? metrics.line_height
            : static_cast<float>(
                  impl.style_store.computed(block.id).font_size_px);
    entry.css_line_height = std::max(
        1.0f, detail::resolved_line_height_px(
                  impl.style_store.computed(block.id),
                  entry.natural_line_height));

    const auto display_segment = [&](std::size_t begin, std::size_t end) {
        begin = std::min(begin, block.text_value.size());
        end = std::min(end, block.text_value.size());
        if (begin > end) std::swap(begin, end);
        return detail::text_control_display_value(
            block,
            std::string_view(block.text_value).substr(begin, end - begin));
    };
    const auto push_caret = [&](std::size_t offset,
                                float x,
                                std::uint16_t caret_line) {
        if (!entry.caret_offsets.empty() &&
            entry.caret_offsets.back() == offset) {
            entry.caret_x.back() = x;
            entry.caret_lines.back() = caret_line;
            return;
        }
        entry.caret_offsets.push_back(offset);
        entry.caret_x.push_back(x);
        entry.caret_lines.push_back(caret_line);
    };

    const auto is_soft_break_space = [&](std::size_t pos) {
        if (pos >= block.text_value.size()) return false;
        const unsigned char ch =
            static_cast<unsigned char>(block.text_value[pos]);
        return ch == ' ' || ch == '\t';
    };

    std::vector<TextVisualLine> visual_lines;
    const auto push_line = [&](std::size_t begin, std::size_t end) {
        begin = std::min(begin, block.text_value.size());
        end = std::min(end, block.text_value.size());
        if (begin > end) std::swap(begin, end);
        visual_lines.push_back({begin, end});
    };

    std::size_t line_start = 0;
    while (line_start <= block.text_value.size()) {
        std::size_t pos = line_start;
        std::size_t last_break_begin = std::numeric_limits<std::size_t>::max();
        std::size_t last_break_end = std::numeric_limits<std::size_t>::max();
        bool consumed_line = false;

        while (pos < block.text_value.size()) {
            const std::size_t next = detail::next_utf8_boundary(block.text_value, pos);
            if (block.text_value[pos] == '\n') {
                push_line(line_start, pos);
                line_start = next;
                consumed_line = true;
                break;
            }

            if (!g.nowrap && line_start < pos) {
                const float next_width = measure_text_advance(
                    painter, g.font, display_segment(line_start, next),
                    g.line_height_mult, g.letter_spacing_px);
                if (next_width > g.content_w) {
                    if (last_break_begin !=
                            std::numeric_limits<std::size_t>::max() &&
                        last_break_begin > line_start) {
                        push_line(line_start, last_break_begin);
                        line_start = last_break_end;
                        while (is_soft_break_space(line_start)) {
                            line_start =
                                detail::next_utf8_boundary(block.text_value,
                                                   line_start);
                        }
                    } else {
                        push_line(line_start, pos);
                        line_start = pos;
                    }
                    consumed_line = true;
                    break;
                }
            }

            if (is_soft_break_space(pos)) {
                last_break_begin = pos;
                last_break_end = next;
            }
            pos = next;
        }

        if (consumed_line) continue;
        push_line(line_start, block.text_value.size());
        break;
    }
    if (visual_lines.empty()) {
        visual_lines.push_back({0, 0});
    }

    for (std::size_t line_i = 0; line_i < visual_lines.size(); ++line_i) {
        const auto& visual = visual_lines[line_i];
        const auto caret_line = static_cast<std::uint16_t>(
            std::min<std::size_t>(line_i,
                                  std::numeric_limits<std::uint16_t>::max()));
        const float line_width = measure_text_advance(
            painter, g.font, display_segment(visual.begin, visual.end),
            g.line_height_mult, g.letter_spacing_px);
        entry.line_widths.push_back(line_width);
        push_caret(visual.begin, 0.0f, caret_line);
        for (std::size_t pos = visual.begin; pos < visual.end;) {
            const std::size_t next = detail::next_utf8_boundary(block.text_value, pos);
            push_caret(next,
                       measure_text_advance(
                           painter, g.font,
                           display_segment(visual.begin, next),
                           g.line_height_mult, g.letter_spacing_px),
                       caret_line);
            pos = next;
        }
    }
    return entry;
}
}  // namespace detail
namespace {

const TextLayoutEntry* cached_text_layout_entry(const detail::DocumentImpl& impl,
                                                int idx) {
    if (auto* elem = detail::element_for_block(impl, idx)) {
        auto* node = lxb_dom_interface_node(elem);
        if (auto sig = impl.text_layout_signatures.find(node);
            sig != impl.text_layout_signatures.end()) {
            if (auto found = impl.text_layout_cache.find(sig->second);
                found != impl.text_layout_cache.end() &&
                found->second.signature == sig->second &&
                !found->second.caret_offsets.empty()) {
                return &found->second;
            }
        }
    }
    return nullptr;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
float aligned_line_origin_x(const TextControlGeometry& g,
                            const TextLayoutEntry& entry,
                            std::uint16_t line) {
    float x = static_cast<float>(g.text_x);
    const float line_w = line < entry.line_widths.size()
        ? entry.line_widths[static_cast<std::size_t>(line)]
        : 0.0f;
    if (g.align == Painter::TextAlign::Right) {
        x += g.content_w - line_w;
    } else if (g.align == Painter::TextAlign::Center) {
        x += (g.content_w - line_w) * 0.5f;
    }
    return x;
}

float aligned_line_origin_x(const TextLayoutEntry& entry,
                            std::uint16_t line) {
    float x = static_cast<float>(entry.text_x);
    const float line_w = line < entry.line_widths.size()
        ? entry.line_widths[static_cast<std::size_t>(line)]
        : 0.0f;
    if (entry.align == Painter::TextAlign::Right) {
        x += entry.content_w - line_w;
    } else if (entry.align == Painter::TextAlign::Center) {
        x += (entry.content_w - line_w) * 0.5f;
    }
    return x;
}
}  // namespace detail
namespace {

std::size_t text_caret_offset_from_point(detail::DocumentImpl& impl,
                                         int idx,
                                         Point p) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return 0;
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (!block.text_control || block.text_value.empty()) return 0;
    const TextLayoutEntry* entry = cached_text_layout_entry(impl, idx);
    if (entry == nullptr && impl.last_measurer != nullptr) {
        const auto g = detail::text_control_geometry(impl, idx, *impl.last_measurer);
        entry = &detail::ensure_text_layout_entry(
            impl, idx, g, block, *impl.last_measurer);
    }
    if (entry == nullptr) {
        const auto& cs = impl.style_store.computed(block.id);
        const int content_x = block.bounds.x + cs.used_border_left() +
                              cs.padding_left;
        const int content_w = std::max(
            1,
            block.bounds.w - cs.used_border_left() - cs.used_border_right() -
                cs.padding_left - cs.padding_right);
        const double ratio = std::clamp(
            static_cast<double>(p.x - content_x) /
                static_cast<double>(content_w),
            0.0, 1.0);
        std::size_t pos = static_cast<std::size_t>(std::llround(
            ratio * static_cast<double>(block.text_value.size())));
        while (pos > 0 && pos < block.text_value.size() &&
               (static_cast<unsigned char>(block.text_value[pos]) & 0xC0u) ==
                   0x80u) {
            ++pos;
        }
        return std::min(pos, block.text_value.size());
    }

    const int line_count = std::max<int>(
        1, static_cast<int>(entry->line_widths.size()));
    int target_line = static_cast<int>(
        std::floor((static_cast<float>(p.y - entry->text_y)) /
                   entry->css_line_height));
    target_line = std::clamp(target_line, 0, line_count - 1);
    const std::uint16_t line = static_cast<std::uint16_t>(target_line);
    const float origin_x = detail::aligned_line_origin_x(*entry, line);
    const float local_x = static_cast<float>(p.x) - origin_x;

    std::size_t best = 0;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < entry->caret_offsets.size(); ++i) {
        if (entry->caret_lines[i] != target_line) continue;
        const float distance = std::abs(entry->caret_x[i] - local_x);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return entry->caret_offsets[best];
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::pair<std::size_t, std::size_t>
normalized_selection(const Block& block) {
    const auto a = std::min(block.selection_anchor, block.text_value.size());
    const auto b = std::min(block.selection_focus, block.text_value.size());
    return {std::min(a, b), std::max(a, b)};
}

bool has_text_selection(const Block& block) {
    const auto [begin, end] = detail::normalized_selection(block);
    return begin != end;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void set_text_selection(detail::DocumentImpl& impl,
                        int idx,
                        Block& block,
                        std::size_t anchor,
                        std::size_t focus) {
    anchor = std::min(anchor, block.text_value.size());
    focus = std::min(focus, block.text_value.size());
    block.selection_anchor = anchor;
    block.selection_focus = focus;
    block.caret_offset = focus;
    if (auto* elem = detail::element_for_block(impl, idx)) {
        auto* node = lxb_dom_interface_node(elem);
        impl.live_text_carets[node] = focus;
        impl.live_text_selections[node] = {anchor, focus};
    }
}

bool set_text_caret_from_point(detail::DocumentImpl& impl,
                               int idx,
                               Point p,
                               bool extend_selection,
                               std::size_t anchor) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return false;
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (!block.text_control) return false;
    const std::size_t next = text_caret_offset_from_point(impl, idx, p);
    const std::size_t next_anchor =
        extend_selection ? std::min(anchor, block.text_value.size()) : next;
    if (next == block.caret_offset &&
        next_anchor == block.selection_anchor &&
        next == block.selection_focus) {
        return false;
    }
    detail::set_text_selection(impl, idx, block, next_anchor, next);
    detail::add_dirty_rect(impl, detail::block_visual_rect(impl, idx));
    return true;
}

std::pair<std::size_t, std::size_t>
word_bounds_at(std::string_view text, std::size_t caret) {
    caret = std::min(caret, text.size());
    const auto is_word = [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    };
    std::size_t begin = caret;
    if (begin == text.size() && begin > 0) begin = detail::previous_utf8_boundary(text, begin);
    while (begin > 0) {
        const std::size_t prev = detail::previous_utf8_boundary(text, begin);
        if (prev >= text.size() || !is_word(static_cast<unsigned char>(text[prev]))) {
            break;
        }
        begin = prev;
    }
    std::size_t end = caret;
    while (end < text.size()) {
        if (!is_word(static_cast<unsigned char>(text[end]))) break;
        end = detail::next_utf8_boundary(text, end);
    }
    if (begin == end && caret < text.size()) {
        begin = caret;
        end = detail::next_utf8_boundary(text, caret);
    }
    return {begin, end};
}
}  // namespace detail
namespace {

std::string erase_selected_text(std::string text,
                                std::size_t begin,
                                std::size_t end,
                                std::size_t& caret) {
    begin = std::min(begin, text.size());
    end = std::min(end, text.size());
    if (begin > end) std::swap(begin, end);
    text.erase(begin, end - begin);
    caret = begin;
    return text;
}

std::string replace_selected_text(std::string text,
                                  std::size_t begin,
                                  std::size_t end,
                                  std::string_view insert,
                                  std::size_t& caret) {
    begin = std::min(begin, text.size());
    end = std::min(end, text.size());
    if (begin > end) std::swap(begin, end);
    text.replace(begin, end - begin, insert);
    caret = begin + insert.size();
    return text;
}

std::string erase_previous_codepoint(std::string text, std::size_t& caret) {
    caret = std::min(caret, text.size());
    if (caret == 0) return text;
    const std::size_t start = detail::previous_utf8_boundary(text, caret);
    text.erase(start, caret - start);
    caret = start;
    return text;
}

std::string erase_next_codepoint(std::string text, std::size_t& caret) {
    caret = std::min(caret, text.size());
    if (caret >= text.size()) return text;
    const std::size_t end = detail::next_utf8_boundary(text, caret);
    text.erase(caret, end - caret);
    return text;
}

std::string insert_text_at_caret(std::string text,
                                 std::size_t& caret,
                                 std::string_view insert) {
    caret = std::min(caret, text.size());
    text.insert(caret, insert);
    caret += insert.size();
    return text;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void set_live_text_value(detail::DocumentImpl& impl,
                         int idx,
                         Block& block,
                         std::string value) {
    const std::size_t caret = value.size();
    detail::set_live_text_state(impl, idx, block, std::move(value), caret);
}

void set_live_text_state(detail::DocumentImpl& impl,
                         int idx,
                         Block& block,
                         std::string value,
                         std::size_t caret) {
    block.text_value = std::move(value);
    block.caret_offset = std::min(caret, block.text_value.size());
    block.selection_anchor = block.caret_offset;
    block.selection_focus = block.caret_offset;
    block.placeholder_visible = false;
    block.text = detail::text_control_display_value(block, block.text_value);
    if (block.text.empty() && !block.placeholder.empty()) {
        block.text = block.placeholder;
        block.placeholder_visible = true;
    }

    if (auto* elem = detail::element_for_block(impl, idx)) {
        auto* node = lxb_dom_interface_node(elem);
        impl.text_layout_signatures.erase(node);
        impl.live_text_values[node] = block.text_value;
        impl.live_text_carets[node] = block.caret_offset;
        impl.live_text_selections[node] = {
            block.selection_anchor, block.selection_focus};
    }
}
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::string emitted_text_control_value(const Block& block) {
    return block.placeholder_visible ? std::string{} : block.text_value;
}

bool command_modifier(const Event& ev) {
    return ev.ctrl || ev.super;
}
}  // namespace detail
namespace {

bool text_word_byte(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) return false;
    const unsigned char c = static_cast<unsigned char>(text[pos]);
    return c >= 0x80u || std::isalnum(c) || c == '_';
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::size_t previous_word_boundary(std::string_view text, std::size_t pos) {
    pos = std::min(pos, text.size());
    while (pos > 0) {
        const std::size_t prev = detail::previous_utf8_boundary(text, pos);
        if (text_word_byte(text, prev)) break;
        pos = prev;
    }
    while (pos > 0) {
        const std::size_t prev = detail::previous_utf8_boundary(text, pos);
        if (!text_word_byte(text, prev)) break;
        pos = prev;
    }
    return pos;
}

std::size_t next_word_boundary(std::string_view text, std::size_t pos) {
    pos = std::min(pos, text.size());
    while (pos < text.size() && text_word_byte(text, pos)) {
        pos = detail::next_utf8_boundary(text, pos);
    }
    while (pos < text.size() && !text_word_byte(text, pos)) {
        pos = detail::next_utf8_boundary(text, pos);
    }
    return pos;
}

std::string selected_text(const Block& block) {
    const auto [begin, end] = detail::normalized_selection(block);
    if (begin == end) return {};
    return detail::emitted_text_control_value(block).substr(begin, end - begin);
}

std::string clipboard_get_text(detail::DocumentImpl& impl) {
    if (impl.clipboard_get) {
        try {
            return impl.clipboard_get();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "AffineUI clipboard get failed: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "AffineUI clipboard get failed\n");
        }
    }
    return impl.fallback_clipboard;
}

void clipboard_set_text(detail::DocumentImpl& impl, std::string_view text) {
    impl.fallback_clipboard.assign(text);
    if (impl.clipboard_set) {
        try {
            impl.clipboard_set(text);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "AffineUI clipboard set failed: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "AffineUI clipboard set failed\n");
        }
    }
}
}  // namespace detail
namespace {

void emit_text_control_change(detail::DocumentImpl& impl, int idx, Block& block) {
    if (auto* elem = detail::element_for_block(impl, idx)) {
        if (detail::class_list_contains(elem, "dcs-colorfield__hex") ||
            detail::class_list_contains(elem, "dcs-colorfield__picker-input")) {
            if (auto* field = detail::nearest_ancestor_with_class(elem,
                                                          "dcs-colorfield")) {
                detail::sync_dcs_colorfield(impl, field,
                                    detail::emitted_text_control_value(block),
                                    /*emit=*/true);
            }
            return;
        }
        detail::emit_widget_change(impl, elem, detail::emitted_text_control_value(block));
    }
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool delete_text_range(detail::DocumentImpl& impl,
                       int idx,
                       Block& block,
                       std::size_t begin,
                       std::size_t end) {
    std::string next = detail::emitted_text_control_value(block);
    std::size_t caret = block.caret_offset;
    const std::string old = next;
    next = erase_selected_text(std::move(next), begin, end, caret);
    if (next == old && caret == block.caret_offset) return false;
    const Rect old_rect = detail::subtree_visual_rect(impl, idx);
    detail::set_live_text_state(impl, idx, block, std::move(next), caret);
    detail::mark_live_mutation_dirty(impl, idx, old_rect, /*needs_layout=*/true);
    emit_text_control_change(impl, idx, block);
    return true;
}

bool replace_text_selection_or_insert(detail::DocumentImpl& impl,
                                      int idx,
                                      Block& block,
                                      std::string_view text) {
    if (text.empty()) return false;
    const Rect old_rect = detail::subtree_visual_rect(impl, idx);
    std::string next = detail::emitted_text_control_value(block);
    std::size_t caret = block.caret_offset;
    if (detail::has_text_selection(block)) {
        const auto [begin, end] = detail::normalized_selection(block);
        next = replace_selected_text(std::move(next), begin, end, text, caret);
    } else {
        next = insert_text_at_caret(std::move(next), caret, text);
    }
    detail::set_live_text_state(impl, idx, block, std::move(next), caret);
    detail::mark_live_mutation_dirty(impl, idx, old_rect, /*needs_layout=*/true);
    emit_text_control_change(impl, idx, block);
    return true;
}

bool move_text_caret(detail::DocumentImpl& impl,
                     int idx,
                     Block& block,
                     std::size_t caret,
                     bool extend_selection) {
    caret = std::min(caret, block.text_value.size());
    const std::size_t anchor = extend_selection
        ? (detail::has_text_selection(block) ? block.selection_anchor
                                     : block.caret_offset)
        : caret;
    if (caret == block.caret_offset &&
        anchor == block.selection_anchor &&
        caret == block.selection_focus) {
        return false;
    }
    detail::set_text_selection(impl, idx, block, anchor, caret);
    detail::add_dirty_rect(impl, detail::block_visual_rect(impl, idx));
    return true;
}

bool apply_deferred_text_focus(detail::DocumentImpl& impl,
                               const detail::DocumentImpl::LiveControlDrag& drag,
                               Point point) {
    if (!drag.defer_text_focus || drag.focus_idx < 0 ||
        drag.focus_idx >= static_cast<int>(impl.blocks.size())) {
        return false;
    }
    // A click that neither dragged (would have scrubbed the value) nor
    // moved focus lands here. Two-stage caret, the numeric-field idiom:
    //   first click (field was NOT already focused) → select the whole
    //     value + caret at the end, so a retype replaces it wholesale;
    //   second click (field already focused) → place the caret at the
    //     click point, for editing a digit in place.
    const bool already_focused = impl.focused_idx == drag.focus_idx;
    bool changed = detail::set_focus(impl, drag.focus_idx);
    auto& block = impl.blocks[static_cast<std::size_t>(drag.focus_idx)];
    if (!already_focused && block.text_control) {
        detail::set_text_selection(impl, drag.focus_idx, block,
                                   /*anchor=*/0,
                                   /*focus=*/block.text_value.size());
        detail::add_dirty_rect(impl,
                               detail::block_visual_rect(impl, drag.focus_idx));
        changed = true;
    } else {
        changed = detail::set_text_caret_from_point(impl, drag.focus_idx,
                                                    point) ||
                  changed;
    }
    return changed;
}
}  // namespace detail
#else  // stub build â€” no DOM, no pseudo / scroll bookkeeping

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool refresh_hover_chain(detail::DocumentImpl&, bool* = nullptr) {
    return false;
}
bool refresh_active_chain(detail::DocumentImpl&, bool* = nullptr) {
    return false;
}
bool set_focus(detail::DocumentImpl&, int)       { return false; }
int  focusable_ancestor(const detail::DocumentImpl&, int) { return -1; }
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
int  find_scrollable_y_ancestor(const detail::DocumentImpl&, int) { return -1; }
bool focused_text_control(detail::DocumentImpl&, Block*&) { return false; }
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool set_text_caret_from_point(detail::DocumentImpl&, int, Point, bool = false,
                               std::size_t = 0) { return false; }
}  // namespace detail
namespace {

void remove_last_utf8_codepoint(std::string&) {}
}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void set_live_text_value(detail::DocumentImpl&, int, Block&, std::string) {}
void set_live_text_state(detail::DocumentImpl&, int, Block&, std::string,
                         std::size_t) {}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::string emitted_text_control_value(const Block&) { return {}; }
}  // namespace detail
#endif  // !AFFINEUI_STUB_BUILD (text-control section)
}  // namespace affineui
