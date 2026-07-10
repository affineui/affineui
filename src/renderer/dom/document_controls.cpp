// document_controls.cpp — part of the AffineUI HTML5 renderer's document implementation.
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
// ── Dock splitter drag (data-dcs-splitter) ──────────────────────────────────
// A `.dcs-splitter` between two flex panes. Grabbing it captures the pair's
// sizes; dragging redistributes their shared budget via inline flex-basis.
// Canonical math from decius.js initSplitter: axis = clientX (vertical split,
// the default) or clientY (data-dcs-splitter="h" / .dcs-splitter--h, a
// horizontal divider in a column dock); minPx = 24; budget = prev+next held
// constant so the rest of the dock never reflows.
bool find_splitter_at(detail::DocumentImpl& impl,
                      int from_idx,
                      Point /*point*/,
                      detail::DocumentImpl::SplitterDrag& out) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem || !detail::has_attr(elem, "data-dcs-splitter")) continue;

        auto* node = lxb_dom_interface_node(elem);
        auto* prev = detail::previous_element_sibling(node);
        auto* next = detail::next_element_sibling(node);
        if (!prev || !next) return false;
        const int prev_idx = detail::block_index_for_exact_element(impl, prev);
        const int next_idx = detail::block_index_for_exact_element(impl, next);
        if (prev_idx < 0 || next_idx < 0) return false;

        const auto& blk = impl.blocks[static_cast<std::size_t>(idx)];
        const bool horizontal =
            detail::block_has_class(blk, "dcs-splitter--h") ||
            detail::attr_string(elem, "data-dcs-splitter") == "h";
        const auto& pb = impl.blocks[static_cast<std::size_t>(prev_idx)];
        const auto& nb = impl.blocks[static_cast<std::size_t>(next_idx)];
        out.block_idx = idx;
        out.prev = prev;
        out.next = next;
        out.horizontal = horizontal;
        out.prev_size = horizontal ? pb.bounds.h : pb.bounds.w;
        out.next_size = horizontal ? nb.bounds.h : nb.bounds.w;
        out.budget = out.prev_size + out.next_size;
        out.min_px = 24;
        // Note which adjacent pane grows (flex:1). The center/document and
        // nested dock containers are emitted flex:1; side leaves are flex:0 0.
        out.prev_grows = impl.style_store.computed(pb.id).flex_grow > 0;
        out.next_grows = impl.style_store.computed(nb.id).flex_grow > 0;
        // Splitter drags are local DOM resizes, like upstream Decius. Asking
        // the app to rebuild the declarative dock tree here re-resolves
        // sibling slots and can move unrelated borders; structural dock and
        // tearoff operations are the ones that request an app layout rebuild.
        out.persist_layout = false;
        return true;
    }
    return false;
}

// Apply the splitter drag at the current pointer position. Sets inline
// flex-basis on prev/next; the style mutation triggers relayout.
bool update_splitter_drag(detail::DocumentImpl& impl, const Event& ev) {
    auto& d = impl.splitter_drag;
    if (d.block_idx < 0 || !d.prev || !d.next) return false;
    const int pos = d.horizontal ? ev.pos.y : ev.pos.x;
    const int delta = pos - d.start_pos;
    const int lo = d.min_px;
    const int hi = std::max(d.min_px, d.budget - d.min_px);
    auto pin = [&](lxb_dom_element_t* el, int basis) {
        return detail::set_attribute_on_element(
            impl, el, "style",
            "flex:0 0 " + std::to_string(basis) + "px;min-width:0;min-height:0");
    };
    // Pin only the FIXED side; leave the flex:1 grower untouched so it keeps
    // absorbing window space. Freezing the grower (the old both-sides write)
    // detached the layout from the far window edge and broke window-resize of
    // the document. Mirrors decius.js, where the splitter resizes the sized
    // pane and the flexible pane reflows.
    if (d.next_grows && !d.prev_grows) {
        // prev is the sized pane; growing/shrinking it moves the boundary and
        // the grower (next) reflows into the remainder.
        return pin(d.prev, std::clamp(d.prev_size + delta, lo, hi));
    }
    if (d.prev_grows && !d.next_grows) {
        // next is the sized pane; dragging right (delta>0) shrinks it.
        return pin(d.next, std::clamp(d.next_size - delta, lo, hi));
    }
    // Two fixed siblings under a growing parent (or, rarely, two growers):
    // split the shared budget between them — the parent still fills the window.
    const int prev_basis = std::clamp(d.prev_size + delta, lo, hi);
    bool changed = pin(d.prev, prev_basis);
    changed = pin(d.next, d.budget - prev_basis) || changed;
    return changed;
}

bool find_live_control_at(detail::DocumentImpl& impl,
                          int from_idx,
                          Point point,
                          detail::DocumentImpl::LiveControlDrag& out) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        bool resize_x = false;
        bool resize_y = false;
        if (detail::point_in_textarea_resize_grip(impl, idx, point,
                                          resize_x, resize_y)) {
            auto* elem = detail::element_for_block(impl, idx);
            if (!elem) continue;
            out.kind = LiveControlKind::TextAreaResize;
            out.elem = elem;
            out.block_idx = idx;
            out.bounds = detail::block_border_visual_rect(impl, idx);
            out.start_w = out.bounds.w;
            out.start_h = out.bounds.h;
            out.resize_x = resize_x;
            out.resize_y = resize_y;
            return true;
        }

        const auto kind = detail::live_control_kind_for_block(block);
        if (kind == LiveControlKind::None || block.is_disabled) continue;

        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        out.kind = kind;
        out.elem = elem;
        out.block_idx = idx;
        out.bounds = detail::block_border_visual_rect(impl, idx);
        auto* combo = kind == LiveControlKind::NumericInput
            ? detail::nearest_ancestor_with_class(elem, "dcs-combo")
            : nullptr;
        if (kind == LiveControlKind::NumericInput && !combo &&
            detail::has_attr(elem, "data-dcs-combo")) {
            combo = elem;
        }
        const bool has_min_attr =
            detail::has_attr(elem, "min") || detail::has_attr(elem, "data-min") ||
            (combo && detail::has_attr(combo, "data-min"));
        const bool has_max_attr =
            detail::has_attr(elem, "max") || detail::has_attr(elem, "data-max") ||
            (combo && detail::has_attr(combo, "data-max"));
        const bool has_fill_min_attr =
            detail::has_attr(elem, "data-fill-min") ||
            (combo && detail::has_attr(combo, "data-fill-min"));
        const bool has_fill_max_attr =
            detail::has_attr(elem, "data-fill-max") ||
            (combo && detail::has_attr(combo, "data-fill-max"));
        out.min = detail::element_attr_double(
            combo, "data-min",
            detail::element_attr_double(elem, "min",
                detail::element_attr_double(elem, "data-min", 0.0)));
        out.max = detail::element_attr_double(
            combo, "data-max",
            detail::element_attr_double(elem, "max",
                detail::element_attr_double(elem, "data-max", 1.0)));
        if (has_fill_min_attr) {
            out.min = detail::element_attr_double(
                combo, "data-fill-min",
                detail::element_attr_double(elem, "data-fill-min", out.min));
        }
        if (has_fill_max_attr) {
            out.max = detail::element_attr_double(
                combo, "data-fill-max",
                detail::element_attr_double(elem, "data-fill-max", out.max));
        }
        out.start_value = detail::element_attr_double(
            elem, "value", detail::element_attr_double(elem, "data-value", out.min));
        if (kind == LiveControlKind::NumericInput) {
            out.bounded = combo != nullptr &&
                (has_min_attr || has_max_attr ||
                 has_fill_min_attr || has_fill_max_attr);
            if (combo != nullptr) {
                const int combo_idx = detail::block_index_for_exact_element(impl, combo);
                if (combo_idx >= 0) {
                    out.bounds = detail::block_border_visual_rect(impl, combo_idx);
                }
            }
            if (!has_min_attr && !has_fill_min_attr) {
                out.min = out.start_value - 100000.0;
            }
            if (!has_max_attr && !has_fill_max_attr) {
                out.max = out.start_value + 100000.0;
            }
            out.step = detail::element_attr_double(
                elem, "step",
                detail::element_attr_double(
                    elem, "data-step",
                    detail::element_attr_double(combo, "data-step", 0.01)));
            if (out.step <= 0.0) out.step = 0.01;
            // data-linear opts a free field out of magnitude-proportional
            // acceleration (rotation degrees: constant step/pixel).
            out.linear = detail::has_attr(elem, "data-linear") ||
                         (combo && detail::has_attr(combo, "data-linear"));
            out.last_x = point.x;
        }
        if (out.max <= out.min) out.max = out.min + 1.0;
        out.bipolar = detail::has_attr(elem, "data-bipolar");
        return true;
    }
    return false;
}
}  // namespace detail
namespace {

bool update_textarea_resize(detail::DocumentImpl& impl,
                            detail::DocumentImpl::LiveControlDrag& drag,
                            const Event& ev) {
    if (!drag.elem || drag.block_idx < 0 ||
        drag.block_idx >= static_cast<int>(impl.blocks.size())) {
        return false;
    }
    auto* node = lxb_dom_interface_node(drag.elem);
    auto& size = impl.user_textarea_sizes[node];
    const int next_w = drag.resize_x
        ? std::max(1, drag.start_w + (ev.pos.x - drag.start_x))
        : (size.width > 0 ? size.width : drag.start_w);
    const int next_h = drag.resize_y
        ? std::max(1, drag.start_h + (ev.pos.y - drag.start_y))
        : (size.height > 0 ? size.height : drag.start_h);
    if (next_w == size.width && next_h == size.height) return false;

    const Rect old_rect = detail::subtree_visual_rect(impl, drag.block_idx);
    size.width = next_w;
    size.height = next_h;
    auto& block = impl.blocks[static_cast<std::size_t>(drag.block_idx)];
    auto& cs = impl.style_store.computed(block.id);
    if (drag.resize_x) {
        cs.width = static_cast<std::int16_t>(std::min(next_w, 32767));
        cs.width_pct_x100 = -1;
    }
    if (drag.resize_y) {
        cs.height = static_cast<std::int16_t>(std::min(next_h, 32767));
        cs.height_pct = -1;
    }
    detail::mark_live_mutation_dirty(impl, drag.block_idx, old_rect,
                             /*needs_layout=*/true);
    return true;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool update_active_live_control(detail::DocumentImpl& impl, const Event& ev) {
    auto& drag = impl.live_drag;
    if (drag.kind == LiveControlKind::None || !drag.elem) return false;

    if (!drag.moved && detail::pointer_moved_past_threshold(ev, drag)) {
        drag.moved = true;
    }
    if (drag.kind == LiveControlKind::NumericInput && !drag.moved) {
        return false;
    }
    if (drag.kind == LiveControlKind::TextAreaResize) {
        if (!drag.moved) return false;
        return update_textarea_resize(impl, drag, ev);
    }

    double value = drag.start_value;
    if (drag.kind == LiveControlKind::RangeInput ||
        drag.kind == LiveControlKind::DeciusSlider) {
        value = detail::value_from_x(drag.bounds, ev.pos.x, drag.min, drag.max);
    } else if (drag.kind == LiveControlKind::NumericInput) {
        if (drag.bounded) {
            value = detail::value_from_x(drag.bounds, ev.pos.x, drag.min, drag.max);
        } else {
            const double mult = ev.shift ? 4.0 : 1.0;
            const double current = detail::element_attr_double(
                drag.elem, "value",
                detail::element_attr_double(drag.elem, "data-value", drag.start_value));
            // Linear fields (rotation) scrub at a constant step/pixel; the
            // default accelerates with magnitude (|value|/100) so large
            // free values are reachable without a mile-long drag.
            const double per_px = drag.linear
                ? drag.step
                : std::max(drag.step, std::abs(current) / 100.0);
            value = current +
                    static_cast<double>(ev.pos.x - drag.last_x) *
                        per_px * mult;
            drag.last_x = ev.pos.x;
        }
    } else if (drag.kind == LiveControlKind::DeciusFader) {
        value = detail::value_from_y(drag.bounds, ev.pos.y, drag.min, drag.max);
    } else if (drag.kind == LiveControlKind::AuiKnob ||
               drag.kind == LiveControlKind::DeciusKnob) {
        value = drag.start_value +
                (static_cast<double>(drag.start_y - ev.pos.y) / 150.0) *
                    (drag.max - drag.min);
    }
    return detail::update_live_control_value(impl, drag.elem, drag.kind, drag.min,
                                     drag.max, value, drag.bipolar);
}
}  // namespace detail
namespace {

bool is_checkbox_like_block(const Block& block) {
    if (block.tag == "input" &&
        (block.input_type == "checkbox" || block.input_type == "radio")) {
        return true;
    }
    if (detail::block_attr_value(block, "data-aui-widget") &&
        *detail::block_attr_value(block, "data-aui-widget") == "checkbox") {
        return true;
    }
    return detail::block_has_class(block, "dcs-check") ||
           detail::block_has_class(block, "dcs-radio") ||
           detail::block_has_class(block, "dcs-switch");
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool find_checkbox_control_at(detail::DocumentImpl& impl,
                              int from_idx,
                              int& out_idx,
                              lxb_dom_element_t*& out_elem) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        if (!is_checkbox_like_block(block) || block.is_disabled) continue;
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        out_idx = idx;
        out_elem = elem;
        return true;
    }
    return false;
}
}  // namespace detail

// Forward declaration of a detail:: helper defined later in this TU.
// Placed at namespace scope: `bool detail::foo(...)` inside an anonymous
// namespace is ill-formed (Clang / GCC reject; MSVC accepts silently).
namespace detail {
bool class_list_contains(lxb_dom_element_t* elem, std::string_view cls);
}  // namespace detail

namespace {

std::string radio_group_name(lxb_dom_element_t* elem,
                             lxb_dom_element_t* input) {
    if (input) {
        auto name = detail::attr_string(input, "name");
        if (!name.empty()) return name;
    }
    auto name = detail::attr_string(elem, "data-dcs-name");
    if (!name.empty()) return name;
    return detail::attr_string(elem, "name");
}

bool radio_peer_matches(lxb_dom_element_t* peer,
                        lxb_dom_element_t* current,
                        std::string_view group_name) {
    if (!peer || peer == current) return false;
    const bool peer_radio = detail::class_list_contains(peer, "dcs-radio") ||
                            (detail::tag_name(peer) == "input" &&
                             detail::attr_string(peer, "type") == "radio");
    if (!peer_radio) return false;
    if (!group_name.empty()) {
        return radio_group_name(peer,
                                detail::tag_name(peer) == "input" ? peer : nullptr) ==
               group_name;
    }
    return detail::parent_element(peer) == detail::parent_element(current);
}

bool uncheck_radio_peers(detail::DocumentImpl& impl,
                         lxb_dom_element_t* current,
                         std::string_view group_name) {
    auto* root = detail::parent_element(current);
    if (!root) return false;
    bool changed = false;
    auto walk = [&](auto& self, lxb_dom_element_t* elem) -> void {
        if (!elem) return;
        if (radio_peer_matches(elem, current, group_name)) {
            if (detail::tag_name(elem) == "input") {
                changed = detail::remove_attribute_on_element(
                    impl, elem, "checked") || changed;
            } else {
                changed = detail::set_attribute_on_element(
                    impl, elem, "aria-checked", "false") || changed;
            }
        }
        for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
             child != nullptr; child = lxb_dom_node_next(child)) {
            if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            self(self, lxb_dom_interface_element(child));
        }
    };
    walk(walk, root);
    return changed;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool toggle_checkbox_control(detail::DocumentImpl& impl, int idx,
                             lxb_dom_element_t* elem) {
    if (!elem) return false;
    const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    lxb_dom_element_t* input =
        block.tag == "input" ? elem : detail::first_descendant_input(elem);
    lxb_dom_element_t* decius_control = nullptr;
    if (detail::class_list_contains(elem, "dcs-check") ||
        detail::class_list_contains(elem, "dcs-radio") ||
        detail::class_list_contains(elem, "dcs-switch")) {
        decius_control = elem;
    } else {
        decius_control = detail::first_descendant_with_class(elem, "dcs-check");
        if (!decius_control) {
            decius_control = detail::first_descendant_with_class(elem, "dcs-radio");
        }
        if (!decius_control) {
            decius_control = detail::first_descendant_with_class(elem, "dcs-switch");
        }
    }
    lxb_dom_element_t* visual_check =
        detail::first_descendant_with_class(elem, "dcs-check__box");
    if (!visual_check) {
        visual_check = detail::block_has_class(block, "dcs-check") ||
                       detail::block_has_class(block, "dcs-radio")
            ? elem
            : detail::first_descendant_with_class(elem, "dcs-check");
    }
    const bool radio = (input && detail::attr_string(input, "type") == "radio") ||
                       detail::block_has_class(block, "dcs-radio") ||
                       detail::class_list_contains(elem, "dcs-radio") ||
                       (decius_control != nullptr &&
                        detail::class_list_contains(decius_control, "dcs-radio"));
    const bool old_checked = input
        ? detail::has_attr(input, "checked")
        : (decius_control
               ? detail::element_attr_true(decius_control, "aria-checked")
               : detail::element_attr_true(elem, "aria-checked"));
    const bool checked = radio ? true : !old_checked;

    bool changed = false;
    if (radio && checked) {
        changed = uncheck_radio_peers(
            impl, elem, radio_group_name(elem, input)) || changed;
    }
    if (input) {
        changed = checked
            ? (detail::set_attribute_on_element(impl, input, "checked", "checked") || changed)
            : (detail::remove_attribute_on_element(impl, input, "checked") || changed);
    }
    if (input != elem || detail::has_attr(elem, "aria-checked") ||
        detail::block_has_class(block, "dcs-check") ||
        detail::block_has_class(block, "dcs-radio") ||
        detail::block_has_class(block, "dcs-switch") ||
        detail::block_attr_value(block, "data-aui-widget")) {
        changed = detail::set_attribute_on_element(
            impl, elem, "aria-checked", checked ? "true" : "false") ||
            changed;
    }
    if (decius_control != nullptr && decius_control != elem) {
        changed = detail::set_attribute_on_element(
            impl, decius_control, "aria-checked",
            checked ? "true" : "false") || changed;
    }
    if (visual_check != nullptr && visual_check != elem) {
        changed = detail::set_attribute_on_element(
            impl, visual_check, "aria-checked",
            checked ? "true" : "false") || changed;
    }
    auto* wrapper = detail::nearest_checkbox_wrapper(elem);
    if (wrapper != nullptr && wrapper != elem &&
        detail::attr_string(wrapper, "data-aui-widget") == "checkbox") {
        changed = checked
            ? (detail::set_attribute_on_element(impl, wrapper, "aria-checked", "true") || changed)
            : (detail::remove_attribute_on_element(impl, wrapper, "aria-checked") || changed);
    }
    if (changed) {
        detail::emit_widget_change(impl, wrapper, checked ? "true" : "false");
    }
    return changed;
}
}  // namespace detail
namespace {

bool is_button_like_block(const Block& block) {
    if (block.tag == "button") return true;
    if (const auto* role = detail::block_attr_value(block, "role");
        role && *role == "button") {
        return true;
    }
    if (const auto* widget = detail::block_attr_value(block, "data-aui-widget");
        widget && *widget == "button") {
        return true;
    }
    return false;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool find_button_control_at(detail::DocumentImpl& impl,
                            int from_idx,
                            lxb_dom_element_t*& out_elem) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        if (!is_button_like_block(block) || block.is_disabled) continue;
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        out_elem = elem;
        return true;
    }
    return false;
}
}  // namespace detail
namespace {

std::string activation_name(lxb_dom_element_t* elem) {
    if (!elem) return {};
    if (auto name = detail::attr_string(elem, "data-aui-name"); !name.empty()) {
        return name;
    }
    return detail::attr_string(elem, "id");
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool activate_button_control(detail::DocumentImpl& impl,
                             lxb_dom_element_t* elem) {
    auto name = activation_name(elem);
    if (name.empty()) return false;
    impl.activated_widgets.push_back(std::move(name));
    return true;
}

bool find_button_group_option_at(detail::DocumentImpl& impl,
                                 int from_idx,
                                 lxb_dom_element_t*& out_group,
                                 lxb_dom_element_t*& out_option) {
    out_group = nullptr;
    out_option = nullptr;
    lxb_dom_element_t* decius_group = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        if (!out_option && block.tag == "button" && !block.is_disabled &&
            (detail::block_has_attr(block, "value") ||
             detail::block_has_attr(block, "data-dcs-value") ||
             detail::block_has_class(block, "dcs-btn"))) {
            out_option = elem;
        }
        const auto* widget = detail::block_attr_value(block, "data-aui-widget");
        if (widget && *widget == "button-group") {
            out_group = elem;
            return out_option != nullptr;
        }
        if (!decius_group && detail::block_has_class(block, "dcs-btn-group")) {
            decius_group = elem;
        }
    }
    if (decius_group && out_option) {
        // A bare .dcs-btn-group can be either a segmented selector or just
        // a visual grouping for independently-bound buttons. Generated
        // button_group widgets mark the outer field with data-aui-widget;
        // otherwise a named child button should keep its own on_click path.
        if (detail::has_attr(out_option, "data-aui-name") &&
            detail::attr_string(decius_group, "data-aui-widget") != "button-group") {
            out_group = nullptr;
            out_option = nullptr;
            return false;
        }
        out_group = decius_group;
        return true;
    }
    out_group = nullptr;
    out_option = nullptr;
    return false;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool class_list_contains(lxb_dom_element_t* elem, std::string_view cls) {
    return detail::class_tokens_contain(detail::attr_view(elem, "class"), cls);
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::string class_list_set(lxb_dom_element_t* elem,
                           std::string_view cls,
                           bool present) {
    auto classes = detail::split_classes(detail::attr_string(elem, "class"));
    auto it = std::find(classes.begin(), classes.end(), cls);
    if (present && it == classes.end()) {
        classes.emplace_back(cls);
    } else if (!present && it != classes.end()) {
        classes.erase(it);
    }
    std::string out;
    for (const auto& c : classes) {
        if (!out.empty()) out.push_back(' ');
        out += c;
    }
    return out;
}
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Add/remove a class on a live element (re-matches selectors, restyles).
bool set_element_class(detail::DocumentImpl& impl,
                       lxb_dom_element_t* elem,
                       std::string_view cls,
                       bool present) {
    if (!elem) return false;
    if (detail::class_list_contains(elem, cls) == present) return false;
    return detail::set_attribute_on_element(impl, elem, "class",
                                    detail::class_list_set(elem, cls, present));
}
}  // namespace detail
namespace {

// Layout-time responsive class toggle (dcs-vec compression): raw DOM
// write + element-local rematch + scoped restyle ONLY. The caller runs
// INSIDE Document::layout and relayouts when any toggle happens, and the
// initiating mutation already owns paint invalidation — the full attr
// ceremony (subtree rect capture, reveal probe, per-op dirty marking)
// made every splitter-drag threshold crossing a multi-toggle stall.
bool set_layout_class(detail::DocumentImpl& impl,
                      int block_idx,
                      lxb_dom_element_t* elem,
                      std::string_view cls,
                      bool present) {
    if (!elem) return false;
    if (detail::class_list_contains(elem, cls) == present) return false;
    const std::string next = detail::class_list_set(elem, cls, present);
    if (!lxb_dom_element_set_attribute(elem, detail::as_lxb("class"), 5,
                                       detail::as_lxb(next), next.size())) {
        return false;
    }
    if (lxb_dom_interface_node(elem)->ns == LXB_NS_HTML) {
        (void) lxb_html_document_element_styles_rematch(
            lxb_html_interface_element(lxb_dom_interface_node(elem)));
    }
    if (impl.resolver) impl.resolver->invalidate(elem);
    if (block_idx >= 0 &&
        block_idx < static_cast<int>(impl.blocks.size())) {
        // Keep block metadata (classes/attrs feed hit-testing and hover
        // chains) in sync with the raw DOM write.
        detail::refresh_block_metadata_from_element(
            impl.blocks[static_cast<std::size_t>(block_idx)], elem);
        (void) detail::restyle_subtree(impl, block_idx);
    }
    return true;
}

double parse_css_number_prefix(std::string_view value, double fallback) {
    const std::string text(detail::trim_css_ws(value));
    if (text.empty()) return fallback;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(parsed)) return fallback;
    return parsed;
}

double block_custom_number(const Block& block,
                           std::string_view prop,
                           double fallback) {
    if (!block.custom_props) return fallback;
    const auto it = block.custom_props->find(std::string(prop));
    if (it == block.custom_props->end()) return fallback;
    return parse_css_number_prefix(it->second, fallback);
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool update_dcs_vec_compression(
    detail::DocumentImpl& impl,
    const std::vector<std::vector<int>>& child_indices,
    const std::vector<detail::ComputedStyle>& layout_styles) {
    bool changed = false;
    for (std::size_t i = 0; i < impl.blocks.size(); ++i) {
        auto& vec = impl.blocks[i];
        if (!detail::block_has_class(vec, "dcs-vec")) continue;
        if (i >= child_indices.size()) continue;
        const auto& kids = child_indices[i];
        const int child_count = std::max(1, static_cast<int>(kids.size()));
        const double min_width = block_custom_number(
            vec, "--dcs-xform-minwidth", 72.0);
        const double gap =
            i < layout_styles.size() ? layout_styles[i].column_gap : 0;
        const double needed =
            child_count * min_width + (child_count - 1) * gap;

        double available = vec.bounds.w;
        lxb_dom_element_t* field_elem = nullptr;
        if (vec.parent_idx >= 0 &&
            static_cast<std::size_t>(vec.parent_idx) < impl.blocks.size() &&
            detail::block_has_class(impl.blocks[static_cast<std::size_t>(vec.parent_idx)],
                            "dcs-field")) {
            const auto parent_idx = static_cast<std::size_t>(vec.parent_idx);
            const auto& parent = impl.blocks[parent_idx];
            field_elem = detail::element_for_block(impl, vec.parent_idx);
            const auto& parent_style = layout_styles[parent_idx];
            const double field_gap = parent_style.column_gap > 0
                ? parent_style.column_gap
                : parent_style.row_gap;
            double used = 0.0;
            int extras = 0;
            if (parent_idx < child_indices.size()) {
                for (const int child : child_indices[parent_idx]) {
                    if (child == static_cast<int>(i)) continue;
                    if (child < 0 ||
                        child >= static_cast<int>(impl.blocks.size())) {
                        continue;
                    }
                    used += impl.blocks[static_cast<std::size_t>(child)]
                                .bounds.w;
                    ++extras;
                }
            }
            const int field_client =
                parent.bounds.w - parent_style.used_border_left() -
                parent_style.used_border_right();
            available = field_client - used - extras * field_gap;
        }

        static const bool vec_trace =
            std::getenv("AFFINEUI_VEC_TRACE") != nullptr;
        // Hysteresis: stack when too narrow, but only UNSTACK once the
        // row is clearly wide enough (+8px). Without the band, widths at
        // the threshold flip-flop between relayout rounds — every
        // splitter-drag crossing paid multiple toggle+relayout cycles.
        auto* vec_elem = detail::element_for_block(impl, static_cast<int>(i));
        const bool was_stacked =
            vec_elem != nullptr &&
            detail::class_list_contains(vec_elem, "dcs-vec--stacked");
        const bool stacked = was_stacked
                                 ? available + 1.0 < needed + 8.0
                                 : available + 1.0 < needed;
        if (vec_trace) {
            const int pidx = vec.parent_idx;
            std::fprintf(stderr,
                         "[vec %zu] kids=%d min=%.1f gap=%.1f needed=%.1f "
                         "avail=%.1f vec=%dx%d field=%dx%d y=%d -> %s\n",
                         i, child_count, min_width, gap, needed, available,
                         vec.bounds.w, vec.bounds.h,
                         pidx >= 0 ? impl.blocks[static_cast<std::size_t>(pidx)].bounds.w : -1,
                         pidx >= 0 ? impl.blocks[static_cast<std::size_t>(pidx)].bounds.h : -1,
                         vec.bounds.y,
                         stacked ? "STACKED" : "row");
        }
        if (field_elem) {
            changed = set_layout_class(impl, vec.parent_idx, field_elem,
                                       "dcs-field--vec", true) ||
                      changed;
            changed = set_layout_class(impl, vec.parent_idx, field_elem,
                                       "dcs-field--vec-stacked", stacked) ||
                      changed;
        }
        if (vec_elem != nullptr) {
            changed = set_layout_class(impl, static_cast<int>(i), vec_elem,
                                       "dcs-vec--stacked", stacked) ||
                      changed;
        }
        if (vec_trace && field_elem && vec.parent_idx >= 0) {
            const auto& fb = impl.blocks[static_cast<std::size_t>(vec.parent_idx)];
            const auto& fcs = impl.style_store.computed(fb.id);
            std::fprintf(stderr,
                         "[vec %zu] post-toggle field style: h=%d minh=%d "
                         "hpct=%d classes=%s\n",
                         i, fcs.height, fcs.min_height, fcs.height_pct,
                         detail::attr_string(field_elem, "class").c_str());
        }
    }
    return changed;
}
}  // namespace detail
namespace {


int color_byte(double value) {
    return std::clamp(static_cast<int>(std::round(value * 255.0)), 0, 255);
}

std::string hex_from_color(Color color) {
    char buf[8]{};
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                  static_cast<unsigned>(color.r),
                  static_cast<unsigned>(color.g),
                  static_cast<unsigned>(color.b));
    return std::string(buf);
}

std::string normalize_hex_color(std::string_view raw) {
    std::string value(detail::trim_css_ws(raw));
    if (value.empty()) return {};
    if (value.front() != '#' &&
        (value.size() == 3 || value.size() == 4 ||
         value.size() == 6 || value.size() == 8)) {
        value.insert(value.begin(), '#');
    }
    std::uint32_t rgba = 0;
    if (!detail::parse_hex_color(value, rgba)) return {};
    return hex_from_color(detail::unpack_rgba(rgba));
}

HsvColor rgb_to_hsv(Color color) {
    const double r = static_cast<double>(color.r) / 255.0;
    const double g = static_cast<double>(color.g) / 255.0;
    const double b = static_cast<double>(color.b) / 255.0;
    const double max_c = std::max({r, g, b});
    const double min_c = std::min({r, g, b});
    const double d = max_c - min_c;
    double h = 0.0;
    if (d > 0.0) {
        if (max_c == r) h = std::fmod((g - b) / d, 6.0);
        else if (max_c == g) h = (b - r) / d + 2.0;
        else h = (r - g) / d + 4.0;
        h *= 60.0;
        if (h < 0.0) h += 360.0;
    }
    const double s = max_c > 0.0 ? d / max_c : 0.0;
    return {h, s, max_c};
}

Color hsv_to_rgb(HsvColor hsv) {
    hsv.h = std::fmod(hsv.h, 360.0);
    if (hsv.h < 0.0) hsv.h += 360.0;
    hsv.s = std::clamp(hsv.s, 0.0, 1.0);
    hsv.v = std::clamp(hsv.v, 0.0, 1.0);
    const double c = hsv.v * hsv.s;
    const double hh = std::fmod(hsv.h / 60.0, 6.0);
    const double x = c * (1.0 - std::abs(std::fmod(hh, 2.0) - 1.0));
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    if (hh < 1.0) {
        r = c; g = x;
    } else if (hh < 2.0) {
        r = x; g = c;
    } else if (hh < 3.0) {
        g = c; b = x;
    } else if (hh < 4.0) {
        g = x; b = c;
    } else if (hh < 5.0) {
        r = x; b = c;
    } else {
        r = c; b = x;
    }
    const double m = hsv.v - c;
    return Color::rgb(static_cast<std::uint8_t>(color_byte(r + m)),
                      static_cast<std::uint8_t>(color_byte(g + m)),
                      static_cast<std::uint8_t>(color_byte(b + m)));
}

std::string hex_from_hsv(HsvColor hsv) {
    return hex_from_color(hsv_to_rgb(hsv));
}

HsvColor hsv_from_hex(std::string_view raw, HsvColor fallback) {
    const std::string hex = normalize_hex_color(raw);
    if (hex.empty()) return fallback;
    std::uint32_t rgba = 0;
    if (!detail::parse_hex_color(hex, rgba)) return fallback;
    return rgb_to_hsv(detail::unpack_rgba(rgba));
}

lxb_dom_element_t* colorfield_owner(lxb_dom_element_t* field) {
    lxb_dom_element_t* named = nullptr;
    for (auto* current = field; current != nullptr;
         current = detail::parent_element(current)) {
        if (detail::attr_string(current, "data-aui-widget") == "colorfield") {
            return current;
        }
        if (!named && !detail::attr_string(current, "data-aui-name").empty()) {
            named = current;
        }
    }
    return named ? named : field;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
HsvColor current_dcs_colorfield_hsv(lxb_dom_element_t* field) {
    HsvColor fallback{};
    auto try_value = [&](std::string_view value, HsvColor& out) {
        const std::string hex = normalize_hex_color(value);
        if (hex.empty()) return false;
        out = hsv_from_hex(hex, fallback);
        return true;
    };

    HsvColor out{};
    if (auto* owner = colorfield_owner(field)) {
        if (try_value(detail::attr_string(owner, "data-value"), out)) return out;
    }
    if (try_value(detail::attr_string(field, "data-value"), out)) return out;
    if (auto* chip = detail::first_descendant_with_class(
            field, "dcs-colorfield__chip")) {
        if (try_value(detail::attr_string(chip, "data-dcs-color"), out)) return out;
    }
    if (auto* input = detail::first_descendant_with_class(
            field, "dcs-colorfield__hex")) {
        if (try_value(detail::attr_string(input, "value"), out)) return out;
    }
    return fallback;
}

bool sync_dcs_colorfield(detail::DocumentImpl& impl,
                         lxb_dom_element_t* field,
                         HsvColor hsv,
                         bool emit,
                         bool live) {
    if (!field) return false;
    hsv.s = std::clamp(hsv.s, 0.0, 1.0);
    hsv.v = std::clamp(hsv.v, 0.0, 1.0);
    hsv.h = std::fmod(hsv.h, 360.0);
    if (hsv.h < 0.0) hsv.h += 360.0;

    auto* owner = colorfield_owner(field);
    const std::string previous = normalize_hex_color(
        owner ? detail::attr_string(owner, "data-value")
              : detail::attr_string(field, "data-value"));
    const std::string hex = hex_from_hsv(hsv);
    const std::string hue_hex = hex_from_hsv({hsv.h, 1.0, 1.0});

    bool changed = false;
    changed = detail::set_attribute_on_element(impl, field, "data-value", hex) ||
              changed;
    if (owner && owner != field) {
        changed = detail::set_attribute_on_element(impl, owner, "data-value", hex) ||
                  changed;
    }
    if (auto* chip = detail::first_descendant_with_class(
            field, "dcs-colorfield__chip")) {
        changed = detail::set_attribute_on_element(impl, chip, "data-dcs-color", hex) ||
                  changed;
        changed = detail::set_attribute_on_element(
                      impl, chip, "style",
                      detail::style_with_properties(
                          detail::attr_string(chip, "style"),
                          {{"--c", hex}, {"background", hex}})) ||
                  changed;
    }
    if (auto* input = detail::first_descendant_with_class(
            field, "dcs-colorfield__hex")) {
        changed = detail::set_attribute_on_element(impl, input, "value", hex) ||
                  changed;
        const int idx = detail::block_index_for_exact_element(impl, input);
        if (idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(idx)];
            if (block.text_control) {
                detail::set_live_text_value(impl, idx, block, hex);
            }
        }
    }
    if (auto* preview_chip = detail::first_descendant_with_class(
            field, "dcs-colorfield__picker-chip")) {
        changed = detail::set_attribute_on_element(
                      impl, preview_chip, "style",
                      detail::style_with_properties(
                          detail::attr_string(preview_chip, "style"),
                          {{"--c", hex}, {"background", hex}})) ||
                  changed;
    }
    if (auto* preview_input = detail::first_descendant_with_class(
            field, "dcs-colorfield__picker-input")) {
        changed = detail::set_attribute_on_element(impl, preview_input, "value", hex) ||
                  changed;
        const int idx = detail::block_index_for_exact_element(impl, preview_input);
        if (idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(idx)];
            if (block.text_control) {
                detail::set_live_text_value(impl, idx, block, hex);
            }
        }
    }
    if (auto* square = detail::first_descendant_with_class(
            field, "dcs-color-square")) {
        changed = detail::set_attribute_on_element(
                      impl, square, "style",
                      detail::style_with_properties(
                          detail::attr_string(square, "style"),
                          {{"--hue", hue_hex},
                           {"aspect-ratio", "1.4 / 1"}})) ||
                  changed;
        if (auto* cursor = detail::first_descendant_with_class(
                square, "dcs-color-square__cursor")) {
            changed = detail::set_attribute_on_element(
                          impl, cursor, "style",
                          detail::style_with_properties(
                              detail::attr_string(cursor, "style"),
                              {{"left", detail::percent_string(hsv.s)},
                               {"top", detail::percent_string(1.0 - hsv.v)}})) ||
                      changed;
        }
    }
    if (auto* hue = detail::first_descendant_with_class(field, "dcs-hue-bar")) {
        if (auto* cursor = detail::first_descendant_with_class(
                hue, "dcs-hue-bar__cursor")) {
            changed = detail::set_attribute_on_element(
                          impl, cursor, "style",
                          detail::style_with_properties(
                              detail::attr_string(cursor, "style"),
                              {{"left", detail::percent_string(hsv.h / 360.0)}})) ||
                      changed;
        }
    }

    if (emit && hex != previous) {
        detail::emit_widget_change(impl, owner ? owner : field, hex, live);
    }
    return changed;
}

bool sync_dcs_colorfield(detail::DocumentImpl& impl,
                         lxb_dom_element_t* field,
                         std::string_view raw_hex,
                         bool emit,
                         bool live) {
    const std::string hex = normalize_hex_color(raw_hex);
    if (hex.empty()) return false;
    return detail::sync_dcs_colorfield(impl, field,
                               hsv_from_hex(hex, HsvColor{}), emit, live);
}
}  // namespace detail
namespace {

bool colorfield_part_kind(lxb_dom_element_t* elem,
                          detail::DocumentImpl::ColorfieldDrag::Kind& kind) {
    using Kind = detail::DocumentImpl::ColorfieldDrag::Kind;
    if (detail::class_list_contains(elem, "dcs-colorfield__chip")) {
        kind = Kind::Chip;
        return true;
    }
    if (detail::class_list_contains(elem, "dcs-color-square")) {
        kind = Kind::Square;
        return true;
    }
    if (detail::class_list_contains(elem, "dcs-hue-bar")) {
        kind = Kind::Hue;
        return true;
    }
    return false;
}

bool find_dcs_colorfield_part_at(
    detail::DocumentImpl& impl,
    int from_idx,
    lxb_dom_element_t*& out_field,
    lxb_dom_element_t*& out_part,
    detail::DocumentImpl::ColorfieldDrag::Kind& out_kind) {
    using Kind = detail::DocumentImpl::ColorfieldDrag::Kind;
    out_field = nullptr;
    out_part = nullptr;
    out_kind = Kind::None;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        Kind kind = Kind::None;
        if (!out_part && colorfield_part_kind(elem, kind)) {
            out_part = elem;
            out_kind = kind;
        }
        if (out_part && detail::class_list_contains(elem, "dcs-colorfield")) {
            out_field = elem;
            return true;
        }
    }
    return false;
}

bool find_dcs_colorfield_part_at_point(
    detail::DocumentImpl& impl,
    Point point,
    lxb_dom_element_t*& out_field,
    lxb_dom_element_t*& out_part,
    detail::DocumentImpl::ColorfieldDrag::Kind& out_kind) {
    using Kind = detail::DocumentImpl::ColorfieldDrag::Kind;
    out_field = nullptr;
    out_part = nullptr;
    out_kind = Kind::None;
    for (std::size_t i = impl.blocks.size(); i-- > 0; ) {
        auto* elem = detail::element_for_block(impl, static_cast<int>(i));
        if (!elem) continue;
        Kind kind = Kind::None;
        if (!colorfield_part_kind(elem, kind)) continue;
        const Rect bounds = detail::block_border_visual_rect(impl, static_cast<int>(i));
        if (bounds.w <= 0 || bounds.h <= 0 ||
            !detail::rect_contains(bounds, point.x, point.y)) {
            continue;
        }
        auto* field = detail::nearest_ancestor_with_class(elem, "dcs-colorfield");
        if (!field) continue;
        out_field = field;
        out_part = elem;
        out_kind = kind;
        return true;
    }
    return false;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool begin_dcs_colorfield_drag(detail::DocumentImpl& impl,
                               int from_idx,
                               const Event& ev) {
    using Kind = detail::DocumentImpl::ColorfieldDrag::Kind;
    lxb_dom_element_t* field = nullptr;
    lxb_dom_element_t* part = nullptr;
    Kind kind = Kind::None;
    if (!find_dcs_colorfield_part_at(impl, from_idx, field, part, kind) &&
        !find_dcs_colorfield_part_at_point(impl, ev.pos, field, part, kind)) {
        return false;
    }
    const int part_idx = detail::block_index_for_exact_element(impl, part);
    if (part_idx < 0) return false;
    const HsvColor hsv = detail::current_dcs_colorfield_hsv(field);
    impl.colorfield_drag.kind = kind;
    impl.colorfield_drag.field = field;
    impl.colorfield_drag.part = part;
    impl.colorfield_drag.bounds = detail::block_border_visual_rect(impl, part_idx);
    impl.colorfield_drag.start_x = ev.pos.x;
    impl.colorfield_drag.start_y = ev.pos.y;
    impl.colorfield_drag.h = hsv.h;
    impl.colorfield_drag.s = hsv.s;
    impl.colorfield_drag.v = hsv.v;
    return true;
}

bool update_dcs_colorfield_drag(detail::DocumentImpl& impl, const Event& ev) {
    using Kind = detail::DocumentImpl::ColorfieldDrag::Kind;
    auto& drag = impl.colorfield_drag;
    if (drag.kind == Kind::None || !drag.field) return false;

    HsvColor next{drag.h, drag.s, drag.v};
    if (drag.kind == Kind::Chip) {
        const double dx = static_cast<double>(ev.pos.x - drag.start_x);
        const double dy = static_cast<double>(drag.start_y - ev.pos.y);
        next.v = std::clamp(drag.v + dy / 200.0, 0.0, 1.0);
        if (ev.ctrl || ev.super) {
            next.s = std::clamp(drag.s + dx / 200.0, 0.0, 1.0);
        } else {
            next.h = std::fmod(drag.h + dx, 360.0);
            if (next.h < 0.0) next.h += 360.0;
        }
    } else if (drag.kind == Kind::Square) {
        next.s = drag.bounds.w > 0
            ? std::clamp((static_cast<double>(ev.pos.x) - drag.bounds.x) /
                             drag.bounds.w,
                         0.0, 1.0)
            : 0.0;
        next.v = drag.bounds.h > 0
            ? 1.0 - std::clamp(
                        (static_cast<double>(ev.pos.y) - drag.bounds.y) /
                            drag.bounds.h,
                        0.0, 1.0)
            : 1.0;
    } else if (drag.kind == Kind::Hue) {
        next.h = drag.bounds.w > 0
            ? std::clamp((static_cast<double>(ev.pos.x) - drag.bounds.x) /
                             drag.bounds.w,
                         0.0, 1.0) *
                  360.0
            : 0.0;
    }
    // Scrub in flight — live changes; finish_dcs_colorfield_drag emits
    // the committed change when the gesture ends.
    return detail::sync_dcs_colorfield(impl, drag.field, next, /*emit=*/true,
                                       /*live=*/true);
}

bool finish_dcs_colorfield_drag(detail::DocumentImpl& impl, const Event& ev) {
    auto& drag = impl.colorfield_drag;
    using Kind = detail::DocumentImpl::ColorfieldDrag::Kind;
    if (drag.kind == Kind::None || !drag.field) return false;
    const std::string start_hex = hex_from_hsv({drag.h, drag.s, drag.v});
    const bool changed = update_dcs_colorfield_drag(impl, ev);
    // One committed change per gesture, with the final colour (the
    // moves emitted live changes only). A press with no colour change
    // (a plain chip click) commits nothing.
    auto* owner = colorfield_owner(drag.field);
    const std::string hex = normalize_hex_color(
        owner ? detail::attr_string(owner, "data-value")
              : detail::attr_string(drag.field, "data-value"));
    if (!hex.empty() && hex != start_hex) {
        detail::emit_widget_change(impl, owner ? owner : drag.field, hex,
                                   /*live=*/false);
    }
    return changed;
}
}  // namespace detail
namespace {

std::string button_group_option_value(lxb_dom_element_t* elem) {
    if (!elem) return {};
    auto value = detail::attr_string(elem, "value");
    if (!value.empty()) return value;
    value = detail::attr_string(elem, "data-dcs-value");
    if (!value.empty()) return value;
    value = detail::node_text(lxb_dom_interface_node(elem));
    return std::string(detail::trim_css_ws(value));
}

bool update_button_group_option_states(detail::DocumentImpl& impl,
                                       lxb_dom_element_t* elem,
                                       std::string_view selected) {
    if (!elem) return false;
    bool changed = false;
    if (detail::tag_name(elem) == "button" &&
        (detail::has_attr(elem, "value") ||
         detail::has_attr(elem, "data-dcs-value") ||
         detail::class_list_contains(elem, "dcs-btn"))) {
        const bool active = button_group_option_value(elem) == selected;
        changed = detail::set_attribute_on_element(
            impl, elem, "aria-pressed", active ? "true" : "false") ||
            changed;
        if (detail::class_list_contains(elem, "btn")) {
            changed = detail::set_attribute_on_element(
                impl, elem, "class",
                active ? "btn btn-primary" : "btn btn-outline-primary") || changed;
        }
        if (detail::class_list_contains(elem, "dcs-btn")) {
            changed = detail::set_attribute_on_element(
                impl, elem, "class",
                detail::class_list_set(elem, "dcs-btn--primary", active)) || changed;
        }
    }
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        changed = update_button_group_option_states(
            impl, lxb_dom_interface_element(child), selected) || changed;
    }
    return changed;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool update_button_group_control(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* group,
                                 lxb_dom_element_t* option) {
    if (!group || !option) return false;
    const auto selected = button_group_option_value(option);
    if (selected.empty()) return false;
    bool changed =
        detail::set_attribute_on_element(impl, group, "data-value", selected);
    changed = update_button_group_option_states(impl, group, selected) || changed;
    if (changed) detail::emit_widget_change(impl, group, selected);
    return changed;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool find_dropdown_control_at(detail::DocumentImpl& impl,
                              int from_idx,
                              lxb_dom_element_t*& out_group,
                              lxb_dom_element_t*& out_select,
                              lxb_dom_element_t*& out_option) {
    out_group = nullptr;
    out_select = nullptr;
    out_option = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        if (!out_option && block.tag == "button" &&
            detail::block_has_attr(block, "value") &&
            detail::block_attr_value(block, "role") &&
            *detail::block_attr_value(block, "role") == "option" &&
            !block.is_disabled) {
            out_option = elem;
        }
        if (!out_select && block.tag == "select" && !block.is_disabled) {
            out_select = elem;
        }
        const auto* widget = detail::block_attr_value(block, "data-aui-widget");
        if (widget && *widget == "dropdown") {
            out_group = elem;
            return out_select != nullptr || out_option != nullptr;
        }
    }
    out_group = nullptr;
    out_select = nullptr;
    out_option = nullptr;
    return false;
}
}  // namespace detail
namespace {

bool update_dropdown_selection_states(detail::DocumentImpl& impl,
                                      lxb_dom_element_t* elem,
                                      std::string_view selected) {
    if (!elem) return false;
    bool changed = false;
    const auto tag = detail::tag_name(elem);
    if (tag == "select") {
        changed =
            detail::set_attribute_on_element(impl, elem, "value", selected) || changed;
    } else if (tag == "option" && detail::has_attr(elem, "value")) {
        const bool active = detail::attr_string(elem, "value") == selected;
        changed = active
            ? (detail::set_attribute_on_element(impl, elem, "selected", "selected") || changed)
            : (detail::remove_attribute_on_element(impl, elem, "selected") || changed);
    } else if (tag == "button" && detail::has_attr(elem, "value") &&
               detail::attr_string(elem, "role") == "option") {
        const bool active = detail::attr_string(elem, "value") == selected;
        changed = active
            ? (detail::set_attribute_on_element(impl, elem, "aria-selected", "true") || changed)
            : (detail::remove_attribute_on_element(impl, elem, "aria-selected") || changed);
        if (detail::class_list_contains(elem, "dropdown-item")) {
            changed = detail::set_attribute_on_element(
                impl, elem, "class", detail::class_list_set(elem, "active", active)) || changed;
        }
        if (detail::class_list_contains(elem, "dcs-menu__item")) {
            changed = detail::set_attribute_on_element(
                impl, elem, "class",
                detail::class_list_set(elem, "dcs-menu__item--active", active)) || changed;
        }
    }
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        changed = update_dropdown_selection_states(
            impl, lxb_dom_interface_element(child), selected) || changed;
    }
    return changed;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
int viewport_width_for_overlay(const detail::DocumentImpl& impl) {
    if (impl.media_viewport_width_px > 0) return impl.media_viewport_width_px;
    return std::max(1, impl.content_size.width);
}

int viewport_height_for_overlay(const detail::DocumentImpl& impl) {
    if (impl.media_viewport_height_px > 0) return impl.media_viewport_height_px;
    return std::max(1, impl.content_size.height);
}
}  // namespace detail
namespace {


int overlay_item_count(lxb_dom_element_t* elem) {
    if (!elem) return 0;
    int count = 0;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* child_elem = lxb_dom_interface_element(child);
        if (detail::class_list_contains(child_elem, "dcs-menu__item") ||
            detail::class_list_contains(child_elem, "dropdown-item") ||
            detail::attr_string(child_elem, "role") == "option" ||
            detail::attr_string(child_elem, "role") == "menuitem") {
            ++count;
        }
        count += overlay_item_count(child_elem);
    }
    return count;
}

bool element_has_direct_text(lxb_dom_element_t* elem) {
    if (!elem) return false;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_TEXT) continue;
        if (!detail::node_text(child).empty()) return true;
    }
    return false;
}

int computed_border_padding_height(const detail::ComputedStyle& cs) {
    return cs.padding_top + cs.padding_bottom +
           cs.used_border_top() + cs.used_border_bottom();
}

int computed_border_padding_width(const detail::ComputedStyle& cs) {
    return cs.padding_left + cs.padding_right +
           cs.used_border_left() + cs.used_border_right();
}

int computed_outer_declared_width(const detail::ComputedStyle& cs) {
    if (cs.width <= 0) return 0;
    int w = cs.width;
    if (cs.box_sizing == detail::ComputedStyle::BoxSizing::ContentBox) {
        w += computed_border_padding_width(cs);
    }
    return std::max(1, w);
}

int computed_outer_declared_height(const detail::ComputedStyle& cs) {
    if (cs.height <= 0) return 0;
    int h = cs.height;
    if (cs.box_sizing == detail::ComputedStyle::BoxSizing::ContentBox) {
        h += computed_border_padding_height(cs);
    }
    return std::max(1, h);
}

int estimate_hidden_overlay_height_from_css(const detail::DocumentImpl& impl,
                                            lxb_dom_element_t* elem,
                                            int depth = 0) {
    if (!elem || !impl.resolver || depth > 8) return 0;

    auto rs = impl.resolver->resolve(elem, impl.root_style);
    const auto& cs = rs.computed;
    if (depth > 0 &&
        cs.display == detail::ComputedStyle::Display::None) {
        return 0;
    }

    if (const int declared = computed_outer_declared_height(cs); declared > 0) {
        return declared;
    }

    int children_h = 0;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* child_elem = lxb_dom_interface_element(child);
        auto child_rs = impl.resolver->resolve(child_elem, impl.root_style);
        const auto& child_cs = child_rs.computed;
        if (child_cs.position == detail::ComputedStyle::Position::Absolute ||
            child_cs.position == detail::ComputedStyle::Position::Fixed) {
            continue;
        }
        children_h += estimate_hidden_overlay_height_from_css(
            impl, child_elem, depth + 1);
    }

    int content_h = children_h;
    if (content_h <= 0 && element_has_direct_text(elem)) {
        // Overlay estimation runs pre-layout without a rasterizer at hand;
        // the kNormalLineHeight substitute is fine here (the value only
        // seeds an anchored-overlay placement budget).
        content_h = std::max(
            1, static_cast<int>(std::ceil(
                   detail::resolved_line_height_px(cs, 0.0f))));
    }

    if (content_h <= 0) return 0;
    return std::max(1, content_h + computed_border_padding_height(cs));
}

int overlay_estimated_height(const detail::DocumentImpl& impl,
                             lxb_dom_element_t* elem,
                             int fallback) {
    const int idx = detail::block_index_for_exact_element(impl, elem);
    if (idx >= 0) {
        const Rect rect = detail::block_border_visual_rect(impl, idx);
        if (rect.h > 0) return rect.h;
    }
    if (const int css_estimate =
            estimate_hidden_overlay_height_from_css(impl, elem);
        css_estimate > 0) {
        return std::clamp(css_estimate, 1, 240);
    }
    const int count = overlay_item_count(elem);
    if (count > 0) return std::clamp(count * 24 + 8, 24, 240);
    return std::max(1, fallback);
}

int overlay_declared_outer_height(const detail::DocumentImpl& impl,
                                  lxb_dom_element_t* elem,
                                  int fallback) {
    if (!elem || !impl.resolver) return overlay_estimated_height(impl, elem, fallback);

    auto rs = impl.resolver->resolve(elem, impl.root_style);
    const auto& cs = rs.computed;
    if (const int declared = computed_outer_declared_height(cs);
        declared > 0) {
        return declared;
    }
    return overlay_estimated_height(impl, elem, fallback);
}

int overlay_declared_outer_width(const detail::DocumentImpl& impl,
                                 lxb_dom_element_t* elem,
                                 int fallback) {
    if (!elem || !impl.resolver) return std::max(1, fallback);
    auto rs = impl.resolver->resolve(elem, impl.root_style);
    if (const int declared = computed_outer_declared_width(rs.computed);
        declared > 0) {
        return declared;
    }
    const int idx = detail::block_index_for_exact_element(impl, elem);
    if (idx >= 0) {
        const Rect rect = detail::block_border_visual_rect(impl, idx);
        if (rect.w > 0) return rect.w;
    }
    return std::max(1, fallback);
}

struct OverlayPlacement {
    int left{8};
    int top{8};
    int max_height{1};
    std::string side{"bottom"};
};

OverlayPlacement place_anchored_overlay(const detail::DocumentImpl& impl,
                                        Rect anchor,
                                        int overlay_width,
                                        int overlay_height,
                                        std::string_view placement,
                                        int gap) {
    constexpr int edge = 8;
    const int viewport_w = detail::viewport_width_for_overlay(impl);
    const int viewport_h = detail::viewport_height_for_overlay(impl);
    overlay_width = std::clamp(std::max(1, overlay_width), 1,
                               std::max(1, viewport_w - edge * 2));
    const int desired_height =
        std::clamp(std::max(1, overlay_height), 1,
                   std::max(1, viewport_h - edge * 2));

    std::string side{"bottom"};
    if (placement.rfind("top", 0) == 0) side = "top";
    else if (placement.rfind("left", 0) == 0) side = "left";
    else if (placement.rfind("right", 0) == 0) side = "right";
    const bool end_aligned = placement.find("end") != std::string_view::npos;

    const int above = anchor.y - edge;
    const int below = viewport_h - (anchor.y + anchor.h) - edge;
    const int left_space = anchor.x - edge;
    const int right_space = viewport_w - (anchor.x + anchor.w) - edge;
    if (side == "bottom" && below < desired_height + gap && above > below) {
        side = "top";
    } else if (side == "top" && above < desired_height + gap && below > above) {
        side = "bottom";
    } else if (side == "right" &&
               right_space < overlay_width + gap && left_space > right_space) {
        side = "left";
    } else if (side == "left" &&
               left_space < overlay_width + gap && right_space > left_space) {
        side = "right";
    }

    int available_height = std::max(1, viewport_h - edge * 2);
    if (side == "bottom") {
        available_height = std::max(1, below);
    } else if (side == "top") {
        available_height = std::max(1, above);
    }
    overlay_height = std::min(desired_height, available_height);

    int left = anchor.x;
    int top = anchor.y + anchor.h + gap;
    if (side == "top") {
        top = anchor.y - overlay_height;
        left = end_aligned ? anchor.x + anchor.w - overlay_width : anchor.x;
    } else if (side == "bottom") {
        top = anchor.y + anchor.h + gap;
        left = end_aligned ? anchor.x + anchor.w - overlay_width : anchor.x;
    } else if (side == "left") {
        left = anchor.x - overlay_width - gap;
        top = anchor.y;
    } else {
        left = anchor.x + anchor.w + gap;
        top = anchor.y;
    }

    const int max_left =
        std::max(edge, viewport_w - overlay_width - edge);
    const int max_top = std::max(edge, viewport_h - overlay_height - edge);
    OverlayPlacement out;
    out.left = std::clamp(left, edge, max_left);
    out.top = std::clamp(top, edge, max_top);
    out.max_height = overlay_height;
    out.side = std::move(side);
    return out;
}

bool hide_dropdown_menu(detail::DocumentImpl& impl, lxb_dom_element_t* group) {
    auto* menu = detail::first_descendant_with_class(group, "aui-select__menu");
    if (!menu) return false;
    bool changed = detail::set_attribute_on_element(impl, menu, "hidden", "");
    changed = detail::remove_attribute_on_element(impl, menu, "style") || changed;
    return changed;
}

std::string dropdown_menu_open_style(const detail::DocumentImpl& impl,
                                     lxb_dom_element_t* group,
                                     lxb_dom_element_t* menu) {
    int width = 160;
    Rect anchor_rect{0, 0, width, 1};
    auto* anchor = detail::first_descendant_tag(group, "select");
    if (!anchor) anchor = group;
    const int anchor_idx = detail::block_index_for_element_or_ancestor(impl, anchor);
    if (anchor_idx >= 0) {
        anchor_rect = detail::block_border_visual_rect(impl, anchor_idx);
        width = std::max(1, anchor_rect.w);
    }
    const auto placed = place_anchored_overlay(
        impl, anchor_rect, width,
        overlay_declared_outer_height(impl, menu, 160),
        "bottom", 0);
    return "display:flex;position:fixed;left:" + std::to_string(placed.left) +
           "px;top:" + std::to_string(placed.top) +
           "px;width:" + std::to_string(width) +
           "px;min-width:" + std::to_string(width) +
           "px;max-width:" + std::to_string(width) +
           "px;max-height:" + std::to_string(placed.max_height) +
           "px;overflow:auto;flex-direction:column;align-items:stretch;z-index:400";
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool toggle_dropdown_menu(detail::DocumentImpl& impl, lxb_dom_element_t* group) {
    auto* menu = detail::first_descendant_with_class(group, "aui-select__menu");
    if (!menu) return false;
    if (!detail::has_attr(menu, "hidden")) {
        return hide_dropdown_menu(impl, group);
    }
    const std::string open_style = dropdown_menu_open_style(impl, group, menu);
    bool changed = detail::close_transient_layers(impl, menu);
    changed = detail::remove_attribute_on_element(impl, menu, "hidden") || changed;
    changed = detail::set_attribute_on_element(
        impl, menu, "style", open_style) ||
        changed;
    return changed;
}

bool update_dropdown_control(detail::DocumentImpl& impl,
                             lxb_dom_element_t* group,
                             lxb_dom_element_t* option) {
    if (!group || !option) return false;
    const auto selected = detail::attr_string(option, "value");
    if (selected.empty()) return false;
    bool changed =
        detail::set_attribute_on_element(impl, group, "data-value", selected);
    changed = update_dropdown_selection_states(impl, group, selected) || changed;
    changed = hide_dropdown_menu(impl, group) || changed;
    if (changed) detail::emit_widget_change(impl, group, selected);
    return changed;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
lxb_dom_node_t* document_dom_root(detail::DocumentImpl& impl) {
    if (!impl.doc) return nullptr;
    auto* body = lxb_html_document_body_element(impl.doc);
    return body ? lxb_dom_interface_node(body) : lxb_dom_interface_node(impl.doc);
}
}  // namespace detail
namespace {

bool close_dropdown_menu_element(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* menu) {
    if (!menu) return false;
    bool changed = detail::set_attribute_on_element(impl, menu, "hidden", "");
    changed = detail::remove_attribute_on_element(impl, menu, "style") || changed;
    return changed;
}

bool close_all_dropdown_menus(detail::DocumentImpl& impl,
                              lxb_dom_element_t* except = nullptr) {
    std::vector<lxb_dom_element_t*> menus;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (elem != except && detail::class_list_contains(elem, "aui-select__menu") &&
            !detail::has_attr(elem, "hidden")) {
            menus.push_back(elem);
        }
    };
    detail::walk_dom_elements(detail::document_dom_root(impl), collect);

    bool changed = false;
    for (auto* menu : menus) {
        changed = close_dropdown_menu_element(impl, menu) || changed;
    }
    return changed;
}

std::string target_id_from_selector(std::string_view selector) {
    selector = detail::trim_css_ws(selector);
    if (selector.empty() || selector.front() != '#') return {};
    selector.remove_prefix(1);
    selector = detail::trim_css_ws(selector);
    std::size_t end = 0;
    while (end < selector.size() &&
           !std::isspace(static_cast<unsigned char>(selector[end]))) {
        ++end;
    }
    return std::string(selector.substr(0, end));
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Resolve a trigger's data-dcs-target (or href) "#id" selector to its
// element — how decius.js identifies a tab's panel. Dock surgery resolves
// panels through THIS, not an id naming convention: raw-HTML documents (the
// DENDER N-panel) target "#npanel-tool" directly, with no "-body" suffix.
lxb_dom_element_t* dcs_target_for_trigger(detail::DocumentImpl& impl,
                                          lxb_dom_element_t* trigger) {
    if (!trigger) return nullptr;
    auto selector = detail::attr_string(trigger, "data-dcs-target");
    if (selector.empty()) selector = detail::attr_string(trigger, "href");
    const auto target_id = target_id_from_selector(selector);
    return target_id.empty() ? nullptr : detail::find_dom_element_by_id(impl, target_id);
}
}  // namespace detail
namespace {

// ── Floating element drag (data-dcs-drag + data-dcs-drag-handle) ─────────────
// A [data-dcs-drag] container (a floating toolbar or torn-off panel) is moved
// by dragging a [data-dcs-drag-handle] inside it. Movement writes inline
// left/top and is clamped to the [data-dcs-drag-bounds] container. Mirrors
// decius drag; position persists via result.layout_changed like the splitter.
// Rebuild an inline style with left/top set and right/bottom dropped (so the
// written left/top win), preserving every other declaration.
std::string with_float_position(std::string_view style, int left, int top) {
    std::string out;
    std::size_t i = 0;
    while (i < style.size()) {
        const std::size_t semi = style.find(';', i);
        const std::string_view decl = style.substr(
            i, semi == std::string_view::npos ? semi : semi - i);
        i = (semi == std::string_view::npos) ? style.size() : semi + 1;
        const std::size_t colon = decl.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view prop = detail::trim_css_ws(decl.substr(0, colon));
            if (prop == "left" || prop == "top" || prop == "right" ||
                prop == "bottom")
                continue;
        }
        const std::string_view t = detail::trim_css_ws(decl);
        if (!t.empty()) {
            if (!out.empty()) out += ';';
            out += t;
        }
    }
    if (!out.empty()) out += ';';
    out += "left:" + std::to_string(left) + "px;top:" + std::to_string(top) +
           "px";
    return out;
}

std::string with_float_rect(std::string_view style,
                            int left,
                            int top,
                            int width,
                            int height) {
    std::string out;
    std::size_t i = 0;
    while (i < style.size()) {
        const std::size_t semi = style.find(';', i);
        const std::string_view decl = style.substr(
            i, semi == std::string_view::npos ? semi : semi - i);
        i = (semi == std::string_view::npos) ? style.size() : semi + 1;
        const std::size_t colon = decl.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view prop = detail::trim_css_ws(decl.substr(0, colon));
            if (prop == "left" || prop == "top" || prop == "right" ||
                prop == "bottom" || prop == "width" || prop == "height" ||
                prop == "transform")
                continue;
        }
        const std::string_view t = detail::trim_css_ws(decl);
        if (!t.empty()) {
            if (!out.empty()) out += ';';
            out += t;
        }
    }
    if (!out.empty()) out += ';';
    out += "left:" + std::to_string(left) + "px;top:" +
           std::to_string(top) + "px;width:" + std::to_string(width) +
           "px;height:" + std::to_string(height) +
           "px;right:auto;bottom:auto;transform:none";
    return out;
}

// The [data-dcs-drag-bounds] container for `dragged`: the selector (.class or
// #id) is matched against the float's ancestors (the bounds box always encloses
// the float). Returns null if there is no selector / no match.
lxb_dom_element_t* resolve_drag_bounds_elem(detail::DocumentImpl& impl,
                                            lxb_dom_element_t* dragged,
                                            std::string_view selector) {
    (void) impl;
    if (!dragged || selector.empty()) return nullptr;
    const bool by_id = selector.front() == '#';
    const std::string name(
        (by_id || selector.front() == '.') ? selector.substr(1) : selector);
    for (auto* n = lxb_dom_node_parent(lxb_dom_interface_node(dragged)); n;
         n = lxb_dom_node_parent(n)) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(n);
        const bool m = by_id ? (detail::attr_string(e, "id") == name)
                             : detail::class_list_contains(e, name);
        if (m) return e;
    }
    return nullptr;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
Rect root_float_host_bounds(detail::DocumentImpl& impl) {
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* e = detail::element_for_block(impl, i);
        if (e && detail::class_list_contains(e, "dcs-dock--floathost")) {
            return impl.blocks[static_cast<std::size_t>(i)].bounds;
        }
    }
    return {};
}

Rect document_float_host_bounds(detail::DocumentImpl& impl) {
    Rect fallback = detail::root_float_host_bounds(impl);
    Rect best{};
    long long best_area = 0;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* e = detail::element_for_block(impl, i);
        if (!e || !detail::has_attr(e, "data-dcs-float-host")) continue;
        if (detail::class_list_contains(e, "dcs-dock--floathost")) continue;
        const auto& b = impl.blocks[static_cast<std::size_t>(i)].bounds;
        if (b.w <= 0 || b.h <= 0) continue;
        const long long area = static_cast<long long>(b.w) * b.h;
        if (best_area == 0 || area < best_area) {
            best = b;
            best_area = area;
        }
    }
    return best_area > 0 ? best : fallback;
}
}  // namespace detail
namespace {


enum FloatResizeDir {
    FloatResizeN = 1,
    FloatResizeS = 2,
    FloatResizeW = 4,
    FloatResizeE = 8,
};

int float_resize_dir_for_point(const Rect& b, Point p) {
    if (b.w <= 0 || b.h <= 0) return 0;
    constexpr int kEdge = 5;
    constexpr int kCorner = 12;
    const int dx = p.x - b.x;
    const int dy = p.y - b.y;
    if (dx < 0 || dy < 0 || dx >= b.w || dy >= b.h) return 0;
    if (dx < kCorner && dy < kCorner) return FloatResizeN | FloatResizeW;
    if (dx >= b.w - kCorner && dy < kCorner) return FloatResizeN | FloatResizeE;
    if (dx < kCorner && dy >= b.h - kCorner) return FloatResizeS | FloatResizeW;
    if (dx >= b.w - kCorner && dy >= b.h - kCorner)
        return FloatResizeS | FloatResizeE;
    if (dy < kEdge) return FloatResizeN;
    if (dy >= b.h - kEdge) return FloatResizeS;
    if (dx < kEdge) return FloatResizeW;
    if (dx >= b.w - kEdge) return FloatResizeE;
    return 0;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
int cursor_for_float_resize_dir(int dir) {
    const bool n = (dir & FloatResizeN) != 0;
    const bool s = (dir & FloatResizeS) != 0;
    const bool w = (dir & FloatResizeW) != 0;
    const bool e = (dir & FloatResizeE) != 0;
    if ((n && w) || (s && e)) return 8;  // nwse
    if ((n && e) || (s && w)) return 9;  // nesw
    if (w || e) return 6;
    if (n || s) return 7;
    return 0;
}
}  // namespace detail
namespace {

int float_resize_dir_from_token(std::string_view dir) {
    int out = 0;
    if (dir.find('n') != std::string_view::npos) out |= FloatResizeN;
    if (dir.find('s') != std::string_view::npos) out |= FloatResizeS;
    if (dir.find('w') != std::string_view::npos) out |= FloatResizeW;
    if (dir.find('e') != std::string_view::npos) out |= FloatResizeE;
    return out;
}

bool floating_resize_enabled(lxb_dom_element_t* elem) {
    if (!elem || detail::attr_string(elem, "data-dcs-resize") == "false") return false;
    if (detail::class_list_contains(elem, "dcs-panel--floating")) return true;
    return detail::class_list_contains(elem, "dcs-toolbar--floating") &&
           detail::attr_string(elem, "data-dcs-resize") == "true";
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// The CSS containing block for a floating (absolutely-positioned) element:
// the padding box of the nearest positioned ancestor, or the document
// origin when there is none. Drag/resize write `left`/`top` relative to
// THIS origin. The old derivation — element position minus its authored
// inset — silently assumed an authored px `left`/`top`; for a panel
// positioned by class rules, `right:`, percentages, or one whose style was
// lost, it mistook the element's own position for the containing-block
// origin, and a single drag teleported the panel off-screen (field trace:
// `write left/top=(-871,16)` — the "tearout completely disappeared" bug).
Point float_containing_block_origin(detail::DocumentImpl& impl, int idx) {
    using P = detail::ComputedStyle::Position;
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return {0, 0};
    for (int a = impl.blocks[static_cast<std::size_t>(idx)].parent_idx;
         a >= 0 && a < static_cast<int>(impl.blocks.size());
         a = impl.blocks[static_cast<std::size_t>(a)].parent_idx) {
        const auto& ab = impl.blocks[static_cast<std::size_t>(a)];
        const auto& acs = impl.style_store.computed(ab.id);
        if (acs.position != P::Static) {
            return {ab.bounds.x + acs.used_border_left(),
                    ab.bounds.y + acs.used_border_top()};
        }
    }
    return {0, 0};
}

bool find_float_resize_at(detail::DocumentImpl& impl, int from_idx, Point point,
                          detail::DocumentImpl::FloatResize& out) {
    int explicit_dir = 0;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (elem && (detail::class_list_contains(elem, "dcs-dockpane__tab") ||
                     detail::class_list_contains(elem, "dcs-dockpane__tab-close") ||
                     detail::class_list_contains(elem, "dcs-panel__title--dock-tab"))) {
            return false;
        }
        if (elem && explicit_dir == 0 &&
            detail::class_list_contains(elem, "dcs-panel__resize-zone")) {
            explicit_dir = float_resize_dir_from_token(detail::attr_string(elem, "data-dir"));
            if (explicit_dir == 0) {
                const std::string classes = detail::attr_string(elem, "class");
                if (classes.find("--nw") != std::string::npos)
                    explicit_dir = FloatResizeN | FloatResizeW;
                else if (classes.find("--ne") != std::string::npos)
                    explicit_dir = FloatResizeN | FloatResizeE;
                else if (classes.find("--sw") != std::string::npos)
                    explicit_dir = FloatResizeS | FloatResizeW;
                else if (classes.find("--se") != std::string::npos)
                    explicit_dir = FloatResizeS | FloatResizeE;
                else if (classes.find("--n") != std::string::npos)
                    explicit_dir = FloatResizeN;
                else if (classes.find("--s") != std::string::npos)
                    explicit_dir = FloatResizeS;
                else if (classes.find("--w") != std::string::npos)
                    explicit_dir = FloatResizeW;
                else if (classes.find("--e") != std::string::npos)
                    explicit_dir = FloatResizeE;
            }
        }
        if (!floating_resize_enabled(elem)) continue;
        const int bidx = detail::block_index_for_exact_element(impl, elem);
        if (bidx < 0) return false;
        const auto& blk = impl.blocks[static_cast<std::size_t>(bidx)];
        const int dir = explicit_dir != 0
            ? explicit_dir
            : float_resize_dir_for_point(blk.bounds, point);
        if (dir == 0) return false;
        const Point cb = float_containing_block_origin(impl, bidx);
        out = {};
        out.elem = elem;
        out.dir = dir;
        out.start_x = point.x;
        out.start_y = point.y;
        out.elem_doc_x = blk.bounds.x;
        out.elem_doc_y = blk.bounds.y;
        out.elem_w = blk.bounds.w;
        out.elem_h = blk.bounds.h;
        out.cb_x = cb.x;
        out.cb_y = cb.y;
        out.panel_id = detail::attr_string(elem, "data-dcs-dock-id");
        detail::dock_trace("float-resize-arm panel=" + out.panel_id +
                   " dir=" + std::to_string(dir) + " at=(" +
                   std::to_string(point.x) + "," + std::to_string(point.y) +
                   ") bounds=(" + std::to_string(blk.bounds.x) + "," +
                   std::to_string(blk.bounds.y) + "," +
                   std::to_string(blk.bounds.w) + "x" +
                   std::to_string(blk.bounds.h) + ")");
        if (auto* be = resolve_drag_bounds_elem(
                impl, elem, detail::attr_string(elem, "data-dcs-drag-bounds"))) {
            const int beidx = detail::block_index_for_exact_element(impl, be);
            if (beidx >= 0) {
                const auto& bb = impl.blocks[static_cast<std::size_t>(beidx)];
                out.bounds_x = bb.bounds.x;
                out.bounds_y = bb.bounds.y;
                out.bounds_w = bb.bounds.w;
                out.bounds_h = bb.bounds.h;
            }
        }
        if (!out.panel_id.empty()) {
            const Rect hb = detail::document_float_host_bounds(impl);
            if (hb.w > 0 && hb.h > 0) {
                out.bounds_x = hb.x;
                out.bounds_y = hb.y;
                out.bounds_w = hb.w;
                out.bounds_h = hb.h;
            }
        }
        return true;
    }
    return false;
}

bool find_float_drag_at(detail::DocumentImpl& impl, int from_idx, Point point,
                        detail::DocumentImpl::FloatDrag& out) {
    bool have_handle = false;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        // Match decius.js' gesture split: dragging a dock tab/title is a dock
        // operation, while dragging empty floating chrome moves the panel.
        if (detail::class_list_contains(elem, "dcs-dockpane__tab") ||
            detail::class_list_contains(elem, "dcs-dockpane__tab-close")) {
            return false;
        }
        const std::string tag = detail::tag_name(elem);
        if (tag == "button" || tag == "a" || tag == "input" ||
            tag == "select" || tag == "textarea" || tag == "label" ||
            detail::class_list_contains(elem, "dcs-btn") ||
            detail::class_list_contains(elem, "dcs-select") ||
            detail::class_list_contains(elem, "dcs-slider") ||
            detail::class_list_contains(elem, "dcs-fader") ||
            detail::class_list_contains(elem, "dcs-knob") ||
            detail::class_list_contains(elem, "dcs-combo")) {
            return false;
        }
        if (detail::has_attr(elem, "data-dcs-drag-handle")) have_handle = true;
        if (!detail::has_attr(elem, "data-dcs-drag")) continue;
        // The draggable container. Require the press to have started on a handle
        // inside it, so the toolbar's own buttons still click rather than drag.
        if (!have_handle) return false;
        const int bidx = detail::block_index_for_exact_element(impl, elem);
        if (bidx < 0) return false;
        const auto& blk = impl.blocks[static_cast<std::size_t>(bidx)];
        // Resolve the element's REAL containing block (nearest positioned
        // ancestor). The written left/top are relative to it, so this is
        // correct no matter how (or whether) the current position was
        // authored — inline px, class rule, right-anchoring, or a panel
        // whose style went missing.
        const Point cb = float_containing_block_origin(impl, bidx);
        out = {};
        out.elem = elem;
        out.block_idx = bidx;
        out.start_x = point.x;
        out.start_y = point.y;
        out.elem_doc_x = blk.bounds.x;
        out.elem_doc_y = blk.bounds.y;
        out.cur_x = blk.bounds.x;
        out.cur_y = blk.bounds.y;
        out.cb_x = cb.x;
        out.cb_y = cb.y;
        out.elem_w = blk.bounds.w;
        out.elem_h = blk.bounds.h;
        out.panel_id = detail::attr_string(elem, "data-dcs-dock-id");
        if (auto* be = resolve_drag_bounds_elem(
                impl, elem, detail::attr_string(elem, "data-dcs-drag-bounds"))) {
            const int beidx = detail::block_index_for_exact_element(impl, be);
            if (beidx >= 0) {
                const auto& bb = impl.blocks[static_cast<std::size_t>(beidx)];
                out.bounds_x = bb.bounds.x;
                out.bounds_y = bb.bounds.y;
                out.bounds_w = bb.bounds.w;
                out.bounds_h = bb.bounds.h;
            }
        }
        if (!out.panel_id.empty()) {
            const Rect hb = detail::document_float_host_bounds(impl);
            if (hb.w > 0 && hb.h > 0) {
                out.bounds_x = hb.x;
                out.bounds_y = hb.y;
                out.bounds_w = hb.w;
                out.bounds_h = hb.h;
            }
        }
        detail::dock_trace(
            "float-drag-arm elem_doc=(" + std::to_string(out.elem_doc_x) +
            "," + std::to_string(out.elem_doc_y) + ") cb=(" +
            std::to_string(out.cb_x) + "," + std::to_string(out.cb_y) +
            ") bounds=(" + std::to_string(out.bounds_x) + "," +
            std::to_string(out.bounds_y) + " " +
            std::to_string(out.bounds_w) + "x" +
            std::to_string(out.bounds_h) + ") start=(" +
            std::to_string(out.start_x) + "," +
            std::to_string(out.start_y) + ") style='" +
            detail::attr_string(elem, "style") + "'");
        return true;
    }
    return false;
}

namespace {

// Clamped visual position for the dragged float at the given pointer pos.
Point float_drag_pos(const detail::DocumentImpl::FloatDrag& d, const Event& ev) {
    int x = d.elem_doc_x + (ev.pos.x - d.start_x);
    int y = d.elem_doc_y + (ev.pos.y - d.start_y);
    if (d.bounds_w > 0 && d.bounds_h > 0) {
        x = std::clamp(x, d.bounds_x,
                       std::max(d.bounds_x, d.bounds_x + d.bounds_w - d.elem_w));
        y = std::clamp(y, d.bounds_y,
                       std::max(d.bounds_y, d.bounds_y + d.bounds_h - d.elem_h));
    }
    return {x, y};
}

}  // namespace

bool update_float_drag(detail::DocumentImpl& impl, const Event& ev) {
    // Compositor semantics: a move only advances the drag's visual position.
    // No style write, no restyle, no relayout — the dragged subtree paints
    // (and hit-tests) through the translation effective_transform_for injects
    // from cur_x/cur_y, and commit_float_drag writes the style once on
    // release. This is what keeps a float drag O(µs) per mouse move.
    auto& d = impl.float_drag;
    if (!d.elem) return false;
    const Point p = float_drag_pos(d, ev);
    if (p.x == d.cur_x && p.y == d.cur_y) return false;
    d.cur_x = p.x;
    d.cur_y = p.y;
    // Paint changed (the subtree's draw transform), layout did not. Without
    // this the renderer keeps re-presenting the cached display list and the
    // panel only visually moves on the release commit.
    impl.paint_dirty = true;
    if (detail::dock_trace_enabled()) {
        detail::dock_trace(
            "float-drag-move ev=(" + std::to_string(ev.pos.x) + "," +
            std::to_string(ev.pos.y) + ") -> doc=(" + std::to_string(p.x) +
            "," + std::to_string(p.y) + ")");
    }
    return true;
}

bool commit_float_drag(detail::DocumentImpl& impl, const Event& ev) {
    // One-shot on release: write the final inline left/top (relative to the
    // real containing block) so the moved position is part of the document.
    // This is the gesture's only restyle/relayout.
    auto& d = impl.float_drag;
    if (!d.elem) return false;
    const Point p = float_drag_pos(d, ev);
    if (detail::dock_trace_enabled()) {
        detail::dock_trace(
            "float-drag-commit doc=(" + std::to_string(p.x) + "," +
            std::to_string(p.y) + ") write left/top=(" +
            std::to_string(p.x - d.cb_x) + "," +
            std::to_string(p.y - d.cb_y) + ")");
    }
    return detail::set_attribute_on_element(
        impl, d.elem, "style",
        with_float_position(detail::attr_string(d.elem, "style"), p.x - d.cb_x,
                            p.y - d.cb_y));
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
Rect float_resize_rect(const detail::DocumentImpl::FloatResize& d,
                       const Event& ev) {
    constexpr int kMinW = 160;
    constexpr int kMinH = 80;
    const int dx = ev.pos.x - d.start_x;
    const int dy = ev.pos.y - d.start_y;
    int left = d.elem_doc_x;
    int top = d.elem_doc_y;
    int right = d.elem_doc_x + d.elem_w;
    int bottom = d.elem_doc_y + d.elem_h;
    if (d.dir & FloatResizeW) left += dx;
    if (d.dir & FloatResizeE) right += dx;
    if (d.dir & FloatResizeN) top += dy;
    if (d.dir & FloatResizeS) bottom += dy;

    if (right - left < kMinW) {
        if (d.dir & FloatResizeW) left = right - kMinW;
        else right = left + kMinW;
    }
    if (bottom - top < kMinH) {
        if (d.dir & FloatResizeN) top = bottom - kMinH;
        else bottom = top + kMinH;
    }

    if (d.bounds_w > 0 && d.bounds_h > 0) {
        const int max_right = d.bounds_x + d.bounds_w;
        const int max_bottom = d.bounds_y + d.bounds_h;
        if (d.dir & FloatResizeW) {
            left = std::clamp(left, d.bounds_x, std::max(d.bounds_x, right - kMinW));
        }
        if (d.dir & FloatResizeE) {
            right = std::clamp(right, left + kMinW,
                               std::max(left + kMinW, max_right));
        }
        if (d.dir & FloatResizeN) {
            top = std::clamp(top, d.bounds_y, std::max(d.bounds_y, bottom - kMinH));
        }
        if (d.dir & FloatResizeS) {
            bottom = std::clamp(bottom, top + kMinH,
                                std::max(top + kMinH, max_bottom));
        }
    }

    return {left, top, std::max(kMinW, right - left),
            std::max(kMinH, bottom - top)};
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool update_float_resize(detail::DocumentImpl& impl, const Event& ev) {
    auto& d = impl.float_resize;
    if (!d.elem) return false;
    const Rect r = detail::float_resize_rect(d, ev);
    return detail::set_attribute_on_element(
        impl, d.elem, "style",
        with_float_rect(detail::attr_string(d.elem, "style"), r.x - d.cb_x,
                        r.y - d.cb_y, r.w, r.h));
}
}  // namespace detail
namespace {

std::string tab_drag_ghost_style(Point p) {
    return "position:fixed;z-index:1000;pointer-events:none;left:" +
        std::to_string(p.x + 10) + "px;top:" + std::to_string(p.y + 8) +
        "px";
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool update_tab_drag_ghost(detail::DocumentImpl& impl,
                           std::string_view label_text,
                           Point p) {
    if (!impl.doc) return false;
    const std::string style = tab_drag_ghost_style(p);
    if (impl.tab_drag_ghost) {
        bool changed = detail::set_attribute_on_element(
            impl, impl.tab_drag_ghost, "style", style);
        changed = detail::remove_attribute_on_element(impl, impl.tab_drag_ghost,
                                              "hidden") || changed;
        return changed;
    }
    if (auto* existing = detail::find_dom_element_by_id(impl, "__dockghost")) {
        impl.tab_drag_ghost = existing;
        bool changed = detail::set_attribute_on_element(impl, existing, "style",
                                                style);
        changed = detail::remove_attribute_on_element(impl, existing, "hidden") ||
                  changed;
        return changed;
    }

    auto* body = lxb_html_document_body_element(impl.doc);
    if (!body) return false;
    auto* ghost = lxb_dom_document_create_element(
        lxb_dom_interface_document(impl.doc), detail::as_lxb("div"), 3, nullptr);
    if (!ghost) return false;
    constexpr std::string_view ghost_id = "__dockghost";
    constexpr std::string_view ghost_class = "dcs-dockpane__tab-ghost";
    lxb_dom_element_set_attribute(ghost, detail::as_lxb("id"), 2,
                                  detail::as_lxb(ghost_id), ghost_id.size());
    lxb_dom_element_set_attribute(ghost, detail::as_lxb("class"), 5,
                                  detail::as_lxb(ghost_class), ghost_class.size());
    lxb_dom_element_set_attribute(ghost, detail::as_lxb("style"), 5,
                                  detail::as_lxb(style), style.size());

    std::string label = std::string(detail::trim_css_ws(label_text));
    if (label.empty()) label = "Panel";
    lxb_dom_node_text_content_set(lxb_dom_interface_node(ghost),
                                  detail::as_lxb(label), label.size());
    lxb_dom_node_insert_child(lxb_dom_interface_node(body),
                              lxb_dom_interface_node(ghost));
    impl.tab_drag_ghost = ghost;
    detail::recollect_blocks_from_current_dom(impl);
    impl.paint_dirty = true;
    return true;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool remove_tab_drag_ghost(detail::DocumentImpl& impl) {
    auto* ghost = impl.tab_drag_ghost;
    if (!ghost) {
        ghost = detail::find_dom_element_by_id(impl, "__dockghost");
    }
    if (!ghost) return false;
    bool changed = detail::set_attribute_on_element(impl, ghost, "hidden", "");
    changed = detail::set_attribute_on_element(
                  impl, ghost, "style",
                  "position:fixed;z-index:1000;pointer-events:none;"
                  "display:none;left:0px;top:0px") ||
              changed;
    impl.paint_dirty = impl.paint_dirty || changed;
    return changed;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool is_dcs_menu_trigger(lxb_dom_element_t* elem) {
    return elem && detail::attr_string(elem, "data-dcs-toggle") == "menu" &&
           !detail::has_attr(elem, "disabled");
}
}  // namespace detail
namespace {


bool set_all_dcs_menu_triggers_expanded(detail::DocumentImpl& impl,
                                        std::string_view value) {
    std::vector<lxb_dom_element_t*> triggers;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (detail::is_dcs_menu_trigger(elem)) triggers.push_back(elem);
    };
    detail::walk_dom_elements(detail::document_dom_root(impl), collect);

    bool changed = false;
    for (auto* trigger : triggers) {
        // Only rewrite triggers whose state actually differs. In particular,
        // collapsing must NOT add aria-expanded="false" to a trigger that never
        // had the attribute: the outside-click "close everything" sweep runs on
        // a fresh view too, and a spurious attribute add restyles + dirties
        // layout — which used to swallow the very first click on any control
        // (found via the game-editor inspector "checkbox needs two clicks").
        const std::string current = detail::attr_string(trigger, "aria-expanded");
        if (current == value) continue;
        if (value == "false" && current.empty()) continue;
        changed =
            detail::set_attribute_on_element(impl, trigger, "aria-expanded", value) ||
            changed;
    }
    return changed;
}

bool close_dcs_menu(detail::DocumentImpl& impl, lxb_dom_element_t* menu) {
    if (!menu) return false;
    bool changed = detail::set_attribute_on_element(impl, menu, "hidden", "");
    changed = detail::remove_attribute_on_element(impl, menu, "style") || changed;
    return changed;
}

bool close_all_dcs_menus(detail::DocumentImpl& impl,
                         lxb_dom_element_t* except = nullptr) {
    std::vector<lxb_dom_element_t*> menus;
    auto collect = [&](lxb_dom_element_t* elem) {
        // A submenu cascade (`.dcs-menu.dcs-menu__sub`) is NOT a top-level
        // menu layer: it lives inside its parent menu and opens/closes purely
        // via CSS (`--has-sub:hover > __sub`). Stamping `hidden` on it here
        // (decius.js never does) leaves a half-dead panel the :hover rule can
        // no longer reveal properly. It disappears with its parent menu.
        if (elem != except && detail::class_list_contains(elem, "dcs-menu") &&
            !detail::class_list_contains(elem, "dcs-menu__sub") &&
            !detail::has_attr(elem, "hidden")) {
            menus.push_back(elem);
        }
    };
    detail::walk_dom_elements(detail::document_dom_root(impl), collect);

    bool changed = false;
    for (auto* menu : menus) {
        changed = close_dcs_menu(impl, menu) || changed;
    }
    changed = set_all_dcs_menu_triggers_expanded(impl, "false") || changed;
    return changed;
}

std::string dcs_menu_open_style(const detail::DocumentImpl& impl,
                                lxb_dom_element_t* trigger,
                                lxb_dom_element_t* menu) {
    Rect anchor_rect{0, 0, 80, 1};
    int overlay_width = detail::class_list_contains(menu, "aui-color-menu") ? 160 : 180;
    const bool stretch_to_anchor = detail::class_list_contains(menu, "aui-color-menu");
    const int trigger_idx =
        detail::block_index_for_element_or_ancestor(impl, trigger);
    if (trigger_idx >= 0) {
        anchor_rect = detail::block_border_visual_rect(impl, trigger_idx);
        if (stretch_to_anchor) {
            overlay_width = std::max(1, anchor_rect.w);
        }
    }
    const int natural_h = overlay_declared_outer_height(impl, menu, 160);
    const auto placed = place_anchored_overlay(
        impl, anchor_rect, overlay_width, natural_h, "bottom", 0);
    std::string style = "display:flex;position:fixed;left:" +
        std::to_string(placed.left) + "px;top:" +
        std::to_string(placed.top) +
        "px;flex-direction:column;align-items:stretch;z-index:400";
    // Mirror decius.js place(): the menu becomes a scroll container ONLY
    // when it is genuinely taller than the available space. Unconditional
    // overflow clipping beheads submenu cascades, which live outside the
    // panel box (`.dcs-menu__sub` at left:100%) by design.
    if (natural_h > placed.max_height) {
        style += ";max-height:" + std::to_string(placed.max_height) +
                 "px;overflow:auto";
    }
    if (stretch_to_anchor) {
        style += ";width:" + std::to_string(overlay_width) +
                 "px;min-width:" + std::to_string(overlay_width) +
                 "px;max-width:" + std::to_string(overlay_width) + "px";
    }
    return style;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool toggle_dcs_menu(detail::DocumentImpl& impl,
                     lxb_dom_element_t* trigger,
                     lxb_dom_element_t* menu) {
    if (!trigger || !menu) return false;
    if (!detail::has_attr(menu, "hidden")) {
        bool changed = close_dcs_menu(impl, menu);
        changed =
            detail::set_attribute_on_element(impl, trigger, "aria-expanded", "false") ||
            changed;
        return changed;
    }

    const std::string open_style = dcs_menu_open_style(impl, trigger, menu);
    bool changed = detail::close_transient_layers(impl, menu);
    changed = detail::remove_attribute_on_element(impl, menu, "hidden") || changed;
    changed =
        detail::set_attribute_on_element(impl, menu, "style", open_style) ||
        changed;
    changed =
        detail::set_attribute_on_element(impl, trigger, "aria-expanded", "true") ||
        changed;
    return changed;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool is_dcs_popover_trigger(lxb_dom_element_t* elem) {
    return elem && detail::attr_string(elem, "data-dcs-toggle") == "popover" &&
           !detail::has_attr(elem, "disabled");
}
}  // namespace detail
namespace {


bool set_all_dcs_popover_triggers_expanded(detail::DocumentImpl& impl,
                                           std::string_view value) {
    std::vector<lxb_dom_element_t*> triggers;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (detail::is_dcs_popover_trigger(elem)) triggers.push_back(elem);
    };
    detail::walk_dom_elements(detail::document_dom_root(impl), collect);

    bool changed = false;
    for (auto* trigger : triggers) {
        // Same no-spurious-write rule as the menu sweep: never ADD
        // aria-expanded="false" to a virgin trigger (e.g. every colorfield
        // caret on a freshly built view) — the mutation dirties layout and
        // used to swallow the first click on unrelated controls.
        const std::string current = detail::attr_string(trigger, "aria-expanded");
        if (current == value) continue;
        if (value == "false" && current.empty()) continue;
        changed =
            detail::set_attribute_on_element(impl, trigger, "aria-expanded", value) ||
            changed;
    }
    return changed;
}

bool close_dcs_popover(detail::DocumentImpl& impl, lxb_dom_element_t* popover) {
    if (!popover) return false;
    bool changed = detail::set_attribute_on_element(impl, popover, "hidden", "");
    if (detail::has_attr(popover, "data-dcs-base-style")) {
        const std::string base = detail::attr_string(popover, "data-dcs-base-style");
        changed = base.empty()
            ? (detail::remove_attribute_on_element(impl, popover, "style") || changed)
            : (detail::set_attribute_on_element(impl, popover, "style", base) ||
               changed);
        changed =
            detail::remove_attribute_on_element(impl, popover, "data-dcs-base-style") ||
            changed;
    } else {
        changed = detail::remove_attribute_on_element(impl, popover, "style") ||
                  changed;
    }
    return changed;
}

bool close_all_dcs_popovers(detail::DocumentImpl& impl,
                            lxb_dom_element_t* except = nullptr) {
    std::vector<lxb_dom_element_t*> popovers;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (elem != except && detail::class_list_contains(elem, "dcs-popover") &&
            !detail::has_attr(elem, "hidden")) {
            popovers.push_back(elem);
        }
    };
    detail::walk_dom_elements(detail::document_dom_root(impl), collect);

    bool changed = false;
    for (auto* popover : popovers) {
        changed = close_dcs_popover(impl, popover) || changed;
    }
    changed = set_all_dcs_popover_triggers_expanded(impl, "false") || changed;
    return changed;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool close_transient_layers(detail::DocumentImpl& impl,
                            lxb_dom_element_t* except) {
    const bool dropdowns = close_all_dropdown_menus(impl, except);
    const bool menus = close_all_dcs_menus(impl, except);
    const bool popovers = close_all_dcs_popovers(impl, except);
    return dropdowns || menus || popovers;
}
}  // namespace detail
namespace {

std::string dcs_popover_open_style(const detail::DocumentImpl& impl,
                                   lxb_dom_element_t* trigger,
                                   lxb_dom_element_t* popover) {
    int pop_w = overlay_declared_outer_width(impl, popover, 220);
    int pop_h = 64;
    const int popover_idx = detail::block_index_for_exact_element(impl, popover);
    if (popover_idx >= 0) {
        const Rect pop_rect = detail::block_border_visual_rect(impl, popover_idx);
        if (pop_rect.w > 0) pop_w = pop_rect.w;
        if (pop_rect.h > 0) pop_h = pop_rect.h;
    }

    Rect anchor_rect{0, 0, 1, 1};
    lxb_dom_element_t* anchor_elem = trigger;
    if (auto* field = detail::nearest_ancestor_with_class(trigger, "dcs-colorfield")) {
        anchor_elem = field;
    }
    const int trigger_idx =
        detail::block_index_for_element_or_ancestor(impl, anchor_elem);
    if (trigger_idx >= 0) {
        anchor_rect = detail::block_border_visual_rect(impl, trigger_idx);
        if (detail::class_list_contains(anchor_elem, "dcs-colorfield") &&
            anchor_rect.w > pop_w) {
            pop_w = anchor_rect.w;
        }
    }

    std::string placement = detail::attr_string(trigger, "data-dcs-placement");
    if (placement.empty()) placement = "bottom";
    const auto placed = place_anchored_overlay(
        impl, anchor_rect, pop_w,
        overlay_declared_outer_height(impl, popover, pop_h), placement, 6);
    const std::string width = std::to_string(pop_w) + "px";
    return detail::style_with_properties(
        detail::attr_string(popover, "style"),
        {{"display", "flex"},
         {"position", "fixed"},
         {"left", std::to_string(placed.left) + "px"},
         {"top", std::to_string(placed.top) + "px"},
         {"width", width},
         {"max-height", std::to_string(placed.max_height) + "px"},
         {"overflow", "auto"},
         {"z-index", "400"}});
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool toggle_dcs_popover(detail::DocumentImpl& impl,
                        lxb_dom_element_t* trigger,
                        lxb_dom_element_t* popover) {
    if (!trigger || !popover) return false;
    if (!detail::has_attr(popover, "hidden")) {
        bool changed = close_dcs_popover(impl, popover);
        changed =
            detail::set_attribute_on_element(impl, trigger, "aria-expanded", "false") ||
            changed;
        return changed;
    }

    if (auto* field = detail::nearest_ancestor_with_class(trigger, "dcs-colorfield")) {
        detail::sync_dcs_colorfield(impl, field, detail::current_dcs_colorfield_hsv(field),
                            /*emit=*/false);
    }
    const std::string base_style = detail::attr_string(popover, "style");
    const std::string open_style =
        dcs_popover_open_style(impl, trigger, popover);
    bool changed = detail::close_transient_layers(impl, popover);
    changed = detail::set_attribute_on_element(impl, popover, "data-dcs-base-style",
                                       base_style) ||
              changed;
    changed = detail::remove_attribute_on_element(impl, popover, "hidden") || changed;
    changed =
        detail::set_attribute_on_element(impl, popover, "style", open_style) ||
        changed;
    changed =
        detail::set_attribute_on_element(impl, trigger, "aria-expanded", "true") ||
        changed;
    return changed;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool find_dcs_menu_trigger_at(detail::DocumentImpl& impl,
                              int from_idx,
                              lxb_dom_element_t*& out_trigger,
                              lxb_dom_element_t*& out_menu) {
    out_trigger = nullptr;
    out_menu = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (!detail::is_dcs_menu_trigger(elem)) continue;
        auto* menu = dcs_target_for_trigger(impl, elem);
        if (!menu || !detail::class_list_contains(menu, "dcs-menu")) continue;
        out_trigger = elem;
        out_menu = menu;
        return true;
    }
    return false;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool find_dcs_popover_trigger_at(detail::DocumentImpl& impl,
                                 int from_idx,
                                 lxb_dom_element_t*& out_trigger,
                                 lxb_dom_element_t*& out_popover) {
    out_trigger = nullptr;
    out_popover = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (!detail::is_dcs_popover_trigger(elem)) continue;
        auto* popover = dcs_target_for_trigger(impl, elem);
        if (!popover || !detail::class_list_contains(popover, "dcs-popover")) continue;
        out_trigger = elem;
        out_popover = popover;
        return true;
    }
    return false;
}
}  // namespace detail
namespace {

bool is_disabled_dcs_menu_item(lxb_dom_element_t* elem) {
    return detail::has_attr(elem, "disabled") ||
           detail::attr_string(elem, "aria-disabled") == "true" ||
           detail::class_list_contains(elem, "dcs-menu__item--disabled");
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool find_dcs_menu_item_at(detail::DocumentImpl& impl,
                           int from_idx,
                           lxb_dom_element_t*& out_menu,
                           lxb_dom_element_t*& out_item) {
    out_menu = nullptr;
    out_item = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        if (!out_item && detail::class_list_contains(elem, "dcs-menu__item")) {
            if (is_disabled_dcs_menu_item(elem)) return false;
            out_item = elem;
        }
        if (out_item && detail::class_list_contains(elem, "dcs-menu")) {
            out_menu = elem;
            return true;
        }
    }
    out_item = nullptr;
    return false;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool clear_pressed_dcs_menu_item(detail::DocumentImpl& impl) {
    auto* item = impl.pressed_dcs_menu_item;
    const bool was_active = impl.pressed_dcs_menu_item_was_active;
    impl.pressed_dcs_menu_item = nullptr;
    impl.pressed_dcs_menu_item_was_active = false;
    impl.pressed_dcs_menu_item_bounds = {};
    if (!item || was_active) return false;
    return detail::set_element_class(impl, item, "dcs-menu__item--active", false);
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool press_dcs_menu_item(detail::DocumentImpl& impl,
                         lxb_dom_element_t* item) {
    bool changed = detail::clear_pressed_dcs_menu_item(impl);
    if (!item) return changed;
    impl.pressed_dcs_menu_item = item;
    impl.pressed_dcs_menu_item_was_active =
        detail::class_list_contains(item, "dcs-menu__item--active");
    if (const int idx = detail::block_index_for_exact_element(impl, item); idx >= 0) {
        impl.pressed_dcs_menu_item_bounds =
            impl.blocks[static_cast<std::size_t>(idx)].bounds;
    } else {
        impl.pressed_dcs_menu_item_bounds = {};
    }
    changed = detail::set_element_class(impl, item, "dcs-menu__item--active", true) ||
              changed;
    return changed;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool activate_dcs_menu_item(detail::DocumentImpl& impl,
                            lxb_dom_element_t* menu,
                            lxb_dom_element_t* item) {
    if (!menu || !item) return false;
    // decius.js wires its click listener on every .dcs-menu, so a click on a
    // cascade leaf bubbles up and the ROOT menu's listener runs closeMenu(m):
    // the whole tree dismisses, not just the sub panel (cascades are
    // hover-CSS inside the now-hidden subtree). Resolve to the outermost
    // menu so select/close target the root exactly like the reference.
    for (auto* n = lxb_dom_node_parent(lxb_dom_interface_node(menu)); n;
         n = lxb_dom_node_parent(n)) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* el = lxb_dom_interface_element(n);
        if (detail::class_list_contains(el, "dcs-menu")) menu = el;
    }
    if (detail::has_attr(menu, "data-aui-colorfield") &&
        detail::has_attr(item, "data-dcs-value")) {
        const auto value = detail::attr_string(item, "data-dcs-value");
        auto* colorfield =
            detail::find_dom_element_by_id(impl, detail::attr_string(menu, "data-aui-colorfield"));
        bool changed = false;
        if (colorfield) {
            changed =
                detail::set_attribute_on_element(impl, colorfield, "data-value", value) ||
                changed;
            if (auto* chip =
                    detail::first_descendant_with_class(colorfield,
                                                "dcs-colorfield__chip")) {
                changed = detail::set_attribute_on_element(
                    impl, chip, "style",
                    "--c:" + value + ";background:" + value) || changed;
            }
            if (auto* hex =
                    detail::first_descendant_with_class(colorfield,
                                                "dcs-colorfield__hex")) {
                changed = detail::set_text_on_element(impl, hex, value) || changed;
            }
            detail::emit_widget_change(impl, colorfield, value);
        }
        changed = close_dcs_menu(impl, menu) || changed;
        changed = set_all_dcs_menu_triggers_expanded(impl, "false") || changed;
        return changed;
    }
    if (detail::has_attr(item, "data-dcs-value")) {
        detail::emit_widget_change(impl, menu, detail::attr_string(item, "data-dcs-value"));
    }
    if (detail::class_list_contains(item, "dcs-menu__item--has-sub")) return false;

    // A leaf item: fire the app's on_click (its activation name) before the
    // menu closes, so menu actions route like button activations.
    if (auto name = activation_name(item); !name.empty()) {
        impl.activated_widgets.push_back(std::move(name));
    }

    bool changed = close_dcs_menu(impl, menu);
    changed = set_all_dcs_menu_triggers_expanded(impl, "false") || changed;
    return changed;
}
}  // namespace detail
namespace {

bool is_dcs_select_row(lxb_dom_element_t* elem) {
    return elem && (detail::class_list_contains(elem, "dcs-list__item") ||
                    detail::class_list_contains(elem, "dcs-tree__row"));
}

bool is_disabled_dcs_select_row(lxb_dom_element_t* elem) {
    return detail::has_attr(elem, "disabled") ||
           detail::attr_string(elem, "aria-disabled") == "true" ||
           detail::class_list_contains(elem, "dcs-list__item--disabled") ||
           detail::class_list_contains(elem, "dcs-tree__row--disabled");
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool find_dcs_select_row_at(detail::DocumentImpl& impl,
                            int from_idx,
                            lxb_dom_element_t*& out_box,
                            lxb_dom_element_t*& out_row) {
    out_box = nullptr;
    out_row = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        if (!out_row && is_dcs_select_row(elem)) {
            if (is_disabled_dcs_select_row(elem)) return false;
            out_row = elem;
        }
        if (out_row && detail::has_attr(elem, "data-dcs-select")) {
            out_box = elem;
            return true;
        }
    }
    out_row = nullptr;
    return false;
}
}  // namespace detail
namespace {

void collect_dcs_select_rows(lxb_dom_element_t* elem,
                             std::vector<lxb_dom_element_t*>& rows) {
    if (!elem) return;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* child_elem = lxb_dom_interface_element(child);
        if (is_dcs_select_row(child_elem) &&
            !is_disabled_dcs_select_row(child_elem)) {
            rows.push_back(child_elem);
        }
        collect_dcs_select_rows(child_elem, rows);
    }
}

bool set_dcs_row_selected(detail::DocumentImpl& impl,
                          lxb_dom_element_t* row,
                          bool selected) {
    return detail::set_attribute_on_element(
        impl, row, "aria-selected", selected ? "true" : "false");
}

std::string dcs_selected_rows_value(
    const std::vector<lxb_dom_element_t*>& rows) {
    std::string out;
    for (auto* row : rows) {
        if (detail::attr_string(row, "aria-selected") != "true") continue;
        std::string value = detail::attr_string(row, "data-dcs-value");
        if (value.empty()) value = detail::attr_string(row, "id");
        if (value.empty()) continue;
        if (!out.empty()) out.push_back(',');
        out += value;
    }
    return out;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool update_dcs_select_control(detail::DocumentImpl& impl,
                               lxb_dom_element_t* box,
                               lxb_dom_element_t* row,
                               const Event& ev) {
    if (!box || !row) return false;
    std::vector<lxb_dom_element_t*> rows;
    collect_dcs_select_rows(box, rows);
    if (rows.empty()) return false;

    const auto row_it = std::find(rows.begin(), rows.end(), row);
    if (row_it == rows.end()) return false;

    const bool multi = detail::attr_string(box, "data-dcs-select") == "multi";
    bool changed = false;
    if (multi && (ev.ctrl || ev.super)) {
        const bool selected = detail::attr_string(row, "aria-selected") == "true";
        changed = set_dcs_row_selected(impl, row, !selected) || changed;
        impl.dcs_select_anchors[lxb_dom_interface_node(box)] =
            lxb_dom_interface_node(row);
    } else if (multi && ev.shift) {
        auto* anchor_node = [&]() -> lxb_dom_node_t* {
            const auto it =
                impl.dcs_select_anchors.find(lxb_dom_interface_node(box));
            return it == impl.dcs_select_anchors.end() ? nullptr : it->second;
        }();
        auto* anchor = anchor_node && anchor_node->type == LXB_DOM_NODE_TYPE_ELEMENT
            ? lxb_dom_interface_element(anchor_node)
            : nullptr;
        const auto anchor_it = anchor
            ? std::find(rows.begin(), rows.end(), anchor)
            : rows.end();
        if (anchor_it != rows.end()) {
            const auto a = static_cast<std::size_t>(anchor_it - rows.begin());
            const auto b = static_cast<std::size_t>(row_it - rows.begin());
            const auto lo = std::min(a, b);
            const auto hi = std::max(a, b);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                changed =
                    set_dcs_row_selected(impl, rows[i], i >= lo && i <= hi) ||
                    changed;
            }
        } else {
            for (auto* r : rows) {
                changed = set_dcs_row_selected(impl, r, r == row) || changed;
            }
            impl.dcs_select_anchors[lxb_dom_interface_node(box)] =
                lxb_dom_interface_node(row);
        }
    } else {
        for (auto* r : rows) {
            changed = set_dcs_row_selected(impl, r, r == row) || changed;
        }
        impl.dcs_select_anchors[lxb_dom_interface_node(box)] =
            lxb_dom_interface_node(row);
    }

    detail::emit_widget_change(impl, box, dcs_selected_rows_value(rows));
    return changed;
}
}  // namespace detail
namespace {

bool find_decius_collapse_at(detail::DocumentImpl& impl,
                             int from_idx,
                             lxb_dom_element_t*& out_block,
                             lxb_dom_element_t*& out_chevron,
                             std::string_view& out_collapsed_class,
                             std::string_view& out_chevron_open_class) {
    out_block = nullptr;
    out_chevron = nullptr;
    out_collapsed_class = {};
    out_chevron_open_class = {};
    // Walk up from the hit block. A click commonly lands on a text/inline block
    // (e.g. the foldout title's own text run) whose element_for_block is null —
    // the loop skips those via `if (!elem) continue` and keeps climbing, exactly
    // like the tree-chevron matcher. (A previous early `if (!hit) return false`
    // here bailed on such clicks, so clicking a foldout's title text never
    // toggled it — only clicking the chevron/padding did.)
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        if (detail::class_list_contains(elem, "dcs-subpanel__close") ||
            detail::class_list_contains(elem, "dcs-foldout__tools")) {
            return false;
        }
        if (detail::class_list_contains(elem, "dcs-subpanel__header")) {
            auto* block = detail::nearest_ancestor_with_class(elem, "dcs-subpanel");
            if (!block) return false;
            out_block = block;
            out_chevron =
                detail::first_descendant_with_class(block, "dcs-subpanel__chevron");
            out_collapsed_class = "dcs-subpanel--collapsed";
            out_chevron_open_class = "dcs-subpanel__chevron--open";
            return true;
        }
        if (detail::class_list_contains(elem, "dcs-foldout__header")) {
            auto* block = detail::nearest_ancestor_with_class(elem, "dcs-foldout");
            if (!block) return false;
            out_block = block;
            out_chevron =
                detail::first_descendant_with_class(block, "dcs-foldout__chevron");
            out_collapsed_class = "dcs-foldout--collapsed";
            out_chevron_open_class = "dcs-foldout__chevron--open";
            return true;
        }
    }
    return false;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool toggle_decius_collapse_control(detail::DocumentImpl& impl, int from_idx) {
    lxb_dom_element_t* block = nullptr;
    lxb_dom_element_t* chevron = nullptr;
    std::string_view collapsed_class;
    std::string_view chevron_open_class;
    if (!find_decius_collapse_at(impl, from_idx, block, chevron,
                                 collapsed_class, chevron_open_class)) {
        return false;
    }

    const bool collapsed = detail::class_list_contains(block, collapsed_class);
    const bool next_collapsed = !collapsed;
    bool changed = detail::set_attribute_on_element(
        impl, block, "class",
        detail::class_list_set(block, collapsed_class, next_collapsed));
    if (chevron) {
        changed = detail::set_attribute_on_element(
            impl, chevron, "class",
            detail::class_list_set(chevron, chevron_open_class, !next_collapsed)) ||
                  changed;
    }
    // Flipping the collapsed class on the block is all decius does — the rule
    // `.dcs-foldout--collapsed > .dcs-foldout__body{display:none}` hides the
    // body via the cascade. detail::set_attribute_on_element() restyles the subtree, so
    // that descendant rule must re-match the body. (If it doesn't, that is a
    // renderer cascade bug to fix in the renderer, not to paper over here.)
    detail::emit_widget_change(impl, block, next_collapsed ? "closed" : "open");
    return changed;
}

// ── Dock-pane tab switch (.dcs-dockpane__tab[data-dcs-target]) ───────────────
// Clicking a pane tab activates its target body and deactivates the pane's
// other tabs/bodies — decius semantics: aria-selected on the tab, `hidden` on
// the inactive bodies. Toggling `hidden` recollects the box tree (see
// set_attribute_on_element), so a revealed body's content reappears.
bool find_dockpane_tab_at(detail::DocumentImpl& impl, int from_idx,
                          lxb_dom_element_t*& out_tab) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (elem && detail::class_list_contains(elem, "dcs-dockpane__tab")) {
            out_tab = elem;
            return true;
        }
    }
    return false;
}
}  // namespace detail

// Forward declarations of detail:: helpers defined later in this TU.
// Must sit at namespace scope, not inside the anonymous namespace
// below (Clang / GCC reject; MSVC accepts silently).
namespace detail {
std::string pane_panel_id(lxb_dom_element_t* pane);
std::string dockpane_tab_panel_id(lxb_dom_element_t* tab);
}  // namespace detail

namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool switch_dockpane_tab(detail::DocumentImpl& impl, lxb_dom_element_t* tab) {
    if (!tab) return false;
    auto* target = dcs_target_for_trigger(impl, tab);  // the body to reveal
    if (!target) return false;
    // No-op if this tab is already the selected one (avoids churn when a future
    // drag begins on the active tab).
    if (detail::attr_string(tab, "aria-selected") == "true" && !detail::has_attr(target, "hidden"))
        return false;

    // The tab's parent is the .dcs-dockpane__tabs container; the pane is the
    // nearest .dcs-dockpane ancestor.
    auto* tabs_node = lxb_dom_node_parent(lxb_dom_interface_node(tab));
    if (!tabs_node) return false;
    lxb_dom_element_t* pane = nullptr;
    for (auto* n = tabs_node; n; n = lxb_dom_node_parent(n)) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(n);
        if (detail::class_list_contains(e, "dcs-dockpane")) {
            pane = e;
            break;
        }
    }
    if (!pane) return false;

    const std::string pane_id = detail::pane_panel_id(pane);
    const std::string active_id = detail::dockpane_tab_panel_id(tab);

    bool changed = false;
    // Activate the clicked tab; deactivate its siblings (the tabs container's
    // direct-child tabs).
    for (auto* c = lxb_dom_node_first_child(tabs_node); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (!detail::class_list_contains(e, "dcs-dockpane__tab")) continue;
        changed = detail::set_attribute_on_element(impl, e, "aria-selected",
                                           e == tab ? "true" : "false") ||
                  changed;
    }
    // Reveal the target tabpanel; hide its sibling tabpanels (decius
    // activateTabInDock: `:scope > [data-dcs-tabpanel]` of the target's parent
    // — the pane's single .dcs-dockpane__body).
    if (auto* body = lxb_dom_node_parent(lxb_dom_interface_node(target))) {
        for (auto* c = lxb_dom_node_first_child(body); c;
             c = lxb_dom_node_next(c)) {
            if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            auto* e = lxb_dom_interface_element(c);
            if (!detail::has_attr(e, "data-dcs-tabpanel")) continue;
            changed = (e == target
                           ? detail::remove_attribute_on_element(impl, e, "hidden")
                           : detail::set_attribute_on_element(impl, e, "hidden", "")) ||
                      changed;
        }
    }
    // Sync tab toolbars: show the toolbar bound to the activated tab, hide the
    // others (decius syncTabToolbars over [data-dcs-tabtoolbar]).
    {
        const std::string sel = detail::attr_string(tab, "data-dcs-target");
        std::vector<lxb_dom_element_t*> toolbars;
        auto collect = [&](lxb_dom_element_t* e) {
            if (detail::has_attr(e, "data-dcs-tabtoolbar")) toolbars.push_back(e);
        };
        detail::walk_dom_elements(lxb_dom_interface_node(pane), collect);
        for (auto* tb : toolbars) {
            const bool match = detail::attr_string(tb, "data-dcs-tabtoolbar") == sel;
            changed = (match ? detail::remove_attribute_on_element(impl, tb, "hidden")
                             : detail::set_attribute_on_element(impl, tb, "hidden", "")) ||
                      changed;
        }
    }
    if (!pane_id.empty() && !active_id.empty()) {
        if (active_id == pane_id) impl.dock_active_tabs.erase(pane_id);
        else impl.dock_active_tabs[pane_id] = active_id;
        detail::dock_trace("active-tab pane=" + pane_id + " active=" + active_id);
        detail::dock_trace_state(impl, "after-active-tab");
    }
    detail::emit_widget_change(impl, tab, "tab");
    return changed;
}

// Nearest ancestor (inclusive) carrying a class.
lxb_dom_element_t* ancestor_with_class(lxb_dom_element_t* e,
                                       std::string_view cls) {
    for (auto* n = e ? lxb_dom_interface_node(e) : nullptr; n;
         n = lxb_dom_node_parent(n)) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* el = lxb_dom_interface_element(n);
        if (detail::class_list_contains(el, cls)) return el;
    }
    return nullptr;
}

// Menubar hover-follow (decius.js wires this on trigger mouseenter): once
// one menubar menu is open, sliding along the bar switches menus without
// another click — native menubar / VS Code behavior. Scoped to triggers
// inside the SAME .dcs-menubar so two unrelated dropdown buttons never
// hover-steal each other's open menu.
bool hover_switch_dcs_menubar_menu(detail::DocumentImpl& impl,
                                   int hovered_idx) {
    static const bool trace = std::getenv("AFFINEUI_MENU_TRACE") != nullptr;
    lxb_dom_element_t* trigger = nullptr;
    lxb_dom_element_t* menu = nullptr;
    if (!detail::find_dcs_menu_trigger_at(impl, hovered_idx, trigger, menu)) {
        if (trace) std::fprintf(stderr, "[menu] hover-switch: no trigger at %d\n", hovered_idx);
        return false;
    }
    if (!detail::has_attr(menu, "hidden")) return false;  // ours is already open
    auto* bar = detail::ancestor_with_class(trigger, "dcs-menubar");
    if (!bar) {
        if (trace) std::fprintf(stderr, "[menu] hover-switch: trigger not in menubar\n");
        return false;
    }

    lxb_dom_element_t* open_trigger = nullptr;
    lxb_dom_element_t* open_menu = nullptr;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (open_trigger || elem == trigger) return;
        if (!detail::is_dcs_menu_trigger(elem)) return;
        const auto expanded = detail::attr_string(elem, "aria-expanded");
        auto* m = dcs_target_for_trigger(impl, elem);
        if (trace) {
            std::fprintf(stderr,
                         "[menu] hover-switch: sibling trigger id='%s' "
                         "expanded='%s' menu=%p hidden=%d\n",
                         detail::attr_string(elem, "id").c_str(), expanded.c_str(),
                         static_cast<void*>(m),
                         m ? detail::has_attr(m, "hidden") : -1);
        }
        if (expanded != "true") return;
        if (!m || !detail::class_list_contains(m, "dcs-menu") ||
            detail::has_attr(m, "hidden")) {
            return;
        }
        open_trigger = elem;
        open_menu = m;
    };
    detail::walk_dom_elements(lxb_dom_interface_node(bar), collect);
    if (!open_trigger) return false;

    // Let toggle_dcs_menu do the whole switch: it computes the new menu's
    // anchored placement from the CURRENT layout first, then its
    // close_transient_layers sweep retires the old menu (and resets every
    // trigger's aria-expanded) before opening ours. Closing the old menu
    // ourselves first would set `hidden` — a box-tree recollect — and the
    // placement math would then read zeroed trigger bounds, dropping the
    // switched menu at the window's top-left.
    return detail::toggle_dcs_menu(impl, trigger, menu);
}

// The dockpanel id behind a tab (its data-dcs-target is "#<id>-body").
std::string dockpane_tab_panel_id(lxb_dom_element_t* tab) {
    std::string sel = detail::attr_string(tab, "data-dcs-target");
    if (!sel.empty() && sel.front() == '#') sel.erase(0, 1);
    static constexpr std::string_view kSuffix = "-body";
    if (sel.size() > kSuffix.size() &&
        sel.compare(sel.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0)
        sel.erase(sel.size() - kSuffix.size());
    return sel;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
int positive_int_attr(lxb_dom_element_t* elem, std::string_view name,
                      int fallback) {
    if (!elem) return fallback;
    const std::string value = detail::attr_string(elem, name);
    if (value.empty()) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || parsed <= 0) return fallback;
    return static_cast<int>(std::min<long>(parsed, 10000));
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
int int_attr(lxb_dom_element_t* elem, std::string_view name, int fallback) {
    if (!elem) return fallback;
    const std::string value = detail::attr_string(elem, name);
    if (value.empty()) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str()) return fallback;
    return static_cast<int>(
        std::clamp<long>(parsed, std::numeric_limits<int>::min(),
                         std::numeric_limits<int>::max()));
}
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
lxb_dom_element_t* find_dockpane_tab_for_panel_id(detail::DocumentImpl& impl,
                                                  std::string_view panel_id) {
    if (panel_id.empty()) return nullptr;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* elem = detail::element_for_block(impl, i);
        if (!elem || !detail::class_list_contains(elem, "dcs-dockpane__tab")) {
            continue;
        }
        const std::string tab_panel_id = detail::dockpane_tab_panel_id(elem);
        if (std::string_view(tab_panel_id) == panel_id) return elem;
    }
    return nullptr;
}
}  // namespace detail
}  // namespace affineui
