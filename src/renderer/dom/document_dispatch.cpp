// document_dispatch.cpp — part of the AffineUI HTML5 renderer's document implementation.
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

DispatchResult Document::dispatch(const Event& ev) {
    DispatchResult result{};
    auto ensure_interaction_layout = [&]() {
#if !defined(AFFINEUI_STUB_BUILD)
        // Relayout with the last-known metrics whenever a mutation dirtied the
        // block tree. A null measurer is fine — painterless layout estimates
        // glyph metrics — so interaction code always sees current geometry
        // (headless apps included; stale blocks read as swallowed clicks,
        // flickering drop cursors, and drops that land nowhere).
        if (impl_->content_size.width == 0 &&
            impl_->media_viewport_width_px > 0) {
            layout(impl_->media_viewport_width_px,
                   impl_->media_viewport_height_px, impl_->last_measurer);
        }
#endif
    };
    // A pseudo-state change (:hover/:active) revealed a display:none subtree
    // that has no boxes (`.item:hover > .sub{display:block}` submenus).
    // Recollect (state bits survive it), relayout, and rebuild the chains
    // against fresh block indices so the revealed subtree is hit-testable in
    // THIS dispatch, not a frame later.
    auto handle_pseudo_reveal = [&](bool needs_recollect) {
#if !defined(AFFINEUI_STUB_BUILD)
        if (!needs_recollect) return;
        auto* active_elem = detail::element_for_block(*impl_, impl_->active_idx);
        detail::recollect_blocks_from_current_dom(*impl_);
        ensure_interaction_layout();
        impl_->hovered_idx = detail::hit_test_blocks(*impl_, impl_->last_mouse_pos.x,
                                             impl_->last_mouse_pos.y);
        impl_->active_idx = active_elem
            ? detail::block_index_for_exact_element(*impl_, active_elem)
            : -1;
        detail::refresh_hover_chain(*impl_);
        detail::refresh_active_chain(*impl_);
        result.redraw_requested = true;
#else
        (void) needs_recollect;
#endif
    };
    switch (ev.type) {
        case EventType::MouseMove: {
            impl_->last_mouse_pos = ev.pos;
            // CAPTURED gestures run before the ensure_interaction_layout
            // below: they use only state cached at mousedown (drag block idx,
            // start sizes, budget) and never hit-test, so they must not pay
            // for a synchronous relayout. This matters at mouse-poll rate — a
            // splitter move dirties layout, and re-laying-out on the NEXT
            // move ran a full Yoga pass per event (up to 1kHz on gaming
            // mice) instead of once per rendered frame.
            if (impl_->scrollbar_drag.block_idx >= 0) {
                if (detail::scrollbar_scroll_from_thumb_y(
                        *impl_,
                        impl_->scrollbar_drag.block_idx,
                        ev.pos.y - impl_->scrollbar_drag.thumb_offset_y)) {
                    result.redraw_requested = true;
                }
                break;
            }
#if !defined(AFFINEUI_STUB_BUILD)
            if (impl_->splitter_drag.block_idx >= 0) {
                if (detail::update_splitter_drag(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                break;
            }
            if (impl_->float_resize.elem) {
                if (detail::update_float_resize(*impl_, ev)) {
                    impl_->content_size = Size{0, 0};
                    result.redraw_requested = true;
                }
                break;
            }
            if (impl_->float_drag.elem) {
                if (detail::update_float_drag(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                break;
            }
            // A prior dispatch may have mutated the DOM (drop-highlight class,
            // transient-layer close, ...) and dirtied layout without a frame
            // running since. Every pointer event below this line starts by
            // ensuring the block tree is current — hit tests and geometric
            // row/target lookups on a stale tree miss, which reads as
            // flickering drop cursors and swallowed clicks. No-op when the
            // tree is clean.
            ensure_interaction_layout();
            // A pressed tab becomes a drag once it moves past a small threshold;
            // while dragging, show the drop indicator for the hovered zone.
            if (!impl_->tab_drag.tab &&
                !impl_->pending_tab_press.panel_id.empty()) {
                const int dx = ev.pos.x - impl_->pending_tab_press.start_x;
                const int dy = ev.pos.y - impl_->pending_tab_press.start_y;
                if (dx * dx + dy * dy > 36) {
                    ensure_interaction_layout();
                    if (detail::arm_tab_drag_from_pending_press(
                            *impl_, impl_->pending_tab_press)) {
                        impl_->tab_drag.dragging = true;
                        detail::dock_trace("tab-drag-start panel=" +
                                   impl_->tab_drag.panel_id +
                                   " from=" +
                                   impl_->tab_drag.source_pane_id +
                                   " source-placement=" +
                                   detail::dock_placement_summary(
                                       impl_->tab_drag.source_placement) +
                                   " source-tabs=[" +
                                   detail::sorted_join(
                                       impl_->tab_drag.source_tab_ids, "|") +
                                   "] source-bounds=" +
                                   detail::dock_rect_summary(
                                       impl_->tab_drag.source_pane_bounds) +
                                   " at=(" + std::to_string(ev.pos.x) + "," +
                                   std::to_string(ev.pos.y) + ")");
                    }
                    impl_->pending_tab_press = {};
                }
            }
            if (impl_->tab_drag.tab) {
                if (!impl_->tab_drag.dragging) {
                    const int dx = ev.pos.x - impl_->tab_drag.start_x;
                    const int dy = ev.pos.y - impl_->tab_drag.start_y;
                    if (dx * dx + dy * dy > 36) {
                        impl_->tab_drag.dragging = true;
                        detail::dock_trace("tab-drag-start panel=" +
                                   impl_->tab_drag.panel_id +
                                   " from=" +
                                   impl_->tab_drag.source_pane_id +
                                   " source-placement=" +
                                   detail::dock_placement_summary(
                                       impl_->tab_drag.source_placement) +
                                   " source-tabs=[" +
                                   detail::sorted_join(
                                       impl_->tab_drag.source_tab_ids, "|") +
                                   "] source-bounds=" +
                                   detail::dock_rect_summary(
                                       impl_->tab_drag.source_pane_bounds) +
                                   " at=(" + std::to_string(ev.pos.x) + "," +
                                   std::to_string(ev.pos.y) + ")");
                    }
                }
                if (impl_->tab_drag.dragging) {
                    ensure_interaction_layout();
                    detail::dock_trace("drag-move at=(" + std::to_string(ev.pos.x) +
                               "," + std::to_string(ev.pos.y) + ")");
                    // Re-resolve the source pane each move (a reload during the
                    // drag invalidates captured element pointers).
                    auto* drag_src_tab = detail::find_dockpane_tab_for_panel_id(
                        *impl_, impl_->tab_drag.panel_id);
                    auto* drag_src_pane =
                        detail::ancestor_with_class(drag_src_tab, "dcs-dockpane");
                    const auto t = detail::compute_drop_target(
                        *impl_, ev.pos, impl_->tab_drag.drag_kind,
                        drag_src_pane);
                    const bool target_changed =
                        impl_->tab_drag.drop_valid != t.valid ||
                        impl_->tab_drag.drop_parent !=
                            (t.valid ? t.parent : std::string()) ||
                        impl_->tab_drag.drop_zone !=
                            (t.valid ? static_cast<int>(t.zone)
                                     : static_cast<int>(DropZone::None)) ||
                        impl_->tab_drag.drop_x != t.x ||
                        impl_->tab_drag.drop_y != t.y ||
                        impl_->tab_drag.drop_w != t.w ||
                        impl_->tab_drag.drop_h != t.h;
                    if (target_changed) {
                        const int hit =
                            detail::hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
                        detail::dock_trace("preview panel=" +
                                   impl_->tab_drag.panel_id +
                                   " at=(" + std::to_string(ev.pos.x) + "," +
                                   std::to_string(ev.pos.y) + ") " +
                                   detail::drop_target_summary(t) +
                                   " hit=" +
                                   detail::hit_chain_summary(*impl_, hit));
                    }
                    const bool indicator_was_visible =
                        impl_->tab_drag.drop_indicator_visible;
                    impl_->tab_drag.drop_valid = t.valid;
                    impl_->tab_drag.drop_parent = t.valid ? t.parent : std::string();
                    impl_->tab_drag.drop_zone =
                        t.valid ? static_cast<int>(t.zone)
                                : static_cast<int>(DropZone::None);
                    impl_->tab_drag.drop_x = t.x;
                    impl_->tab_drag.drop_y = t.y;
                    impl_->tab_drag.drop_w = t.w;
                    impl_->tab_drag.drop_h = t.h;
                    impl_->tab_drag.drop_indicator_visible = t.valid;
                    if (detail::set_drop_indicator(*impl_, t.valid ? &t : nullptr) ||
                        indicator_was_visible != t.valid) {
                        result.redraw_requested = true;
                    }
                    if (detail::update_tab_drag_ghost(
                            *impl_, impl_->tab_drag.label, ev.pos)) {
                        result.redraw_requested = true;
                    }
                }
            }
            if (impl_->ui_control_script_attached &&
                impl_->colorfield_drag.kind !=
                    detail::DocumentImpl::ColorfieldDrag::Kind::None) {
                result.defer_widget_changes = true;
                if (detail::update_dcs_colorfield_drag(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                break;
            }
            if (impl_->ui_control_script_attached && impl_->tree_drag.row) {
                if (detail::update_dcs_tree_drag(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                if (impl_->tree_drag.dragging) break;
            }
            if (impl_->ui_control_script_attached &&
                impl_->live_drag.kind != LiveControlKind::None) {
                if (detail::update_active_live_control(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                break;
            }
#endif
            if (impl_->text_selection_drag_idx >= 0) {
                if (detail::set_text_caret_from_point(
                        *impl_, impl_->text_selection_drag_idx, ev.pos,
                        true, impl_->text_selection_drag_anchor)) {
                    result.redraw_requested = true;
                }
                break;
            }
            int new_hover = detail::hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
            {
                // A scrollbar is browser UI, not content: hovering its
                // track/thumb must not :hover the row painted beneath it.
                int sb_idx = -1;
                ScrollbarGeometry sb{};
                if (detail::find_vertical_scrollbar_at(*impl_, ev.pos, sb_idx,
                                                       sb)) {
                    new_hover = -1;
                }
            }
            if (new_hover != impl_->hovered_idx) {
                impl_->hovered_idx      = new_hover;
                result.redraw_requested = true;
            }
            // Refresh :hover chain even when hovered_idx didn't change â€”
            // mouse may have moved within the same leaf block (no-op
            // here) or the tree may have churned underneath us (rare,
            // but cheap to verify).
            {
                bool reveal = false;
                if (detail::refresh_hover_chain(*impl_, &reveal)) {
                    result.redraw_requested = true;
                }
                handle_pseudo_reveal(reveal);
            }
            if (impl_->ui_control_script_attached &&
                detail::hover_switch_dcs_menubar_menu(*impl_, impl_->hovered_idx)) {
                // The switch mutated hidden/style attrs and dirtied
                // layout; resolve it in-dispatch so the very next hit
                // test and hover refresh see the newly opened panel.
                ensure_interaction_layout();
                impl_->hovered_idx =
                    detail::hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
                bool reveal = false;
                detail::refresh_hover_chain(*impl_, &reveal);
                handle_pseudo_reveal(reveal);
                result.redraw_requested = true;
            }
            break;
        }
        case EventType::MouseDown: {
            impl_->last_mouse_pos = ev.pos;
            ensure_interaction_layout();  // see MouseMove — never press on a
                                          // stale block tree
            impl_->hovered_idx    = detail::hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
            impl_->mouse_down_consumed_release = false;
            impl_->text_selection_drag_idx = -1;
            impl_->scrollbar_drag = {};
#if !defined(AFFINEUI_STUB_BUILD)
            impl_->splitter_drag = {};
            impl_->float_resize = {};
            impl_->pending_tab_press = {};
            impl_->pressed_button = nullptr;
#endif
            if (ev.button == MouseButton::Left) {
                int scrollbar_idx = -1;
                ScrollbarGeometry scrollbar{};
                if (detail::find_vertical_scrollbar_at(
                        *impl_, ev.pos, scrollbar_idx, scrollbar)) {
                    impl_->scrollbar_drag.block_idx = scrollbar_idx;
                    impl_->scrollbar_drag.start_y = ev.pos.y;
                    impl_->scrollbar_drag.start_scroll_y =
                        impl_->blocks[static_cast<std::size_t>(
                            scrollbar_idx)].scroll_y;
                    impl_->scrollbar_drag.thumb_offset_y =
                        detail::rect_contains(scrollbar.thumb, ev.pos.x, ev.pos.y)
                            ? ev.pos.y - scrollbar.thumb.y
                            : scrollbar.thumb.h / 2;
                    if (!detail::rect_contains(scrollbar.thumb, ev.pos.x, ev.pos.y) &&
                        detail::scrollbar_scroll_from_thumb_y(
                            *impl_, scrollbar_idx,
                            ev.pos.y -
                                impl_->scrollbar_drag.thumb_offset_y)) {
                        result.redraw_requested = true;
                    }
                    // Grabbing the scrollbar un-hovers content for the whole
                    // drag: the pointer is on browser UI, and the rows
                    // scrolling by underneath must not keep a stale :hover.
                    if (impl_->hovered_idx >= 0) {
                        impl_->hovered_idx = -1;
                        bool reveal = false;
                        if (detail::refresh_hover_chain(*impl_, &reveal)) {
                            result.redraw_requested = true;
                        }
                    }
                    break;
                }
            }
#if !defined(AFFINEUI_STUB_BUILD)
            // Dock splitter grab takes priority over anything inside the panes
            // (the splitter is a thin element the pointer lands on directly).
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left) {
                detail::DocumentImpl::SplitterDrag sd{};
                if (detail::find_splitter_at(*impl_, impl_->hovered_idx, ev.pos, sd)) {
                    impl_->splitter_drag = sd;
                    impl_->splitter_drag.start_pos =
                        sd.horizontal ? ev.pos.y : ev.pos.x;
                    if (auto* selem =
                            detail::element_for_block(*impl_, sd.block_idx)) {
                        detail::set_element_class(*impl_, selem,
                                          "dcs-splitter--active", true);
                    }
                    break;
                }
            }
            // Floating tearoff resize from the synthetic JS-compatible edge and
            // corner zones. This has priority over chrome dragging.
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left) {
                detail::DocumentImpl::FloatResize fr{};
                if (detail::find_float_resize_at(*impl_, impl_->hovered_idx, ev.pos,
                                         fr)) {
                    impl_->float_resize = fr;
                    result.redraw_requested = true;
                    break;
                }
            }
            // Floating toolbar / panel grab: a [data-dcs-drag] container dragged
            // by a [data-dcs-drag-handle] inside it. Falls through if the press
            // wasn't on a handle, so the toolbar's own buttons still click.
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left) {
                detail::DocumentImpl::FloatDrag fd{};
                if (detail::find_float_drag_at(*impl_, impl_->hovered_idx, ev.pos, fd)) {
                    impl_->float_drag = fd;
                    result.redraw_requested = true;
                    break;
                }
            }
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left) {
                if (detail::begin_dcs_colorfield_drag(*impl_, impl_->hovered_idx, ev)) {
                    const auto kind = impl_->colorfield_drag.kind;
                    result.defer_widget_changes = true;
                    if (kind ==
                            detail::DocumentImpl::ColorfieldDrag::Kind::Square ||
                        kind ==
                            detail::DocumentImpl::ColorfieldDrag::Kind::Hue) {
                        detail::update_dcs_colorfield_drag(*impl_, ev);
                    }
                    result.redraw_requested = true;
                    break;
                }
            }
#endif
            // :active follows the press: set to whatever's under the
            // pointer right now, refresh the active chain so the bit
            // toggles on and an immediate restyle visualizes the press.
            impl_->active_idx     = impl_->hovered_idx;
            bool press_reveal = false;
            const bool h = detail::refresh_hover_chain(*impl_, &press_reveal);
            const bool a = detail::refresh_active_chain(*impl_, &press_reveal);
            handle_pseudo_reveal(press_reveal);
#if !defined(AFFINEUI_STUB_BUILD)
            detail::DocumentImpl::LiveControlDrag pending_live_drag{};
            const bool has_pending_live_drag =
                impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left &&
                detail::find_live_control_at(*impl_, impl_->hovered_idx, ev.pos,
                                     pending_live_drag);
            const bool defer_text_focus =
                has_pending_live_drag &&
                pending_live_drag.kind == LiveControlKind::NumericInput;
            const bool resize_textarea =
                has_pending_live_drag &&
                pending_live_drag.kind == LiveControlKind::TextAreaResize;
#else
            const bool defer_text_focus = false;
            const bool resize_textarea = false;
#endif
            // Focus moves to the nearest focusable ancestor of whatever
            // the press landed on. Clicking blank space (no focusable
            // ancestor) clears focus, matching browser behavior for
            // mousedown outside any form control.
            const int target = detail::focusable_ancestor(*impl_, impl_->hovered_idx);
            bool f = false;
            bool caret = false;
            if (!defer_text_focus && !resize_textarea) {
                f = detail::set_focus(*impl_, target);
                caret = detail::set_text_caret_from_point(*impl_, target, ev.pos);
            }
            if (!defer_text_focus && !resize_textarea && target >= 0 &&
                target < static_cast<int>(impl_->blocks.size()) &&
                impl_->blocks[static_cast<std::size_t>(target)].text_control) {
                auto& block = impl_->blocks[static_cast<std::size_t>(target)];
                const auto now = std::chrono::steady_clock::now();
                const bool double_click =
                    impl_->last_text_click_valid &&
                    impl_->last_text_click_idx == target &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - impl_->last_text_click_time).count() < 500 &&
                    std::abs(ev.pos.x - impl_->last_text_click_pos.x) <= 4 &&
                    std::abs(ev.pos.y - impl_->last_text_click_pos.y) <= 4;
                if (double_click && !block.text_value.empty()) {
                    const auto [begin, end] =
                        detail::word_bounds_at(block.text_value, block.caret_offset);
                    detail::set_text_selection(*impl_, target, block, begin, end);
                    detail::add_dirty_rect(*impl_, detail::block_visual_rect(*impl_, target));
                    caret = true;
                    impl_->last_text_click_valid = false;
                } else {
                    impl_->last_text_click_valid = true;
                    impl_->last_text_click_idx = target;
                    impl_->last_text_click_pos = ev.pos;
                    impl_->last_text_click_time = now;
                    if (ev.button == MouseButton::Left) {
                        impl_->text_selection_drag_idx = target;
                        impl_->text_selection_drag_anchor =
                            block.caret_offset;
                    }
                }
            } else {
                impl_->text_selection_drag_idx = -1;
            }
            if (h || a || f || caret) result.redraw_requested = true;
#if !defined(AFFINEUI_STUB_BUILD)
            // Checks / radios / switches toggle on PRESS (decius.js wires
            // pointerdown — waiting for the release reads as sluggish). The
            // release path only claims the click for checkbox-like targets so
            // the same gesture can't also activate a menu/dropdown beneath.
            bool press_consumed_by_checkbox = false;
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left && !has_pending_live_drag) {
                int check_idx = -1;
                lxb_dom_element_t* check_elem = nullptr;
                if (detail::find_checkbox_control_at(*impl_, impl_->hovered_idx,
                                             check_idx, check_elem) &&
                    detail::toggle_checkbox_control(*impl_, check_idx, check_elem)) {
                    result.redraw_requested = true;
                    // A checkbox inside a virtual-list row (a checklist) owns the
                    // click — it must not also select the row underneath it.
                    press_consumed_by_checkbox = true;
                }
            }
            // Collapsibles (foldout/subpanel headers), tree chevrons, and
            // selectable rows resolve on PRESS for the same immediate feel as
            // tabs. A chevron press consumes row selection so expand/collapse
            // does not also select the tree row underneath it.
            bool press_consumed_by_collapse = false;
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left && !has_pending_live_drag) {
                if (detail::toggle_decius_collapse_control(*impl_, impl_->hovered_idx) ||
                    detail::toggle_dcs_tree_chevron_control(*impl_, impl_->hovered_idx) ||
                    detail::toggle_virtual_tree_chevron(*impl_, impl_->hovered_idx)) {
                    result.redraw_requested = true;
                    press_consumed_by_collapse = true;
                }
                if (!press_consumed_by_collapse && !press_consumed_by_checkbox) {
                    lxb_dom_element_t* tree = nullptr;
                    lxb_dom_element_t* row = nullptr;
                    const bool draggable_tree_row =
                        detail::find_dcs_tree_row_at(*impl_, impl_->hovered_idx,
                                             tree, row) &&
                        detail::dcs_tree_row_draggable(row);
                    lxb_dom_element_t* select_box = nullptr;
                    lxb_dom_element_t* select_row = nullptr;
                    const bool selectable_row =
                        detail::find_dcs_select_row_at(*impl_, impl_->hovered_idx,
                                               select_box, select_row);
                    if (draggable_tree_row) {
                        impl_->tree_drag = {};
                        impl_->tree_drag.tree = tree;
                        impl_->tree_drag.row = row;
                        impl_->tree_drag.select_box = select_box;
                        impl_->tree_drag.select_row =
                            selectable_row ? select_row : nullptr;
                        impl_->tree_drag.start_x = ev.pos.x;
                        impl_->tree_drag.start_y = ev.pos.y;
                        impl_->tree_drag.press_ctrl = ev.ctrl;
                        impl_->tree_drag.press_shift = ev.shift;
                        impl_->tree_drag.press_super = ev.super;
                    } else if (selectable_row &&
                               detail::update_dcs_select_control(*impl_, select_box,
                                                         select_row, ev)) {
                        result.redraw_requested = true;
                    }
                }
            }
            // Decius menu triggers/items SELECT on press. Opening a menubar
            // menu is selection; leaf item activation still happens on release.
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left && !has_pending_live_drag) {
                lxb_dom_element_t* menu_elem = nullptr;
                lxb_dom_element_t* menu_item = nullptr;
                lxb_dom_element_t* trigger_elem = nullptr;
                lxb_dom_element_t* dropdown_group = nullptr;
                lxb_dom_element_t* dropdown_select = nullptr;
                lxb_dom_element_t* dropdown_option = nullptr;
                const bool over_dropdown =
                    detail::find_dropdown_control_at(*impl_, impl_->hovered_idx,
                                             dropdown_group, dropdown_select,
                                             dropdown_option);
                bool consume_release = false;
                if (!over_dropdown &&
                    detail::find_dcs_menu_item_at(*impl_, impl_->hovered_idx,
                                          menu_elem, menu_item)) {
                    if (detail::press_dcs_menu_item(*impl_, menu_item)) {
                        result.redraw_requested = true;
                    }
                } else if (!over_dropdown &&
                           detail::find_dcs_menu_trigger_at(*impl_, impl_->hovered_idx,
                                                    trigger_elem, menu_elem)) {
                    if (detail::toggle_dcs_menu(*impl_, trigger_elem, menu_elem)) {
                        result.redraw_requested = true;
                    }
                    consume_release = true;
                }
                if (consume_release) {
                    impl_->mouse_down_consumed_release = true;
                }
            }
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left && !has_pending_live_drag) {
                lxb_dom_element_t* button_elem = nullptr;
                if (detail::find_button_control_at(*impl_, impl_->hovered_idx,
                                           button_elem)) {
                    impl_->pressed_button = button_elem;
                }
            }
            // Dock-pane tab: SELECT IMMEDIATELY on press so it feels responsive
            // (first click always selects — the standard drag/drop rule), then
            // arm a potential drag. If the pointer then moves out of the pane it
            // tears off; if not, the press already did its job (selection).
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left) {
                lxb_dom_element_t* tab_elem = nullptr;
                if (detail::find_dockpane_tab_at(*impl_, impl_->hovered_idx, tab_elem)) {
                    const bool switched = detail::switch_dockpane_tab(*impl_, tab_elem);
                    if (switched) {
                        result.invalidate_view = true;
                        result.layout_changed = true;
                        result.redraw_requested = true;
                    }
                    auto* pane = detail::ancestor_with_class(tab_elem, "dcs-dockpane");
                    const std::string panel_id = detail::dockpane_tab_panel_id(tab_elem);
                    impl_->pending_tab_press.panel_id = panel_id;
                    impl_->pending_tab_press.start_x = ev.pos.x;
                    impl_->pending_tab_press.start_y = ev.pos.y;
                    impl_->pending_tab_press.switched_on_down = switched;
                    detail::dock_trace("tab-press panel=" + panel_id +
                               " pane=" + detail::pane_panel_id(pane) +
                               " switched=" + (switched ? "1" : "0") +
                               " at=(" + std::to_string(ev.pos.x) + "," +
                               std::to_string(ev.pos.y) + ")");
                    if (detail::dock_kind_of(pane) == "documents") {
                        break;
                    }
                    impl_->tab_drag = {};
                    impl_->tab_drag.tab = tab_elem;
                    impl_->tab_drag.pane = pane;
                    impl_->tab_drag.panel_id = panel_id;
                    impl_->tab_drag.start_x = ev.pos.x;
                    impl_->tab_drag.start_y = ev.pos.y;
                    impl_->tab_drag.switched_on_down = switched;
                    detail::capture_tab_drag_metadata(
                        *impl_, impl_->tab_drag, tab_elem, pane);
                }
            }
            if (has_pending_live_drag) {
                impl_->live_drag = pending_live_drag;
                impl_->live_drag.start_x = ev.pos.x;
                impl_->live_drag.start_y = ev.pos.y;
                impl_->live_drag.last_x = ev.pos.x;
                if (impl_->live_drag.kind == LiveControlKind::NumericInput) {
                    impl_->live_drag.defer_text_focus = true;
                    impl_->live_drag.focus_idx = target;
                    impl_->live_drag.focus_point = ev.pos;
                }
                if (impl_->live_drag.kind != LiveControlKind::AuiKnob &&
                    impl_->live_drag.kind != LiveControlKind::DeciusKnob &&
                    impl_->live_drag.kind != LiveControlKind::NumericInput &&
                    impl_->live_drag.kind != LiveControlKind::TextAreaResize &&
                    detail::update_active_live_control(*impl_, ev)) {
                    result.redraw_requested = true;
                }
            }
#endif
            break;
        }
        case EventType::MouseUp: {
            impl_->last_mouse_pos = ev.pos;
            ensure_interaction_layout();  // see MouseMove — never release on a
                                          // stale block tree
            if (impl_->scrollbar_drag.block_idx >= 0) {
                if (detail::scrollbar_scroll_from_thumb_y(
                        *impl_,
                        impl_->scrollbar_drag.block_idx,
                        ev.pos.y - impl_->scrollbar_drag.thumb_offset_y)) {
                    result.redraw_requested = true;
                }
                impl_->scrollbar_drag = {};
                break;
            }
#if !defined(AFFINEUI_STUB_BUILD)
            if (impl_->splitter_drag.block_idx >= 0) {
                const bool persist_layout = impl_->splitter_drag.persist_layout;
                if (detail::update_splitter_drag(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                if (auto* selem =
                        detail::element_for_block(*impl_, impl_->splitter_drag.block_idx)) {
                    if (detail::set_element_class(*impl_, selem, "dcs-splitter--active",
                                          false)) {
                        result.redraw_requested = true;
                    }
                }
                impl_->splitter_drag = {};
                // A pane was resized — let the app persist the new dock layout.
                result.layout_changed = persist_layout;
                break;
            }
            if (impl_->float_resize.elem) {
                if (detail::update_float_resize(*impl_, ev)) {
                    impl_->content_size = Size{0, 0};
                    result.redraw_requested = true;
                }
                const auto fr = impl_->float_resize;
                const Rect r = detail::float_resize_rect(fr, ev);
                impl_->float_resize = {};
                if (!fr.panel_id.empty()) {
                    Document::DockPlacement p;
                    p.present = true;
                    p.floating = true;
                    p.x = r.x - fr.cb_x;
                    p.y = r.y - fr.cb_y;
                    p.w = r.w;
                    p.h = r.h;
                    impl_->dock_overrides[fr.panel_id] = p;
                    detail::dock_trace("float-resize panel=" + fr.panel_id +
                               " rect=(" + std::to_string(p.x) + "," +
                               std::to_string(p.y) + "," +
                               std::to_string(p.w) + "x" +
                               std::to_string(p.h) + ")");
                    detail::dock_trace_state(*impl_, "after-float-resize");
                    result.layout_changed = true;
                }
                result.redraw_requested = true;
                break;
            }
            if (impl_->float_drag.elem) {
                // The gesture's moves were pure visual translations; land the
                // final position in the document with a single style write.
                if (detail::commit_float_drag(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                const auto fd = impl_->float_drag;
                impl_->float_drag = {};
                if (detail::set_drop_indicator(*impl_, nullptr)) {  // hide
                    result.redraw_requested = true;
                }
                if (!fd.panel_id.empty()) {
                    // Moving floating chrome only moves the panel. Re-docking is
                    // handled by dragging the dock tab/title, mirroring
                    // decius.js and preventing accidental reparents while a
                    // user is simply repositioning a tearoff.
                    int nx = fd.elem_doc_x + (ev.pos.x - fd.start_x);
                    int ny = fd.elem_doc_y + (ev.pos.y - fd.start_y);
                    if (fd.bounds_w > 0)
                        nx = std::clamp(nx, fd.bounds_x,
                                        std::max(fd.bounds_x, fd.bounds_x +
                                                                  fd.bounds_w -
                                                                  fd.elem_w));
                    if (fd.bounds_h > 0)
                        ny = std::clamp(ny, fd.bounds_y,
                                        std::max(fd.bounds_y, fd.bounds_y +
                                                                  fd.bounds_h -
                                                                  fd.elem_h));
                    Document::DockPlacement p;
                    p.present = true;
                    p.floating = true;
                    p.x = nx - fd.cb_x;
                    p.y = ny - fd.cb_y;
                    p.w = fd.elem_w;
                    p.h = fd.elem_h;
                    impl_->dock_overrides[fd.panel_id] = p;
                    detail::dock_trace("float-move panel=" + fd.panel_id +
                               " rect=(" + std::to_string(p.x) + "," +
                               std::to_string(p.y) + "," +
                               std::to_string(p.w) + "x" +
                               std::to_string(p.h) + ")");
                    detail::dock_trace_state(*impl_, "after-float-move");
                    result.layout_changed = true;  // re-seed + rebuild
                    result.redraw_requested = true;
                } else {
                    // A non-dockable float (e.g. a toolbar): in-session move only
                    // (no override store yet — see the float-position follow-up).
                    result.redraw_requested = true;
                }
                break;
            }
            // Dock-pane tab release: the tab was already selected on press; here
            // we only complete a DRAG. decius.js drop semantics, applied as DOM
            // surgery: center → join the target's tab row; the source pane
            // itself → visualized no-op ("drop back where it was"); edge →
            // split the target; anywhere else (free space or a wrong-kind
            // pane) → tear off into a floating panel.
            if (impl_->tab_drag.tab) {
                const auto td = impl_->tab_drag;
                impl_->tab_drag = {};
                impl_->pending_tab_press = {};
                if (td.dragging) {
                    if (detail::remove_tab_drag_ghost(*impl_)) {
                        result.redraw_requested = true;
                    }
                    if (detail::set_drop_indicator(*impl_, nullptr)) {  // hide
                        result.redraw_requested = true;
                    }
                    ensure_interaction_layout();
                    // Re-resolve the live elements by panel id (a reload during
                    // the drag invalidates the captured pointers).
                    auto* tab = detail::find_dockpane_tab_for_panel_id(*impl_,
                                                               td.panel_id);
                    // The tab's data-dcs-target IS the panel reference
                    // (decius.js semantics) — raw-HTML tabs target "#<id>"
                    // directly; View-emitted tabs target "#<id>-body". The
                    // id-convention lookup remains as a fallback for tabs
                    // that lost their target attribute.
                    auto* panel = detail::dcs_target_for_trigger(*impl_, tab);
                    if (!panel) {
                        panel = detail::find_dom_element_by_id(
                            *impl_, td.panel_id + "-body");
                    }
                    auto* source =
                        tab ? detail::ancestor_with_class(tab, "dcs-dockpane") : nullptr;
                    // Suppress lexbor's eager insert-time style attach for the
                    // whole gesture (incl. the finisher) — moves leave the tree
                    // transiently inconsistent and the eager attach reads a
                    // half-torn-down style weak-list (ASAN use-after-poison).
                    // detail::dock_structure_changed() rebuilds all styles once.
                    detail::SuppressDomStyleAttach no_eager_attach(*impl_);
                    bool changed_dock = false;
                    if (tab && panel && source) {
                        const auto t = detail::compute_drop_target(
                            *impl_, ev.pos, td.drag_kind, source);
                        if (t.self_noop) {
                            // Released back on its own pane (any pane, docked
                            // or floating): the hover showed "drop back where
                            // it was" feedback; the release does nothing.
                            detail::dock_trace("dock-noop-self panel=" + td.panel_id);
                        } else if (t.valid && t.pane &&
                                   t.zone == DropZone::Tab) {
                            // Center: join the target pane's tab row.
                            changed_dock =
                                detail::dock_move_tab_to(*impl_, tab, panel, t.pane);
                            if (changed_dock) {
                                detail::dock_cleanup_source(*impl_, source);
                                detail::dock_trace("dock panel=" + td.panel_id +
                                           " target=" + t.parent);
                            }
                        } else if (t.valid && t.pane) {
                            // Edge: a fresh pane owns the tab; split the target.
                            auto* fresh = detail::dock_create_pane(*impl_, td.panel_id,
                                                           td.drag_kind);
                            if (fresh &&
                                detail::dock_move_tab_to(*impl_, tab, panel, fresh)) {
                                detail::dock_split(*impl_, t.pane, t.zone, fresh,
                                           t.window_edge);
                                detail::dock_cleanup_source(*impl_, source);
                                changed_dock = true;
                                detail::dock_trace(
                                    "edge-dock panel=" + td.panel_id +
                                    " edge=" + detail::drop_zone_name(t.zone) +
                                    (t.window_edge ? " window-edge" : "") +
                                    " target=" + t.parent);
                            }
                        } else if (detail::document_float_host_bounds(*impl_)
                                       .w <= 0) {
                            // No float host in this document — a standalone
                            // dockpane's tabs switch panels but there is
                            // nowhere to float a tearoff. Dock gestures are
                            // inert.
                            detail::dock_trace("tab-drag-cancel (no float host) panel=" +
                                       td.panel_id);
                        } else {
                            // Free space → tear off into a (new) floating
                            // panel. This includes a tab dragged out of an
                            // existing tearout: it spawns its own floater at
                            // the drop point (for a single-tab floater that
                            // reads as moving the tearout; the emptied source
                            // dissolves in dock_cleanup_source).
                            const int si =
                                detail::block_index_for_exact_element(*impl_, source);
                            const Rect sb =
                                si >= 0 ? impl_->blocks[static_cast<std::size_t>(
                                                            si)]
                                              .bounds
                                        : Rect{};
                            int w = sb.w > 0 ? std::min(420, sb.w) : 320;
                            int h = sb.h > 0 ? std::min(360, sb.h) : 240;
                            w = detail::positive_int_attr(tab, "data-dcs-tearout-width",
                                                  w);
                            h = detail::positive_int_attr(tab,
                                                  "data-dcs-tearout-height", h);
                            auto* fp = detail::dock_spawn_floating_panel(
                                *impl_, tab, panel, ev.pos, w, h, td.drag_kind,
                                td.panel_id);
                            if (fp) {
                                detail::dock_cleanup_source(*impl_, source);
                                changed_dock = true;
                                detail::dock_trace("tearoff panel=" + td.panel_id);
                            }
                        }
                    }
                    if (changed_dock) {
                        // Gesture surgery restructured dock DOM the retained
                        // View doesn't know about; a later incremental
                        // reconcile over it would leave the surgery wrappers
                        // behind as duplicate panels. The app consumes this
                        // (take_dock_structure_changed) and re-bootstraps.
                        impl_->dock_structure_dirty = true;
                        detail::dock_structure_changed(*impl_);
                        detail::dock_trace_state(*impl_, "after-dock-surgery");
                        result.layout_changed = true;  // app persists + re-emits
                        result.redraw_requested = true;
                    }
                }
                break;  // tab interaction consumed this release
            }
            if (!impl_->pending_tab_press.panel_id.empty()) {
                impl_->pending_tab_press = {};
                break;  // tab press survived a rebuild; release is consumed
            }
            if (impl_->ui_control_script_attached &&
                impl_->colorfield_drag.kind !=
                    detail::DocumentImpl::ColorfieldDrag::Kind::None) {
                if (detail::update_dcs_colorfield_drag(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                impl_->colorfield_drag = {};
                result.redraw_requested = true;
                break;
            }
            if (impl_->ui_control_script_attached && impl_->tree_drag.row) {
                const bool was_dragging = impl_->tree_drag.dragging;
                bool changed = false;
                if (was_dragging) {
                    changed = detail::finish_dcs_tree_drag(*impl_, ev);
                    if (!changed) {
                        changed = detail::cancel_dcs_tree_drag(*impl_) || changed;
                    } else {
                        impl_->tree_drag = {};
                    }
                } else {
                    auto* select_box = impl_->tree_drag.select_box;
                    auto* select_row = impl_->tree_drag.select_row;
                    const bool press_ctrl = impl_->tree_drag.press_ctrl;
                    const bool press_shift = impl_->tree_drag.press_shift;
                    const bool press_super = impl_->tree_drag.press_super;
                    impl_->tree_drag = {};
                    if (select_box && select_row) {
                        Event select_event = ev;
                        select_event.ctrl = press_ctrl;
                        select_event.shift = press_shift;
                        select_event.super = press_super;
                        changed = detail::update_dcs_select_control(
                                      *impl_, select_box, select_row,
                                      select_event) ||
                                  changed;
                    }
                }
                if (changed) result.redraw_requested = true;
                break;
            }
#endif
            bool released_live_control = false;
#if !defined(AFFINEUI_STUB_BUILD)
            if (impl_->ui_control_script_attached &&
                impl_->live_drag.kind != LiveControlKind::None) {
                const auto released_drag = impl_->live_drag;
                if (detail::update_active_live_control(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                if (released_drag.kind == LiveControlKind::NumericInput &&
                    !impl_->live_drag.moved &&
                    detail::apply_deferred_text_focus(*impl_, released_drag, ev.pos)) {
                    result.redraw_requested = true;
                }
                released_live_control = true;
                impl_->live_drag = {};
            }
#endif
            impl_->hovered_idx    = detail::hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
            impl_->text_selection_drag_idx = -1;
            // Clear :active on every MouseUp â€” the press is over. We
            // don't try to be clever about "release outside the
            // pressed element" today; that nuance is part of the
            // click-state machinery to layer in later.
            impl_->active_idx     = -1;
            bool release_reveal = false;
            const bool h = detail::refresh_hover_chain(*impl_, &release_reveal);
            const bool a = detail::refresh_active_chain(*impl_, &release_reveal);
            if (h || a) result.redraw_requested = true;
            handle_pseudo_reveal(release_reveal);
            auto* pressed_menu_item = impl_->pressed_dcs_menu_item;
            const auto pressed_menu_item_bounds =
                impl_->pressed_dcs_menu_item_bounds;
            const bool pressed_menu_item_in_bounds =
                pressed_menu_item &&
                detail::rect_contains(pressed_menu_item_bounds, ev.pos.x, ev.pos.y);
            if (detail::clear_pressed_dcs_menu_item(*impl_)) {
                result.redraw_requested = true;
            }
            auto* pressed_button = impl_->pressed_button;
            impl_->pressed_button = nullptr;
            bool activated_pressed_menu_item = false;
            if (pressed_menu_item_in_bounds) {
                if (auto* menu_elem =
                        detail::ancestor_with_class(pressed_menu_item, "dcs-menu")) {
                    if (detail::activate_dcs_menu_item(*impl_, menu_elem,
                                               pressed_menu_item)) {
                        result.redraw_requested = true;
                    }
                    activated_pressed_menu_item = true;
                }
            }
            const bool release_consumed_on_down =
                impl_->mouse_down_consumed_release;
            impl_->mouse_down_consumed_release = false;
#if !defined(AFFINEUI_STUB_BUILD)
            if (release_consumed_on_down || activated_pressed_menu_item) {
                break;
            }
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left && !released_live_control) {
                if (!detail::click_preserves_transient_layers(*impl_, impl_->hovered_idx,
                                                      ev.pos) &&
                    detail::close_transient_layers(*impl_)) {
                    result.redraw_requested = true;
                    // The close mutated the DOM (hidden/style/aria attrs) and
                    // dirtied layout; re-hit-testing the stale tree returns -1
                    // and the release would be swallowed — the click must BOTH
                    // dismiss the layer and reach the control under the cursor
                    // (browser behavior). Relayout before resolving the target.
                    ensure_interaction_layout();
                    impl_->hovered_idx =
                        detail::hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
                    if (detail::refresh_hover_chain(*impl_)) {
                        result.redraw_requested = true;
                    }
                }
                bool toggled_checkbox = false;
                int check_idx = -1;
                lxb_dom_element_t* check_elem = nullptr;
                if (detail::find_checkbox_control_at(*impl_, impl_->hovered_idx,
                                             check_idx, check_elem)) {
                    // The toggle itself happened on MouseDown (pointerdown
                    // model). The release only claims the click so it does
                    // not fall through to dropdown/menu activation.
                    toggled_checkbox = true;
                }
                bool changed_button_group = false;
                bool changed_dropdown = false;
                if (!toggled_checkbox) {
                    lxb_dom_element_t* dropdown_group = nullptr;
                    lxb_dom_element_t* dropdown_select = nullptr;
                    lxb_dom_element_t* dropdown_option = nullptr;
                    if (detail::find_dropdown_control_at(*impl_, impl_->hovered_idx,
                                                 dropdown_group,
                                                 dropdown_select,
                                                 dropdown_option)) {
                        if (dropdown_option &&
                            detail::update_dropdown_control(*impl_, dropdown_group,
                                                    dropdown_option)) {
                            result.redraw_requested = true;
                            changed_dropdown = true;
                        } else if (dropdown_select &&
                                   detail::toggle_dropdown_menu(*impl_,
                                                        dropdown_group)) {
                            result.redraw_requested = true;
                            changed_dropdown = true;
                        }
                    }
                }
                if (!toggled_checkbox && !changed_dropdown) {
                    lxb_dom_element_t* group_elem = nullptr;
                    lxb_dom_element_t* option_elem = nullptr;
                    if (detail::find_button_group_option_at(*impl_, impl_->hovered_idx,
                                                    group_elem, option_elem) &&
                        detail::update_button_group_control(*impl_, group_elem,
                                                    option_elem)) {
                        result.redraw_requested = true;
                        changed_button_group = true;
                    }
                }
                bool changed_menu = false;
                if (!toggled_checkbox && !changed_dropdown &&
                    !changed_button_group) {
                    lxb_dom_element_t* menu_elem = nullptr;
                    lxb_dom_element_t* menu_item = nullptr;
                    lxb_dom_element_t* trigger_elem = nullptr;
                    if (detail::find_dcs_menu_item_at(*impl_, impl_->hovered_idx,
                                              menu_elem, menu_item)) {
                        if (menu_item == pressed_menu_item &&
                            detail::activate_dcs_menu_item(*impl_, menu_elem,
                                                   menu_item)) {
                            result.redraw_requested = true;
                        }
                        changed_menu = true;
                    } else if (detail::find_dcs_menu_trigger_at(*impl_,
                                                       impl_->hovered_idx,
                                                       trigger_elem,
                                                       menu_elem)) {
                        if (detail::toggle_dcs_menu(*impl_, trigger_elem,
                                            menu_elem)) {
                            result.redraw_requested = true;
                        }
                        changed_menu = true;
                    }
                }
                bool changed_popover = false;
                if (!toggled_checkbox && !changed_dropdown &&
                    !changed_button_group && !changed_menu) {
                    lxb_dom_element_t* trigger_elem = nullptr;
                    lxb_dom_element_t* popover_elem = nullptr;
                    if (detail::find_dcs_popover_trigger_at(*impl_,
                                                    impl_->hovered_idx,
                                                    trigger_elem,
                                                    popover_elem)) {
                        if (detail::toggle_dcs_popover(*impl_, trigger_elem,
                                               popover_elem)) {
                            result.redraw_requested = true;
                        }
                        changed_popover = true;
                    }
                }
                // NB: foldout/subpanel collapse and tree-chevron expand are
                // handled on MouseDown (press) — see the MouseDown case — so
                // they are intentionally absent here.
                // NB: dock-pane tab selection is handled by the tab-drag release
                // path above (a clean click selects; a drag tears off), so it is
                // intentionally absent from this click chain.
                //
                // NB: dcs-select ROW selection (handled on press) deliberately
                // does NOT suppress activation here. In the browser model a
                // click listener on a row fires alongside the selection
                // behavior — decius.js rows do both — so a selectable row
                // with a bound on_click gets its activation too (the
                // documented tree_row contract: "wire on_click for
                // selection"). Suppressing it left every selectable row's
                // on_click silently dead.
                if (!toggled_checkbox && !changed_dropdown &&
                    !changed_button_group && !changed_menu &&
                    !changed_popover) {
                    lxb_dom_element_t* button_elem = nullptr;
                    if (detail::find_button_control_at(*impl_, impl_->hovered_idx,
                                               button_elem) &&
                        button_elem == pressed_button &&
                        detail::activate_button_control(*impl_, button_elem)) {
                        result.redraw_requested = true;
                    }
                }
            }
#endif
            break;
        }
#if !defined(AFFINEUI_STUB_BUILD)
        case EventType::KeyDown: {
            // ESC clears focus, matching the convention browsers use for
            // dismissing a focused control.
            if (ev.key == Key::Escape) {
#if !defined(AFFINEUI_STUB_BUILD)
                if (impl_->ui_control_script_attached &&
                    detail::close_transient_layers(*impl_)) {
                    result.redraw_requested = true;
                }
#endif
                if (detail::set_focus(*impl_, -1)) result.redraw_requested = true;
                break;
            }

            Block* control = nullptr;
            if (!detail::focused_text_control(*impl_, control)) {
                // No text control has focus: Home/End and the vertical arrows
                // scroll the nearest scrollable-Y container under the pointer
                // (a virtual list re-windows off the new offset via
                // set_block_scroll_y's scroll-change emit). Ctrl/Cmd combos are
                // left for app shortcuts. (PageUp/PageDown await a Key-enum +
                // C-ABI addition — see the keyboard/horizontal-scroll PR.)
                if (!detail::command_modifier(ev)) {
                    const int target = detail::find_scrollable_y_ancestor(
                        *impl_, impl_->hovered_idx);
                    if (target >= 0) {
                        auto& sb = impl_->blocks[static_cast<std::size_t>(target)];
                        const int max_scroll =
                            std::max(0, sb.content_h - sb.bounds.h);
                        constexpr int kLineStep = 48;
                        bool handled = true;
                        int next = sb.scroll_y;
                        switch (ev.key) {
                            case Key::Home:      next = 0; break;
                            case Key::End:       next = max_scroll; break;
                            case Key::ArrowDown: next += kLineStep; break;
                            case Key::ArrowUp:   next -= kLineStep; break;
                            default: handled = false; break;
                        }
                        if (handled) {
                            next = std::clamp(next, 0, max_scroll);
                            if (detail::set_block_scroll_y(*impl_, target, next)) {
                                result.redraw_requested = true;
                            }
                            break;
                        }
                    }
                }
                break;
            }
            const int idx = impl_->focused_idx;
            const auto text = detail::emitted_text_control_value(*control);
            const bool command = detail::command_modifier(ev);

            if (command && ev.key == Key::A) {
                if (detail::move_text_caret(*impl_, idx, *control, text.size(), true)) {
                    detail::set_text_selection(*impl_, idx, *control, 0, text.size());
                    detail::add_dirty_rect(*impl_, detail::block_visual_rect(*impl_, idx));
                    result.redraw_requested = true;
                } else if (!detail::has_text_selection(*control) && !text.empty()) {
                    detail::set_text_selection(*impl_, idx, *control, 0, text.size());
                    detail::add_dirty_rect(*impl_, detail::block_visual_rect(*impl_, idx));
                    result.redraw_requested = true;
                }
            } else if (command && ev.key == Key::C) {
                if (detail::has_text_selection(*control)) {
                    detail::clipboard_set_text(*impl_, detail::selected_text(*control));
                }
            } else if (command && ev.key == Key::X) {
                if (detail::has_text_selection(*control)) {
                    detail::clipboard_set_text(*impl_, detail::selected_text(*control));
                    const auto [begin, end] = detail::normalized_selection(*control);
                    result.redraw_requested =
                        detail::delete_text_range(*impl_, idx, *control, begin, end);
                }
            } else if (command && ev.key == Key::V) {
                const std::string paste = detail::clipboard_get_text(*impl_);
                result.redraw_requested =
                    detail::replace_text_selection_or_insert(*impl_, idx, *control, paste);
            } else if (ev.key == Key::Backspace) {
                if (detail::has_text_selection(*control)) {
                    const auto [begin, end] = detail::normalized_selection(*control);
                    result.redraw_requested =
                        detail::delete_text_range(*impl_, idx, *control, begin, end);
                } else if (command) {
                    const std::size_t begin =
                        detail::previous_word_boundary(text, control->caret_offset);
                    result.redraw_requested =
                        detail::delete_text_range(*impl_, idx, *control, begin,
                                          control->caret_offset);
                } else {
                    std::size_t begin =
                        detail::previous_utf8_boundary(text, control->caret_offset);
                    result.redraw_requested =
                        detail::delete_text_range(*impl_, idx, *control, begin,
                                          control->caret_offset);
                }
            } else if (ev.key == Key::Delete) {
                if (detail::has_text_selection(*control)) {
                    const auto [begin, end] = detail::normalized_selection(*control);
                    result.redraw_requested =
                        detail::delete_text_range(*impl_, idx, *control, begin, end);
                } else if (command) {
                    const std::size_t end =
                        detail::next_word_boundary(text, control->caret_offset);
                    result.redraw_requested =
                        detail::delete_text_range(*impl_, idx, *control,
                                          control->caret_offset, end);
                } else {
                    const std::size_t end =
                        detail::next_utf8_boundary(text, control->caret_offset);
                    result.redraw_requested =
                        detail::delete_text_range(*impl_, idx, *control,
                                          control->caret_offset, end);
                }
            } else if (ev.key == Key::ArrowLeft ||
                       ev.key == Key::ArrowRight ||
                       ev.key == Key::Home ||
                       ev.key == Key::End) {
                std::size_t caret = control->caret_offset;
                if (!ev.shift && detail::has_text_selection(*control) &&
                    ev.key == Key::ArrowLeft) {
                    caret = detail::normalized_selection(*control).first;
                } else if (!ev.shift && detail::has_text_selection(*control) &&
                           ev.key == Key::ArrowRight) {
                    caret = detail::normalized_selection(*control).second;
                } else if (ev.key == Key::ArrowLeft) {
                    caret = command
                        ? detail::previous_word_boundary(text, caret)
                        : detail::previous_utf8_boundary(text, caret);
                } else if (ev.key == Key::ArrowRight) {
                    caret = command
                        ? detail::next_word_boundary(text, caret)
                        : detail::next_utf8_boundary(text, caret);
                } else if (ev.key == Key::Home) {
                    caret = 0;
                } else {
                    caret = text.size();
                }
                if (detail::move_text_caret(*impl_, idx, *control, caret, ev.shift)) {
                    result.redraw_requested = true;
                }
            }
            break;
        }
        case EventType::TextInput: {
            Block* control = nullptr;
            if (detail::focused_text_control(*impl_, control) && !ev.text.empty()) {
                result.redraw_requested =
                    detail::replace_text_selection_or_insert(
                        *impl_, impl_->focused_idx, *control, ev.text);
            }
            break;
        }
        case EventType::MouseWheel: {
            // Route to the nearest scrollable-Y ancestor of whatever
            // the pointer is over. Convention: positive wheel_dy
            // scrolls content up (i.e. scroll position increases).
            // The platform adapter is responsible for normalizing
            // direction + step size before we get here.
            const int wheel_hover =
                detail::hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
            const int target = detail::find_scrollable_y_ancestor(
                *impl_, wheel_hover >= 0 ? wheel_hover : impl_->hovered_idx);
            if (target < 0) break;
            auto& sb = impl_->blocks[static_cast<std::size_t>(target)];
            constexpr int kPxPerWheelStep = 24;
            // 64-bit intermediates: a large fling delta overflows the
            // float->int cast (UB — lands at INT_MIN and clamps to the TOP
            // instead of the bottom), and scroll_y + delta can overflow int
            // against multi-million-px virtual-list extents.
            const auto delta = static_cast<std::int64_t>(
                static_cast<double>(-ev.wheel_dy) * kPxPerWheelStep);
            const auto max_scroll = std::max<std::int64_t>(
                0, static_cast<std::int64_t>(sb.content_h) - sb.bounds.h);
            const int next = static_cast<int>(std::clamp<std::int64_t>(
                static_cast<std::int64_t>(sb.scroll_y) + delta, 0,
                max_scroll));
            if (detail::set_block_scroll_y(*impl_, target, next)) {
                result.redraw_requested = true;
            }
            break;
        }
#endif
        default:
            break;
    }
    return result;
}

namespace {
// Walk from the hovered block up the parent chain, returning the
// nearest non-default cursor. CSS-correct: a child without its own
// cursor inherits from its parent. The cascade already does this for
// ComputedStyle::cursor, but the *root* element with no inline
// cursor returns Default â€” so this walk is mostly belt-and-braces.
detail::ComputedStyle::Cursor effective_cursor(
        const std::vector<Block>& blocks,
        const detail::StyleStore& styles,
        int idx) {
    using C = detail::ComputedStyle::Cursor;
    while (idx >= 0) {
        const auto c = styles.computed(blocks[static_cast<std::size_t>(idx)].id).cursor;
        if (c != C::Default) return c;
        idx = blocks[static_cast<std::size_t>(idx)].parent_idx;
    }
    return C::Default;
}

// Translate the internal Cursor enum to the stable integer protocol that
// App's map_cursor() consumes: 0 default, 1 pointer, 2 text, 3 crosshair,
// 4 move/all, 5 not-allowed, 6 ew-resize, 7 ns-resize, 8 nwse-resize,
// 9 nesw-resize. Explicit on purpose — do NOT lean on enum ordinals lining up
// with the protocol codes (they don't, and that mismatch silently showed a
// diagonal cursor for ew-resize and a plain arrow for ns-resize).
int cursor_protocol_code(detail::ComputedStyle::Cursor c) {
    using C = detail::ComputedStyle::Cursor;
    switch (c) {
        case C::Pointer:    return 1;
        case C::Text:       return 2;
        case C::Crosshair:  return 3;
        case C::Move:       return 4;
        case C::NotAllowed: return 5;
        case C::ResizeEW:   return 6;
        case C::ResizeNS:   return 7;
        default:            return 0;  // Default
    }
}
}  // namespace

/// Cursor the OS should display right now (under the last mouse pos).
/// Lives on the public Document surface so App can poll it once per
/// frame without taking a Painter-style dependency.
int Document::hovered_cursor() const {
#if !defined(AFFINEUI_STUB_BUILD)
    for (int idx = impl_->hovered_idx;
         idx >= 0 && idx < static_cast<int>(impl_->blocks.size());
         idx = impl_->blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl_->blocks[static_cast<std::size_t>(idx)];
        if (detail::live_control_kind_for_block(block) == LiveControlKind::NumericInput) {
            return 6;
        }
        bool resize_x = false;
        bool resize_y = false;
        if (detail::point_in_textarea_resize_grip(*impl_, idx, impl_->last_mouse_pos,
                                          resize_x, resize_y)) {
            if (resize_x && !resize_y) return 6;
            if (!resize_x && resize_y) return 7;
            return 4;
        }
        // A visible scrollbar is browser UI, not content: it always shows
        // the plain arrow, regardless of the element's (or an ancestor's)
        // cursor — e.g. a textarea's UA `cursor:text` stops at the gutter.
        ScrollbarGeometry sb{};
        if (detail::vertical_scrollbar_geometry(*impl_, idx, sb)) {
            const Rect box = detail::block_border_visual_rect(*impl_, idx);
            const Rect gutter{sb.track.x - 2, box.y,
                              box.x + box.w - (sb.track.x - 2), box.h};
            if (detail::rect_contains(gutter, impl_->last_mouse_pos.x,
                              impl_->last_mouse_pos.y)) {
                return 0;
            }
        }
        detail::DocumentImpl::FloatResize fr{};
        if (detail::find_float_resize_at(*impl_, idx, impl_->last_mouse_pos, fr)) {
            return detail::cursor_for_float_resize_dir(fr.dir);
        }
    }
#endif
    return cursor_protocol_code(
        effective_cursor(impl_->blocks, impl_->style_store, impl_->hovered_idx));
}

Document::HoverInfo Document::hovered_info() const {
    HoverInfo info{};
    const int idx = impl_->hovered_idx;
    if (idx < 0 || idx >= static_cast<int>(impl_->blocks.size())) return info;
    const auto& b = impl_->blocks[static_cast<std::size_t>(idx)];
    info.valid   = true;
    info.tag     = b.tag;
    info.elem_id = b.elem_id;
    info.classes = b.classes;
    info.attrs   = b.attrs;
#if !defined(AFFINEUI_STUB_BUILD)
    info.bounds  = detail::block_border_visual_rect(*impl_, idx);
#else
    info.bounds  = b.bounds;
#endif
    return info;
}

std::vector<Document::HoverInfo> Document::hovered_info_chain() const {
    std::vector<HoverInfo> chain;
    hovered_info_chain(chain);
    return chain;
}

void Document::hovered_info_chain(std::vector<HoverInfo>& chain) const {
    chain.clear();
    chain.reserve(impl_->hovered_chain.size());
    int idx = impl_->hovered_idx;
    while (idx >= 0 && idx < static_cast<int>(impl_->blocks.size())) {
        const auto& b = impl_->blocks[static_cast<std::size_t>(idx)];
        HoverInfo info{};
        info.valid   = true;
        info.tag     = b.tag;
        info.elem_id = b.elem_id;
        info.classes = b.classes;
        info.attrs   = b.attrs;
#if !defined(AFFINEUI_STUB_BUILD)
        info.bounds  = detail::block_border_visual_rect(*impl_, idx);
#else
        info.bounds  = b.bounds;
#endif
        chain.push_back(std::move(info));
        idx = b.parent_idx;
    }
}
}  // namespace affineui
