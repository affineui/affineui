// document_dock.cpp — part of the AffineUI HTML5 renderer's document implementation.
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

// ── decius.js dock-layout surgery ────────────────────────────────────────────
// Faithful port of the drag-to-dock layout manager in decius-css
// js/src/decius.js (moveTabTo / splitDock / unsplitFromLayout /
// cleanupSourceDock / spawnFloatingPanel). Like the JS, these mutate the live
// DOM — the dock structure IS the DOM — so docking works identically for
// raw-HTML documents and View-built apps. After a gesture completes,
// detail::dock_structure_changed() recollects the box tree once and the Document's
// dock-layout snapshot (read back by the View on re-emit) keeps the result
// across view reloads.

// The drop zone over a dock target (defined ahead of the geometry section so
// the surgery ops can take an edge).

// el(tag, classes) — create an element with a class list (raw attrs; the
// post-gesture recollect restyles everything new).
lxb_dom_element_t* dock_create_el(detail::DocumentImpl& impl,
                                  std::string_view tag,
                                  std::string_view classes) {
    auto* e = lxb_dom_document_create_element(
        lxb_dom_interface_document(impl.doc), detail::as_lxb(tag), tag.size(), nullptr);
    if (e && !classes.empty()) {
        lxb_dom_element_set_attribute(e, detail::as_lxb("class"), 5, detail::as_lxb(classes),
                                      classes.size());
    }
    return e;
}

void dock_set_attr(lxb_dom_element_t* e, std::string_view name,
                   std::string_view value) {
    if (!e) return;
    lxb_dom_element_set_attribute(e, detail::as_lxb(name), name.size(), detail::as_lxb(value),
                                  value.size());
}

// First direct child carrying a class (JS `:scope > .cls`).
lxb_dom_element_t* dock_child_with_class(lxb_dom_element_t* parent,
                                         std::string_view cls) {
    if (!parent) return nullptr;
    for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(parent)); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (detail::class_list_contains(e, cls)) return e;
    }
    return nullptr;
}

// dockTabsEl: `:scope > __tabbar > __tabs` (or a bare `:scope > __tabs`).
lxb_dom_element_t* dock_tabs_el(lxb_dom_element_t* dock) {
    if (!dock) return nullptr;
    if (auto* tabbar = dock_child_with_class(dock, "dcs-dockpane__tabbar")) {
        if (auto* tabs = dock_child_with_class(tabbar, "dcs-dockpane__tabs"))
            return tabs;
    }
    return dock_child_with_class(dock, "dcs-dockpane__tabs");
}

lxb_dom_element_t* dock_tabbar_el(lxb_dom_element_t* dock) {
    return dock_child_with_class(dock, "dcs-dockpane__tabbar");
}

lxb_dom_element_t* dock_body_el(lxb_dom_element_t* dock) {
    return dock_child_with_class(dock, "dcs-dockpane__body");
}

lxb_dom_element_t* dock_toolbar_slot_el(lxb_dom_element_t* dock) {
    return dock_child_with_class(dock_tabbar_el(dock),
                                 "dcs-dockpane__toolbars");
}

lxb_dom_element_t* dock_shelf_el(lxb_dom_element_t* dock) {
    return dock_child_with_class(dock, "dcs-dockpane__shelf");
}

lxb_dom_element_t* dock_titlebar_el(lxb_dom_element_t* dock) {
    return dock_child_with_class(dock, "dcs-dockpane__titlebar");
}

lxb_dom_element_t* dock_title_tab(lxb_dom_element_t* dock) {
    auto* titlebar = dock_titlebar_el(dock);
    if (!titlebar) return nullptr;
    for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(titlebar));
         c; c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (detail::class_list_contains(e, "dcs-dockpane__tab") &&
            detail::has_attr(e, "data-dcs-title-tab"))
            return e;
    }
    return nullptr;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// dockTabs: title tab (if any) + the tab-row tabs, in order.
std::vector<lxb_dom_element_t*> dock_tabs(lxb_dom_element_t* dock) {
    std::vector<lxb_dom_element_t*> out;
    if (auto* title = dock_title_tab(dock)) out.push_back(title);
    if (auto* tabs = dock_tabs_el(dock)) {
        for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(tabs));
             c; c = lxb_dom_node_next(c)) {
            if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            auto* e = lxb_dom_interface_element(c);
            if (detail::class_list_contains(e, "dcs-dockpane__tab")) out.push_back(e);
        }
    }
    return out;
}
}  // namespace detail
namespace {


bool is_floating_dock(lxb_dom_element_t* dock) {
    return dock && detail::dock_kind_of(dock) == "panels" &&
           detail::ancestor_with_class(dock, "dcs-panel--floating") != nullptr;
}

// Raw class toggle — surgery runs on possibly-detached nodes mid-gesture; the
// one finisher recollect restyles everything, so no impl-aware setter here.
void dock_set_class(lxb_dom_element_t* e, std::string_view cls, bool on) {
    if (!e) return;
    dock_set_attr(e, "class", detail::class_list_set(e, cls, on));
}

// syncDockTabShape: single/multi/title-only modifier classes.
void sync_dock_tab_shape(detail::DocumentImpl& impl, lxb_dom_element_t* dock) {
    (void) impl;
    if (!dock) return;
    const std::size_t count = detail::dock_tabs(dock).size();
    dock_set_class(dock, "dcs-dockpane--single-tab", count == 1);
    dock_set_class(dock, "dcs-dockpane--multi-tab", count > 1);
    dock_set_class(dock, "dcs-dockpane--title-only",
                   dock_title_tab(dock) != nullptr);
}

// Detach a node that is being MOVED (not destroyed): the _wo_events variant
// keeps lexbor's CSS hooks from tearing down the element's style attachment
// mid-move (the post-gesture rematch+recollect rebuilds matches) and keeps
// the element's weak handles alive. True removals use lxb_dom_node_remove.
void dock_detach_for_move(lxb_dom_element_t* e) {
    if (e) lxb_dom_node_remove_wo_events(lxb_dom_interface_node(e));
}

// appendDockTab: insert after the last existing tab (before any non-tab
// chrome that shares the tabs container).
void append_dock_tab(lxb_dom_element_t* tabs, lxb_dom_element_t* tab) {
    if (!tabs || !tab) return;
    lxb_dom_node_t* before = nullptr;
    lxb_dom_element_t* last_tab = nullptr;
    for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(tabs)); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (detail::class_list_contains(e, "dcs-dockpane__tab")) last_tab = e;
        else if (!last_tab && !before) before = c;
    }
    dock_detach_for_move(tab);
    if (last_tab) {
        lxb_dom_node_insert_after(lxb_dom_interface_node(last_tab),
                                  lxb_dom_interface_node(tab));
    } else if (before) {
        lxb_dom_node_insert_before(before, lxb_dom_interface_node(tab));
    } else {
        lxb_dom_node_insert_child(lxb_dom_interface_node(tabs),
                                  lxb_dom_interface_node(tab));
    }
}

// activateTabInDock (unconditional flavor of switch_dockpane_tab, used after
// structural moves; the gesture finisher recollects, so raw attrs are fine).
void activate_tab_in_dock(lxb_dom_element_t* dock, lxb_dom_element_t* tab) {
    if (!dock || !tab) return;
    for (auto* t : detail::dock_tabs(dock)) {
        dock_set_attr(t, "aria-selected", t == tab ? "true" : "false");
    }
    const std::string sel = detail::attr_string(tab, "data-dcs-target");
    if (sel.empty() || sel.front() != '#') return;
    const std::string target_id = sel.substr(1);
    // The tabpanel lives in this dock's body; toggle hidden among siblings.
    auto* body = dock_body_el(dock);
    if (!body) return;
    for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(body)); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (!detail::has_attr(e, "data-dcs-tabpanel")) continue;
        if (detail::attr_string(e, "id") == target_id) {
            lxb_dom_element_remove_attribute(e, detail::as_lxb("hidden"), 6);
        } else {
            dock_set_attr(e, "hidden", "");
        }
    }
    // Toolbar visibility follows the active tab.
    std::vector<lxb_dom_element_t*> toolbars;
    auto collect = [&](lxb_dom_element_t* e) {
        if (detail::has_attr(e, "data-dcs-tabtoolbar")) toolbars.push_back(e);
    };
    detail::walk_dom_elements(lxb_dom_interface_node(dock), collect);
    for (auto* tb : toolbars) {
        if (detail::attr_string(tb, "data-dcs-tabtoolbar") == sel) {
            lxb_dom_element_remove_attribute(tb, detail::as_lxb("hidden"), 6);
        } else {
            dock_set_attr(tb, "hidden", "");
        }
    }
}

// prepareTabForTitlebar / prepareTabForTabbar: a single-tab floating pane's
// tab doubles as the panel title; these flip it between the two roles. The
// LOOK comes from the bundle's own `.dcs-panel__title--dock-tab` rules
// (appearance reset, grab cursor, tab chrome suppressed) — no inline styles,
// exactly like decius.js prepareTabForTitlebar.
void prepare_tab_for_titlebar(detail::DocumentImpl& impl,
                              lxb_dom_element_t* tab) {
    (void) impl;
    dock_set_class(tab, "dcs-panel__title", true);
    dock_set_class(tab, "dcs-panel__title--dock-tab", true);
    dock_set_attr(tab, "data-dcs-title-tab", "");
    dock_set_attr(tab, "aria-selected", "true");
}

void prepare_tab_for_tabbar(detail::DocumentImpl& impl,
                            lxb_dom_element_t* tab) {
    (void) impl;
    dock_set_class(tab, "dcs-panel__title", false);
    dock_set_class(tab, "dcs-panel__title--dock-tab", false);
    lxb_dom_element_remove_attribute(tab, detail::as_lxb("data-dcs-title-tab"), 18);
}

lxb_dom_element_t* titlebar_for_dock(detail::DocumentImpl& impl,
                                     lxb_dom_element_t* dock) {
    if (auto* existing = dock_titlebar_el(dock)) {
        return existing;
    }
    auto* titlebar = dock_create_el(
        impl, "header", "dcs-panel__header dcs-dockpane__titlebar");
    if (!titlebar) return nullptr;
    dock_set_attr(titlebar, "data-dcs-drag-handle", "");
    auto* tools = dock_create_el(impl, "div", "dcs-panel__tools");
    if (tools) {
        lxb_dom_node_insert_child(lxb_dom_interface_node(titlebar),
                                  lxb_dom_interface_node(tools));
    }
    auto* anchor = dock_tabbar_el(dock);
    auto* anchor_node = anchor ? lxb_dom_interface_node(anchor)
                               : lxb_dom_node_first_child(
                                     lxb_dom_interface_node(dock));
    if (anchor_node) {
        lxb_dom_node_insert_before(anchor_node,
                                   lxb_dom_interface_node(titlebar));
    } else {
        lxb_dom_node_insert_child(lxb_dom_interface_node(dock),
                                  lxb_dom_interface_node(titlebar));
    }
    return titlebar;
}

// convertDockToTitleOnly: a 1-tab floating pane shows its tab as the panel
// title bar instead of a tab row.
bool convert_dock_to_title_only(detail::DocumentImpl& impl,
                                lxb_dom_element_t* dock) {
    if (!is_floating_dock(dock)) return false;
    if (dock_title_tab(dock)) {
        sync_dock_tab_shape(impl, dock);
        return true;
    }
    auto tabs = detail::dock_tabs(dock);
    if (tabs.size() != 1) return false;
    auto* tab = tabs.front();
    auto* titlebar = titlebar_for_dock(impl, dock);
    if (!titlebar) return false;
    prepare_tab_for_titlebar(impl, tab);
    auto* tools = dock_child_with_class(titlebar, "dcs-panel__tools");
    dock_detach_for_move(tab);
    if (tools) {
        lxb_dom_node_insert_before(lxb_dom_interface_node(tools),
                                   lxb_dom_interface_node(tab));
    } else {
        lxb_dom_node_insert_child(lxb_dom_interface_node(titlebar),
                                  lxb_dom_interface_node(tab));
    }
    if (auto* tabbar = dock_tabbar_el(dock)) dock_set_attr(tabbar, "hidden", "");
    sync_dock_tab_shape(impl, dock);
    return true;
}

// ensureTabbedDock: the inverse — a title-only pane regrows its tab row
// before receiving another tab.
void ensure_tabbed_dock(detail::DocumentImpl& impl, lxb_dom_element_t* dock) {
    auto* title_tab = dock_title_tab(dock);
    auto* tabbar = dock_tabbar_el(dock);
    if (!title_tab) {
        if (tabbar) lxb_dom_element_remove_attribute(tabbar, detail::as_lxb("hidden"), 6);
        sync_dock_tab_shape(impl, dock);
        return;
    }
    // A title-only pane authored without the hidden tab row (or one that lost
    // it) regrows the chrome rather than destroying the title tab.
    if (!dock_tabs_el(dock)) {
        auto* fresh_tabbar = dock_create_el(impl, "div", "dcs-dockpane__tabbar");
        auto* fresh_tabs = dock_create_el(impl, "div", "dcs-dockpane__tabs");
        auto* fresh_toolbars =
            dock_create_el(impl, "div", "dcs-dockpane__toolbars");
        if (fresh_tabbar && fresh_tabs && fresh_toolbars) {
            lxb_dom_node_insert_child(lxb_dom_interface_node(fresh_tabbar),
                                      lxb_dom_interface_node(fresh_tabs));
            lxb_dom_node_insert_child(lxb_dom_interface_node(fresh_tabbar),
                                      lxb_dom_interface_node(fresh_toolbars));
            auto* titlebar = dock_titlebar_el(dock);
            if (titlebar &&
                lxb_dom_node_next(lxb_dom_interface_node(titlebar))) {
                lxb_dom_node_insert_after(lxb_dom_interface_node(titlebar),
                                          lxb_dom_interface_node(fresh_tabbar));
            } else {
                lxb_dom_node_insert_child(lxb_dom_interface_node(dock),
                                          lxb_dom_interface_node(fresh_tabbar));
            }
            tabbar = fresh_tabbar;
        }
    }
    if (auto* tabs = dock_tabs_el(dock)) {
        prepare_tab_for_tabbar(impl, title_tab);
        append_dock_tab(tabs, title_tab);
    } else {
        return;  // could not regrow chrome — leave the titlebar intact
    }
    if (auto* titlebar = dock_titlebar_el(dock)) {
        lxb_dom_node_remove(lxb_dom_interface_node(titlebar));
    }
    if (!tabbar) tabbar = dock_tabbar_el(dock);
    if (tabbar) lxb_dom_element_remove_attribute(tabbar, detail::as_lxb("hidden"), 6);
    sync_dock_tab_shape(impl, dock);
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// moveTabTo: move a tab + its tabpanel (+ its bound tab toolbar) into a
// target dock and activate it there.
bool dock_move_tab_to(detail::DocumentImpl& impl, lxb_dom_element_t* tab,
                      lxb_dom_element_t* panel, lxb_dom_element_t* target) {
    if (!tab || !panel || !target) return false;
    auto* source = detail::ancestor_with_class(tab, "dcs-dockpane");
    ensure_tabbed_dock(impl, target);
    auto* tabs = dock_tabs_el(target);
    auto* body = dock_body_el(target);
    if (!tabs || !body) return false;
    // The toolbar bound to this tab rides along, into the target's slot.
    const std::string sel = detail::attr_string(tab, "data-dcs-target");
    if (!sel.empty()) {
        if (auto* slot = dock_toolbar_slot_el(target)) {
            std::vector<lxb_dom_element_t*> toolbars;
            auto collect = [&](lxb_dom_element_t* e) {
                if (detail::attr_string(e, "data-dcs-tabtoolbar") == sel)
                    toolbars.push_back(e);
            };
            if (source) {
                detail::walk_dom_elements(lxb_dom_interface_node(source), collect);
            }
            for (auto* tb : toolbars) {
                dock_detach_for_move(tb);
                lxb_dom_node_insert_child(lxb_dom_interface_node(slot),
                                          lxb_dom_interface_node(tb));
            }
        }
    }
    prepare_tab_for_tabbar(impl, tab);
    append_dock_tab(tabs, tab);
    if (source && source != target) sync_dock_tab_shape(impl, source);
    dock_detach_for_move(panel);
    lxb_dom_node_insert_child(lxb_dom_interface_node(body),
                              lxb_dom_interface_node(panel));
    activate_tab_in_dock(target, tab);
    sync_dock_tab_shape(impl, target);
    return true;
}
}  // namespace detail
namespace {

// unsplitFromLayout: remove a pane (or emptied inner dock) + its adjacent
// Flex-grow of an element's inline `flex:` (0 when unset). A dock child with
// grow > 0 is FLEXIBLE (the document center's `1 1 0`); grow == 0 is a fixed
// column/row (`0 0 260px`) whose size the user expects to be stable.
double dock_flex_grow_of(lxb_dom_element_t* e) {
    const std::string v =
        detail::find_decl_value(detail::attr_string(e, "style"), "flex");
    if (v.empty()) return 0.0;
    return std::strtod(v.c_str(), nullptr);
}

// splitter; collapse empty .dcs-dock wrappers; hand the freed slice back so
// it never shows as dead space — WITHOUT scrambling sibling widths.
//
// NOTE: deliberately NOT the decius.js unsplitFromLayout rebalance (which
// resets every survivor to `flex:1 1 0`, collapsing the fixed-column vs
// flexible-center distinction — after one undock a 260px Outliner column
// became an equal share of the row and grew into the document). Instead:
//   • the removed slot's NEIGHBOUR is fixed  → grow it back by exactly the
//     freed px (un-splitting restores the width the split carved up), or
//   • the neighbour is flexible (the center) → touch nothing; it absorbs.
// Fixed columns keep their width through every dock/undock cycle either way.
// (Upstream decius.js has the same rebalance bug — candidate to back-port.)
void dock_unsplit_from_layout(detail::DocumentImpl& impl,
                              lxb_dom_element_t* node) {
    if (!node) return;
    auto* parent_node = lxb_dom_node_parent(lxb_dom_interface_node(node));
    lxb_dom_element_t* parent =
        parent_node && parent_node->type == LXB_DOM_NODE_TYPE_ELEMENT
            ? lxb_dom_interface_element(parent_node)
            : nullptr;
    const bool vertical =
        parent && detail::class_list_contains(parent, "dcs-dock--v");
    auto axis_px = [&](lxb_dom_element_t* e) -> int {
        const int bi = detail::block_index_for_exact_element(impl, e);
        if (bi < 0) return 0;
        const Rect& b = impl.blocks[static_cast<std::size_t>(bi)].bounds;
        return vertical ? b.h : b.w;
    };
    auto* prev = detail::previous_element_sibling(lxb_dom_interface_node(node));
    auto* next = detail::next_element_sibling(lxb_dom_interface_node(node));
    lxb_dom_element_t* splitter = nullptr;
    lxb_dom_element_t* adjacent = nullptr;
    if (prev && detail::class_list_contains(prev, "dcs-splitter")) {
        splitter = prev;
        adjacent = detail::previous_element_sibling(
            lxb_dom_interface_node(prev));
    } else if (next && detail::class_list_contains(next, "dcs-splitter")) {
        splitter = next;
        adjacent = detail::next_element_sibling(lxb_dom_interface_node(next));
    }
    // Measure BEFORE the removal while the blocks still resolve.
    const int freed = axis_px(node) + (splitter ? axis_px(splitter) : 0);
    const int adjacent_px = adjacent ? axis_px(adjacent) : 0;
    if (splitter) lxb_dom_node_remove(lxb_dom_interface_node(splitter));
    lxb_dom_node_remove(lxb_dom_interface_node(node));
    if (!parent || !detail::class_list_contains(parent, "dcs-dock")) return;
    std::vector<lxb_dom_element_t*> live;
    for (auto* c = lxb_dom_node_first_child(parent_node); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (!detail::class_list_contains(e, "dcs-splitter")) live.push_back(e);
    }
    if (live.empty()) {
        // Never collapse the workspace root itself. (decius.js leaves
        // single-child docks AS-IS otherwise — a one-pane dock is the normal
        // one-panel state, not a wrapper to unwrap.)
        if (!detail::class_list_contains(parent, "dcs-dock--floathost")) {
            dock_unsplit_from_layout(impl, parent);
        }
        return;
    }
    if (adjacent && dock_flex_grow_of(adjacent) <= 0.0 && freed > 0 &&
        adjacent_px > 0) {
        dock_set_attr(adjacent, "style",
                      "flex:0 0 " + std::to_string(adjacent_px + freed) +
                          "px;min-width:0;min-height:0");
    }
    // else: a flexible neighbour (or one of the survivors) absorbs the freed
    // slice via normal flex layout; every fixed survivor keeps its px.
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// cleanupSourceDock: after a tab leaves a pane — drop the pane when empty
// (whole floater if floating), else keep a tab selected; a single-tab floater
// collapses to title-only.
void dock_cleanup_source(detail::DocumentImpl& impl, lxb_dom_element_t* dock) {
    if (!dock) return;
    auto remaining = detail::dock_tabs(dock);
    if (remaining.empty()) {
        if (auto* floater = detail::ancestor_with_class(dock, "dcs-panel--floating")) {
            lxb_dom_node_remove(lxb_dom_interface_node(floater));
            return;
        }
        dock_unsplit_from_layout(impl, dock);
        return;
    }
    bool any_selected = false;
    for (auto* t : remaining) {
        if (detail::attr_string(t, "aria-selected") == "true") {
            any_selected = true;
            break;
        }
    }
    if (!any_selected) activate_tab_in_dock(dock, remaining.front());
    if (remaining.size() == 1 && is_floating_dock(dock)) {
        convert_dock_to_title_only(impl, dock);
        return;
    }
    sync_dock_tab_shape(impl, dock);
}
}  // namespace detail
namespace {

// splitDock: insert `fresh` on an edge of `target`. Same-direction parent →
// insert as a sibling (lock other siblings to their px, split the target's
// slice); otherwise wrap target+fresh in a new .dcs-dock that INHERITS the
// target's slot flex. Window-edge drops size the new pane as a fixed panel
// column (~320/220 px capped at 20%), pane-on-pane drops split 50/50.
constexpr int kDockNewPxH = 320;
constexpr int kDockNewPxV = 220;

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void dock_split(detail::DocumentImpl& impl, lxb_dom_element_t* target,
                DropZone edge, lxb_dom_element_t* fresh, bool window_edge) {
    if (!target || !fresh) return;
    const bool horizontal = edge == DropZone::Left || edge == DropZone::Right;
    const char* dock_cls = horizontal ? "dcs-dock" : "dcs-dock dcs-dock--v";
    const char* splitter_cls =
        horizontal ? "dcs-splitter" : "dcs-splitter dcs-splitter--h";
    auto* parent_node = lxb_dom_node_parent(lxb_dom_interface_node(target));
    lxb_dom_element_t* parent =
        parent_node && parent_node->type == LXB_DOM_NODE_TYPE_ELEMENT
            ? lxb_dom_interface_element(parent_node)
            : nullptr;
    const bool parent_is_dock =
        parent && detail::class_list_contains(parent, "dcs-dock");
    const bool parent_vertical =
        parent_is_dock && detail::class_list_contains(parent, "dcs-dock--v");
    const bool need_vertical = !horizontal;

    const int target_idx = detail::block_index_for_exact_element(impl, target);
    const Rect tb = target_idx >= 0
                        ? impl.blocks[static_cast<std::size_t>(target_idx)].bounds
                        : Rect{};
    const int t_size = horizontal ? tb.w : tb.h;
    const int default_new = horizontal ? kDockNewPxH : kDockNewPxV;
    const int cap = std::max(96, t_size * 20 / 100);
    // Budget the inserted splitter's slice too, so target + splitter + fresh
    // sum to the target's old size exactly — otherwise every dock/undock
    // cycle leaks a splitter's width into (or out of) the fixed column.
    constexpr int kSplitterPx = 6;
    const int new_px =
        window_edge ? std::min(default_new, cap)
                    : std::max(20, (t_size - kSplitterPx) / 2);
    const int target_px =
        window_edge ? std::max(120, t_size - new_px - kSplitterPx)
                    : std::max(20, t_size - kSplitterPx - new_px);
    const std::string flex_min = ";min-width:0;min-height:0";
    // The fresh pane is always a FIXED slice (`flex:0 0 px`, no grow), and a
    // fixed target carves into fixed halves that sum to its old size — a
    // docked side column never gains a grow factor, so its width is stable
    // through every later layout change (the user-visible invariant). A
    // FLEXIBLE target (the document center, `flex:1 1 0`) keeps its style
    // and simply yields the fresh pane's slice. Deliberately not decius.js's
    // `flex:1 1 basis` + lock-all-siblings scheme, which let fixed columns
    // creep whenever free space moved. (Candidate to back-port upstream.)
    auto flex_fixed = [&](int px) {
        return "flex:0 0 " + std::to_string(px) + "px" + flex_min;
    };
    const bool target_flexible = dock_flex_grow_of(target) > 0.0;

    auto* splitter = dock_create_el(impl, "div", splitter_cls);
    dock_set_attr(splitter, "data-dcs-splitter", horizontal ? "" : "h");
    if (parent_is_dock && parent_vertical == need_vertical) {
        // Same-direction parent: only the target's slice is carved; siblings
        // keep their styles (fixed stay fixed, the center stays flexible).
        if (!target_flexible) {
            dock_set_attr(target, "style", flex_fixed(target_px));
        }
        dock_set_attr(fresh, "style", flex_fixed(new_px));
        if (edge == DropZone::Left || edge == DropZone::Top) {
            lxb_dom_node_insert_before(lxb_dom_interface_node(target),
                                       lxb_dom_interface_node(fresh));
            lxb_dom_node_insert_before(lxb_dom_interface_node(target),
                                       lxb_dom_interface_node(splitter));
        } else {
            lxb_dom_node_insert_after(lxb_dom_interface_node(target),
                                      lxb_dom_interface_node(splitter));
            lxb_dom_node_insert_after(lxb_dom_interface_node(splitter),
                                      lxb_dom_interface_node(fresh));
        }
    } else {
        // Different-direction (or no) parent: wrap target+fresh in a new dock
        // of the matching direction; the wrapper INHERITS the target's slot
        // (so a fixed column stays a fixed column no matter how its interior
        // splits). Inside, the target flexes to fill the wrapper's remainder
        // and the fresh pane is the fixed slice.
        auto* wrap = dock_create_el(impl, "div", dock_cls);
        std::string target_flex = detail::find_decl_value(detail::attr_string(target, "style"),
                                                  "flex");
        std::string wrap_style =
            (target_flex.empty() ? std::string("flex:1")
                                 : "flex:" + target_flex) +
            flex_min + ";display:flex" +
            (need_vertical ? ";flex-direction:column" : "");
        dock_set_attr(wrap, "style", wrap_style);
        lxb_dom_node_insert_before(lxb_dom_interface_node(target),
                                   lxb_dom_interface_node(wrap));
        dock_detach_for_move(target);  // re-parents into the wrapper below
        dock_set_attr(target, "style", "flex:1 1 0" + flex_min);
        dock_set_attr(fresh, "style", flex_fixed(new_px));
        auto append = [&](lxb_dom_element_t* e) {
            lxb_dom_node_insert_child(lxb_dom_interface_node(wrap),
                                      lxb_dom_interface_node(e));
        };
        if (edge == DropZone::Left || edge == DropZone::Top) {
            append(fresh);
            append(splitter);
            append(target);
        } else {
            append(target);
            append(splitter);
            append(fresh);
        }
    }
}

// Fresh empty dockpane chrome (tabbar > tabs + toolbars, shelf, body) — the
// canonical shape both the View emitter and decius.js produce.
lxb_dom_element_t* dock_create_pane(detail::DocumentImpl& impl,
                                    std::string_view panel_id,
                                    std::string_view kind) {
    auto* pane = dock_create_el(impl, "section", "dcs-dockpane");
    if (!pane) return nullptr;
    if (!panel_id.empty()) {
        dock_set_attr(pane, "data-aui-name", "pane-" + std::string(panel_id));
    }
    if (!kind.empty() && kind != "panels") {
        dock_set_attr(pane, "data-dcs-dock-kind", kind);
    }
    auto* tabbar = dock_create_el(impl, "div", "dcs-dockpane__tabbar");
    dock_set_attr(tabbar, "data-dcs-drag-handle", "");
    auto* tabs = dock_create_el(impl, "div", "dcs-dockpane__tabs");
    auto* toolbars = dock_create_el(impl, "div", "dcs-dockpane__toolbars");
    auto* shelf = dock_create_el(impl, "div", "dcs-dockpane__shelf");
    dock_set_attr(shelf, "hidden", "");
    auto* body = dock_create_el(impl, "div", "dcs-dockpane__body");
    if (!tabbar || !tabs || !toolbars || !shelf || !body) return nullptr;
    lxb_dom_node_insert_child(lxb_dom_interface_node(tabbar),
                              lxb_dom_interface_node(tabs));
    lxb_dom_node_insert_child(lxb_dom_interface_node(tabbar),
                              lxb_dom_interface_node(toolbars));
    lxb_dom_node_insert_child(lxb_dom_interface_node(pane),
                              lxb_dom_interface_node(tabbar));
    lxb_dom_node_insert_child(lxb_dom_interface_node(pane),
                              lxb_dom_interface_node(shelf));
    lxb_dom_node_insert_child(lxb_dom_interface_node(pane),
                              lxb_dom_interface_node(body));
    return pane;
}

// spawnFloatingPanel: tear a tab off into a floating panel hovering over the
// float host. Spawn position clamps inside the host so a drop at the window
// edge can't leave the floater off-screen ("the panel disappeared").
lxb_dom_element_t* dock_spawn_floating_panel(
    detail::DocumentImpl& impl, lxb_dom_element_t* tab,
    lxb_dom_element_t* panel, Point drop, int w, int h, std::string_view kind,
    std::string_view panel_id) {
    const Rect host = detail::document_float_host_bounds(impl);
    const Rect root = detail::root_float_host_bounds(impl);
    // Root-relative coordinates (floats are root overlays); clamped to the
    // document-body host rect, mirroring the existing tearoff math.
    const int hx = root.w > 0 ? root.x : host.x;
    const int hy = root.h > 0 ? root.y : host.y;
    constexpr int kMargin = 8;
    int fw = w > 0 ? w : 320;
    int fh = h > 0 ? h : 220;
    if (host.w > kMargin * 2) fw = std::max(1, std::min(fw, host.w - kMargin * 2));
    if (host.h > kMargin * 2) fh = std::max(1, std::min(fh, host.h - kMargin * 2));
    const int host_x = host.x - hx;
    const int host_y = host.y - hy;
    int x = drop.x - hx - 60;
    int y = drop.y - hy - 12;
    if (host.w > 0) {
        const int lo = host_x + kMargin;
        const int hi = host_x + host.w - fw - kMargin;
        x = std::clamp(x, lo, std::max(lo, hi));
    }
    if (host.h > 0) {
        const int lo = host_y + kMargin;
        const int hi = host_y + host.h - fh - kMargin;
        y = std::clamp(y, lo, std::max(lo, hi));
    }

    auto* fp = dock_create_el(impl, "section", "dcs-panel dcs-panel--floating");
    if (!fp) return nullptr;
    dock_set_attr(fp, "data-aui-name", "float-" + std::string(panel_id));
    dock_set_attr(fp, "style",
                  "position:absolute;left:" + std::to_string(x) + "px;top:" +
                      std::to_string(y) + "px;width:" + std::to_string(fw) +
                      "px;height:" + std::to_string(fh) +
                      "px;z-index:60;display:flex;flex-direction:column;"
                      "pointer-events:auto");
    dock_set_attr(fp, "data-dcs-drag", "");
    dock_set_attr(fp, "data-dcs-drag-bounds", ".dcs-dock--floathost");
    dock_set_attr(fp, "data-dcs-dock-id", panel_id);
    auto* dock = detail::dock_create_pane(impl, panel_id, kind);
    if (!dock) return nullptr;
    dock_set_attr(dock, "style", "flex:1;min-width:0;min-height:0");
    lxb_dom_node_insert_child(lxb_dom_interface_node(fp),
                              lxb_dom_interface_node(dock));
    detail::dock_move_tab_to(impl, tab, panel, dock);
    // Floats are root overlays (siblings of the split tree).
    lxb_dom_element_t* parent = nullptr;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* e = detail::element_for_block(impl, i);
        if (e && detail::class_list_contains(e, "dcs-dock--floathost")) {
            parent = e;
            break;
        }
    }
    if (!parent) {
        if (auto* body = lxb_html_document_body_element(impl.doc)) {
            parent = lxb_dom_interface_element(body);
        }
    }
    if (!parent) return nullptr;
    lxb_dom_node_insert_child(lxb_dom_interface_node(parent),
                              lxb_dom_interface_node(fp));
    convert_dock_to_title_only(impl, dock);
    return fp;
}

// One finisher per completed gesture: restyle + recollect the box tree, drop
// stale interaction indices, and request a full relayout + repaint.
void dock_structure_changed(detail::DocumentImpl& impl) {
    const Rect old_rect = detail::document_visual_rect(impl);
    const auto st0 = std::chrono::steady_clock::now();
    if (impl.resolver) impl.resolver->clear();
    // Elements created or re-parented by the surgery have no (or stale)
    // lexbor stylesheet attachments; rebuild the match lists before the
    // resolver walks them (same contract as set_attribute_on_element).
    // Inline styles FIRST: elements that acquired style="" during the
    // suppressed gesture have an empty cached inline list, and the rematch
    // below re-attaches that cache verbatim — without the re-parse a spawned
    // floater's position/size style is inert and the panel collapses to an
    // intrinsic box at the origin.
    if (impl.doc) {
        if (auto* body = lxb_html_document_body_element(impl.doc)) {
            detail::parse_inline_styles_deep(lxb_dom_interface_node(body));
        }
    }
    detail::rematch_stylesheet_matches_for_subtree(impl, -1);
    const auto st1 = std::chrono::steady_clock::now();
    detail::recollect_blocks_from_current_dom(impl);
    const auto st2 = std::chrono::steady_clock::now();
    if (detail::MutationTraceTimer::enabled()) {
        const auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        std::fprintf(stderr,
                     "[settle]   rematch(all)=%.1f ms  recollect=%.1f ms\n",
                     ms(st0, st1), ms(st1, st2));
        std::fflush(stderr);
    }
    detail::debug_validate_attr_lists(impl, "dock-structure-changed");
    impl.hovered_idx = -1;
    impl.active_idx = -1;
    impl.hovered_chain.clear();
    impl.active_chain.clear();
    detail::mark_live_mutation_dirty(impl, -1, old_rect, /*needs_layout=*/true);
    impl.paint_dirty = true;
}
}  // namespace detail
namespace {

// Tear a docked panel off into a floating panel at the drop point: records a
// floating placement override so the next resolve_dock emits it as a
// .dcs-panel--floating overlay (then movable via float-drag). The app reloads on
// result.layout_changed, which re-seeds the layout from these overrides.
bool tear_off_panel(detail::DocumentImpl& impl, std::string_view panel_id,
                    lxb_dom_element_t* pane, Point drop) {
    if (panel_id.empty()) return false;
    (void) pane;
    constexpr int kDefaultW = 320;
    constexpr int kDefaultH = 240;
    constexpr int kMargin = 8;
    const Rect host = detail::document_float_host_bounds(impl);
    const Rect root = detail::root_float_host_bounds(impl);
    auto* tab = detail::find_dockpane_tab_for_panel_id(impl, panel_id);
    int w = detail::positive_int_attr(tab, "data-dcs-tearout-width", kDefaultW);
    int h = detail::positive_int_attr(tab, "data-dcs-tearout-height", kDefaultH);
    // Place so the title lands near the cursor, clamped into the document
    // content float host. Runtime floating overrides are root-relative because
    // View emits tearoffs as root overlays, while the allowed rectangle remains
    // the document body.
    const int hx = root.w > 0 ? root.x : host.x;
    const int hy = root.h > 0 ? root.y : host.y;
    const int hw = host.w;
    const int hh = host.h;
    if (hw > kMargin * 2) {
        w = std::max(1, std::min(w, hw - kMargin * 2));
    }
    if (hh > kMargin * 2) {
        h = std::max(1, std::min(h, hh - kMargin * 2));
    }
    const int host_x = host.x - hx;
    const int host_y = host.y - hy;
    int x = drop.x - hx - 60;
    int y = drop.y - hy - 12;
    if (hw > 0) {
        const int lo = host_x + kMargin;
        const int hi = host_x + hw - w - kMargin;
        x = std::clamp(x, lo, std::max(lo, hi));
    }
    if (hh > 0) {
        const int lo = host_y + kMargin;
        const int hi = host_y + hh - h - kMargin;
        y = std::clamp(y, lo, std::max(lo, hi));
    }
    Document::DockPlacement p;
    p.present = true;
    p.floating = true;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    impl.dock_overrides[std::string(panel_id)] = p;
    detail::dock_trace("tearoff panel=" + std::string(panel_id) +
               " drop=(" + std::to_string(drop.x) + "," +
               std::to_string(drop.y) + ") rect=(" + std::to_string(p.x) +
               "," + std::to_string(p.y) + "," + std::to_string(p.w) +
               "x" + std::to_string(p.h) + ")");
    detail::dock_trace_state(impl, "after-tearoff");
    return true;
}

// ── Dock-layout snapshot (DOM → Document::DockLayout) ───────────────────────
// A pure read of the live dock DOM. The View replays it on re-emit so surgery
// survives reloads; apps serialize it for workspace persistence.
Document::DockLayout::Node dock_layout_node(lxb_dom_element_t* e) {
    Document::DockLayout::Node n;
    n.flex = detail::find_decl_value(detail::attr_string(e, "style"), "flex");
    if (detail::class_list_contains(e, "dcs-dock")) {
        n.split = true;
        n.vertical = detail::class_list_contains(e, "dcs-dock--v");
        for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(e)); c;
             c = lxb_dom_node_next(c)) {
            if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            auto* ce = lxb_dom_interface_element(c);
            if (detail::class_list_contains(ce, "dcs-splitter")) continue;
            if (detail::class_list_contains(ce, "dcs-dock") ||
                detail::class_list_contains(ce, "dcs-dockpane")) {
                n.children.push_back(dock_layout_node(ce));
            }
        }
        return n;
    }
    // Leaf: a dockpane.
    n.kind = detail::dock_kind_of(e);
    n.dock_parent = detail::attr_string(e, "data-aui-dock-parent");
    n.dock_side = detail::has_attr(e, "data-aui-dock-side")
                      ? detail::int_attr(e, "data-aui-dock-side", -1)
                      : -1;
    for (auto* tab : detail::dock_tabs(e)) {
        const std::string id = detail::dockpane_tab_panel_id(tab);
        if (id.empty()) continue;
        n.tabs.push_back(id);
        if (detail::attr_string(tab, "aria-selected") == "true") n.active = id;
    }
    return n;
}

lxb_dom_element_t* find_first_descendant_with_class(lxb_dom_node_t* root,
                                                    std::string_view cls) {
    lxb_dom_element_t* found = nullptr;
    auto collect = [&](lxb_dom_element_t* e) {
        if (!found && detail::class_list_contains(e, cls)) found = e;
    };
    detail::walk_dom_elements(root, collect);
    return found;
}

// ── Drop zones (drag-to-dock / re-dock) ─────────────────────────────────────
// Faithful port of decius.js Je/je/Ke (see [[decius-js-docking-algorithm]]):
//  • Ke: within 32px of the viewport outer edge -> dock to the WHOLE-AREA edge
//        (parent = the document/root), unless a pane edge under the cursor is a
//        better local target.
//  • else the .dcs-dockpane under the cursor (dock-kind must match the dragged
//    tab's kind): over its tabbar -> center (co-tab); else je() picks the
//    nearest edge if within the outer 22%, else center. The document
//    (--center, kind "documents") is only a target for
//    documents-kind tabs and only via window-edge / its tabbar — a body drop
//    there yields no target, so the caller tears off.

// edgeOwnerDock: the OUTERMOST .dcs-dock whose direction matches the edge —
// window-edge drops always split at the workspace level, not at an inner
// pane's level.
lxb_dom_element_t* edge_owner_dock(detail::DocumentImpl& impl, DropZone edge) {
    const bool horizontal = edge == DropZone::Left || edge == DropZone::Right;
    const bool want_v = !horizontal;
    std::vector<lxb_dom_element_t*> docks;
    auto collect = [&](lxb_dom_element_t* e) {
        if (detail::class_list_contains(e, "dcs-dock")) docks.push_back(e);
    };
    detail::walk_dom_elements(detail::document_dom_root(impl), collect);
    auto is_match = [&](lxb_dom_element_t* d) {
        return detail::class_list_contains(d, "dcs-dock") &&
               detail::class_list_contains(d, "dcs-dock--v") == want_v;
    };
    lxb_dom_element_t* first_match = nullptr;
    for (auto* d : docks) {
        if (!is_match(d)) continue;
        if (!first_match) first_match = d;
        bool has_matching_ancestor = false;
        for (auto* n = lxb_dom_node_parent(lxb_dom_interface_node(d)); n;
             n = lxb_dom_node_parent(n)) {
            if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (is_match(lxb_dom_interface_element(n))) {
                has_matching_ancestor = true;
                break;
            }
        }
        if (!has_matching_ancestor) return d;  // outermost matching
    }
    if (first_match) return first_match;
    // No matching-direction dock: any root dock (splitDock wraps it).
    for (auto* d : docks) {
        bool inside_dock = false;
        for (auto* n = lxb_dom_node_parent(lxb_dom_interface_node(d)); n;
             n = lxb_dom_node_parent(n)) {
            if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (detail::class_list_contains(lxb_dom_interface_element(n), "dcs-dock")) {
                inside_dock = true;
                break;
            }
        }
        if (!inside_dock) return d;
    }
    return docks.empty() ? nullptr : docks.front();
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
const char* drop_zone_name(DropZone zone) {
    switch (zone) {
        case DropZone::Left: return "left";
        case DropZone::Right: return "right";
        case DropZone::Top: return "top";
        case DropZone::Bottom: return "bottom";
        case DropZone::Tab: return "tab";
        default: return "none";
    }
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::string drop_target_summary(const DropTarget& t) {
    if (!t.valid) return "invalid";
    return "parent=" + t.parent + " zone=" + detail::drop_zone_name(t.zone) +
           " preview=(" + std::to_string(t.x) + "," + std::to_string(t.y) +
           "," + std::to_string(t.w) + "x" + std::to_string(t.h) + ")";
}

// A pane's dock-kind: data-dcs-dock-kind, else "documents" for the center, else
// "panels". A drag only docks into a pane of the same kind (decius J()).
std::string dock_kind_of(lxb_dom_element_t* pane) {
    if (!pane) return "panels";
    const std::string k = detail::attr_string(pane, "data-dcs-dock-kind");
    if (!k.empty()) return k;
    return detail::class_list_contains(pane, "dcs-dockpane--center") ? "documents"
                                                             : "panels";
}

std::string pane_panel_id(lxb_dom_element_t* pane) {
    // Pane identity, best-effort (decius.js targets pane ELEMENTS — this id
    // is metadata for overrides/traces, so derive it from whatever the pane
    // carries rather than requiring one naming convention):
    //   1. View/surgery panes: data-aui-name="pane-<id>".
    //   2. Panes inside a floating panel: the floater's data-dcs-dock-id.
    //   3. App-authored panes (the DENDER N-panel): the selected (or first)
    //      tab's panel id — a pane IS its tabs.
    if (!pane) return {};
    const std::string n = detail::attr_string(pane, "data-aui-name");  // pane-<id>
    if (n.rfind("pane-", 0) == 0) return n.substr(5);
    if (auto* floater =
            detail::ancestor_with_class(pane, "dcs-panel--floating")) {
        std::string id = detail::attr_string(floater, "data-dcs-dock-id");
        if (!id.empty()) return id;
    }
    lxb_dom_element_t* first_tab = nullptr;
    lxb_dom_element_t* selected_tab = nullptr;
    struct Walk {
        lxb_dom_element_t** first;
        lxb_dom_element_t** selected;
        void operator()(lxb_dom_element_t* e) {
            if (!detail::class_list_contains(e, "dcs-dockpane__tab")) return;
            if (!*first) *first = e;
            if (!*selected &&
                detail::attr_string(e, "aria-selected") == "true") {
                *selected = e;
            }
        }
    } walk{&first_tab, &selected_tab};
    detail::walk_dom_elements(lxb_dom_interface_node(pane), walk);
    auto* tab = selected_tab ? selected_tab : first_tab;
    return tab ? detail::dockpane_tab_panel_id(tab) : std::string();
}
}  // namespace detail
namespace {

std::vector<std::string> dockpane_tab_ids(detail::DocumentImpl& impl,
                                          lxb_dom_element_t* pane) {
    std::vector<std::string> out;
    if (!pane) return out;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* elem = detail::element_for_block(impl, i);
        if (!elem || !detail::class_list_contains(elem, "dcs-dockpane__tab")) {
            continue;
        }
        if (detail::ancestor_with_class(elem, "dcs-dockpane") != pane) continue;
        const std::string id = detail::dockpane_tab_panel_id(elem);
        if (!id.empty() &&
            std::find(out.begin(), out.end(), id) == out.end()) {
            out.push_back(id);
        }
    }
    return out;
}

std::string dock_side_name(int side) {
    switch (side) {
        case 0: return "left";
        case 1: return "right";
        case 2: return "top";
        case 3: return "bottom";
        case 4: return "tab";
        default: return std::to_string(side);
    }
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::string dock_rect_summary(const Rect& r) {
    return "(" + std::to_string(r.x) + "," + std::to_string(r.y) + "," +
           std::to_string(r.w) + "x" + std::to_string(r.h) + ")";
}

std::string dock_placement_summary(const Document::DockPlacement& p) {
    if (!p.present) return "none";
    if (p.floating) {
        return "float rect=(" + std::to_string(p.x) + "," +
               std::to_string(p.y) + "," + std::to_string(p.w) + "x" +
               std::to_string(p.h) + ")";
    }
    return "dock parent=" + (p.parent.empty() ? std::string("__document__")
                                               : p.parent) +
           " side=" + dock_side_name(p.side) +
           (p.size > 0 ? " size=" + std::to_string(p.size) : "");
}

std::string sorted_join(std::vector<std::string> parts,
                        std::string_view sep) {
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) out += sep;
        out += p;
    }
    return out;
}
}  // namespace detail
namespace {

struct DockGraphRel {
    bool floating{false};
    std::string parent;
    int side{0};
};

void add_rendered_dock_relations(
    detail::DocumentImpl& impl,
    std::unordered_map<std::string, DockGraphRel>& rels) {
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* pane = detail::element_for_block(impl, i);
        if (!pane || !detail::class_list_contains(pane, "dcs-dockpane")) continue;
        const std::string id = detail::pane_panel_id(pane);
        if (id.empty() || id == "__document__") continue;

        if (detail::attr_string(pane, "data-aui-dock-floating") == "true") {
            rels[id] = DockGraphRel{true, {}, 0};
        } else {
            const std::string side = detail::attr_string(pane, "data-aui-dock-side");
            if (!side.empty()) {
                rels[id] = DockGraphRel{
                    false,
                    detail::attr_string(pane, "data-aui-dock-parent").empty()
                        ? std::string("__document__")
                        : detail::attr_string(pane, "data-aui-dock-parent"),
                    detail::int_attr(pane, "data-aui-dock-side", 0)};
            }
        }

        const auto tabs = dockpane_tab_ids(impl, pane);
        for (const auto& tab_id : tabs) {
            if (tab_id.empty() || tab_id == id) continue;
            rels[tab_id] = DockGraphRel{false, id, 4};
        }
    }
}

void apply_override_dock_relations(
    const detail::DocumentImpl& impl,
    std::unordered_map<std::string, DockGraphRel>& rels) {
    for (const auto& [id, p] : impl.dock_overrides) {
        if (!p.present) continue;
        if (p.floating) {
            rels[id] = DockGraphRel{true, {}, 0};
        } else {
            rels[id] = DockGraphRel{
                false,
                p.parent.empty() ? std::string("__document__") : p.parent,
                p.side};
        }
    }
}

std::string dock_graph_warning_summary(
    const std::unordered_map<std::string, DockGraphRel>& rels) {
    std::vector<std::string> warnings;
    for (const auto& [id, rel] : rels) {
        if (rel.floating) continue;
        std::vector<std::string> path;
        std::string cur = id;
        for (int depth = 0; depth < 32; ++depth) {
            const auto it = rels.find(cur);
            if (it == rels.end() || it->second.floating ||
                it->second.parent.empty() ||
                it->second.parent == "__document__") {
                if (it != rels.end() && !it->second.floating &&
                    !it->second.parent.empty() &&
                    it->second.parent != "__document__" &&
                    rels.find(it->second.parent) == rels.end()) {
                    warnings.push_back("missing-parent:" + cur + "->" +
                                       it->second.parent);
                }
                break;
            }
            const auto hit =
                std::find(path.begin(), path.end(), cur);
            if (hit != path.end()) {
                std::string cycle;
                for (auto ci = hit; ci != path.end(); ++ci) {
                    if (!cycle.empty()) cycle += "->";
                    cycle += *ci;
                }
                if (!cycle.empty()) cycle += "->" + cur;
                warnings.push_back("cycle:" + cycle);
                break;
            }
            path.push_back(cur);
            cur = it->second.parent;
        }
    }
    if (warnings.empty()) return "none";
    return detail::sorted_join(std::move(warnings), ";");
}

std::string dock_trace_snapshot(detail::DocumentImpl& impl) {
    std::vector<std::string> override_parts;
    override_parts.reserve(impl.dock_overrides.size());
    for (const auto& [id, p] : impl.dock_overrides) {
        override_parts.push_back(id + "=" + detail::dock_placement_summary(p));
    }

    std::vector<std::string> active_parts;
    active_parts.reserve(impl.dock_active_tabs.size());
    for (const auto& [pane, active] : impl.dock_active_tabs) {
        active_parts.push_back(pane + "->" + active);
    }

    std::unordered_map<std::string, DockGraphRel> graph;
    add_rendered_dock_relations(impl, graph);
    apply_override_dock_relations(impl, graph);
    std::vector<std::string> graph_parts;
    graph_parts.reserve(graph.size());
    for (const auto& [id, rel] : graph) {
        graph_parts.push_back(
            id + "->" +
            (rel.floating ? std::string("float")
                          : rel.parent + ":" + dock_side_name(rel.side)));
    }

    std::vector<std::string> rendered_parts;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* pane = detail::element_for_block(impl, i);
        if (!pane || !detail::class_list_contains(pane, "dcs-dockpane")) continue;
        const std::string id = detail::pane_panel_id(pane);
        if (id.empty()) continue;
        const auto& b = impl.blocks[static_cast<std::size_t>(i)];
        std::string placement;
        if (id == "__document__") {
            placement = "document";
        } else if (detail::attr_string(pane, "data-aui-dock-floating") == "true") {
            placement = "float";
        } else if (!detail::attr_string(pane, "data-aui-dock-side").empty()) {
            const std::string parent = detail::attr_string(pane, "data-aui-dock-parent");
            placement = (parent.empty() ? std::string("__document__")
                                        : parent) +
                        ":" + dock_side_name(
                            detail::int_attr(pane, "data-aui-dock-side", 0));
        } else {
            placement = "unplaced";
        }
        rendered_parts.push_back(
            id + "@" + detail::dock_rect_summary(b.bounds) + ":" + placement +
            ":tabs=[" + detail::sorted_join(dockpane_tab_ids(impl, pane), "|") + "]");
    }

    return "overrides={" + detail::sorted_join(std::move(override_parts), ";") +
           "} active={" + detail::sorted_join(std::move(active_parts), ";") +
           "} graph={" + detail::sorted_join(std::move(graph_parts), ";") +
           "} warnings={" + dock_graph_warning_summary(graph) +
           "} rendered={" + detail::sorted_join(std::move(rendered_parts), ";") + "}";
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void dock_trace_state(detail::DocumentImpl& impl, std::string_view reason) {
    if (!detail::dock_trace_enabled()) return;
    const std::string snapshot = dock_trace_snapshot(impl);
    impl.last_dock_trace_signature = snapshot;
    detail::dock_trace("state reason=" + std::string(reason) + " " + snapshot);
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void dock_trace_state_if_changed(detail::DocumentImpl& impl,
                                 std::string_view reason) {
    if (!detail::dock_trace_enabled()) return;
    const std::string snapshot = dock_trace_snapshot(impl);
    if (snapshot == impl.last_dock_trace_signature) return;
    impl.last_dock_trace_signature = snapshot;
    detail::dock_trace("state reason=" + std::string(reason) + " " + snapshot);
}
}  // namespace detail
namespace {

Document::DockPlacement source_placement_for_pane(lxb_dom_element_t* pane,
                                                  const Rect& bounds) {
    Document::DockPlacement p;
    if (!pane) return p;
    if (detail::attr_string(pane, "data-aui-dock-floating") == "true") {
        p.present = true;
        p.floating = true;
        p.x = detail::int_attr(pane, "data-aui-dock-x", bounds.x);
        p.y = detail::int_attr(pane, "data-aui-dock-y", bounds.y);
        p.w = detail::int_attr(pane, "data-aui-dock-w", bounds.w);
        p.h = detail::int_attr(pane, "data-aui-dock-h", bounds.h);
        return p;
    }
    if (!detail::has_attr(pane, "data-aui-dock-side")) return p;
    p.present = true;
    p.floating = false;
    p.parent = detail::attr_string(pane, "data-aui-dock-parent");
    p.side = detail::int_attr(pane, "data-aui-dock-side", 0);
    if (p.side == 0 || p.side == 1) {
        p.size = bounds.w;
    } else if (p.side == 2 || p.side == 3) {
        p.size = bounds.h;
    }
    return p;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void capture_tab_drag_metadata(detail::DocumentImpl& impl,
                               detail::DocumentImpl::TabDrag& drag,
                               lxb_dom_element_t* tab,
                               lxb_dom_element_t* pane) {
    drag.source_pane_id = detail::pane_panel_id(pane);
    drag.drag_kind = detail::dock_kind_of(pane);
    drag.label.clear();
    if (tab) {
        drag.label = std::string(
            detail::trim_css_ws(detail::node_text(lxb_dom_interface_node(tab))));
    }
    drag.source_tab_ids = dockpane_tab_ids(impl, pane);
    drag.source_placement = {};
    drag.source_pane_bounds = {};
    drag.source_pane_bounds_valid = false;
    if (pane) {
        const int pi = detail::block_index_for_exact_element(impl, pane);
        if (pi >= 0) {
            drag.source_pane_bounds =
                impl.blocks[static_cast<std::size_t>(pi)].bounds;
            drag.source_pane_bounds_valid =
                drag.source_pane_bounds.w > 0 && drag.source_pane_bounds.h > 0;
            drag.source_placement =
                source_placement_for_pane(pane, drag.source_pane_bounds);
        }
    }
}

bool arm_tab_drag_from_pending_press(
    detail::DocumentImpl& impl,
    const detail::DocumentImpl::PendingTabPress& press) {
    auto* tab = detail::find_dockpane_tab_for_panel_id(impl, press.panel_id);
    if (!tab) return false;
    auto* pane = detail::ancestor_with_class(tab, "dcs-dockpane");
    if (!pane || detail::dock_kind_of(pane) == "documents") return false;
    impl.tab_drag = {};
    impl.tab_drag.tab = tab;
    impl.tab_drag.pane = pane;
    impl.tab_drag.panel_id = press.panel_id;
    impl.tab_drag.start_x = press.start_x;
    impl.tab_drag.start_y = press.start_y;
    impl.tab_drag.switched_on_down = press.switched_on_down;
    detail::capture_tab_drag_metadata(impl, impl.tab_drag, tab, pane);
    return true;
}
}  // namespace detail
namespace {

// Is the point over the pane's own tabbar (its direct-child .dcs-dockpane__tabbar)?
bool point_over_pane_tabbar(detail::DocumentImpl& impl, lxb_dom_element_t* pane,
                            Point pt) {
    for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(pane)); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (!detail::class_list_contains(e, "dcs-dockpane__tabbar")) continue;
        const int bi = detail::block_index_for_exact_element(impl, e);
        if (bi < 0) return false;
        const auto& b = impl.blocks[static_cast<std::size_t>(bi)].bounds;
        return pt.x >= b.x && pt.x < b.x + b.w && pt.y >= b.y && pt.y < b.y + b.h;
    }
    return false;
}

bool is_dockpane_top_chrome(lxb_dom_element_t* elem) {
    return elem &&
           (detail::class_list_contains(elem, "dcs-dockpane__tabbar") ||
            detail::class_list_contains(elem, "dcs-dockpane__titlebar") ||
            detail::class_list_contains(elem, "dcs-dockpane__shelf"));
}

Rect dockpane_zone_bounds(detail::DocumentImpl& impl,
                          lxb_dom_element_t* pane,
                          Rect pane_bounds) {
    if (!pane) return pane_bounds;
    int top = pane_bounds.y;
    const int bottom = pane_bounds.y + pane_bounds.h;
    for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(pane)); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (!is_dockpane_top_chrome(e)) continue;
        const int bi = detail::block_index_for_exact_element(impl, e);
        if (bi < 0) continue;
        const auto& cb = impl.blocks[static_cast<std::size_t>(bi)].bounds;
        if (cb.w <= 0 || cb.h <= 0) continue;
        if (cb.y <= top + 2 && cb.y + cb.h > top) {
            top = std::min(bottom, std::max(top, cb.y + cb.h));
        }
    }
    if (top >= bottom) return pane_bounds;
    pane_bounds.y = top;
    pane_bounds.h = bottom - top;
    return pane_bounds;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
DropTarget compute_drop_target(detail::DocumentImpl& impl, Point pt,
                               std::string_view drag_kind,
                               lxb_dom_element_t* source_pane) {
    DropTarget out;
    int hx = 0, hy = 0, hw = 0, hh = 0;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* e = detail::element_for_block(impl, i);
        if (e && detail::class_list_contains(e, "dcs-dock--floathost")) {
            const auto& hb = impl.blocks[static_cast<std::size_t>(i)].bounds;
            hx = hb.x; hy = hb.y; hw = hb.w; hh = hb.h;
            break;
        }
    }
    if (hw <= 0) return out;
    if (!(pt.x >= hx && pt.x < hx + hw && pt.y >= hy && pt.y < hy + hh))
        return out;  // outside the dock area -> no target (caller tears off)

    // (Ke) window-edge: JS checks the browser viewport, not the dock workarea.
    // Keep the preview rect root-relative, but do the gesture band against the
    // viewport so chrome below/above the dock does not make panel edges vanish.
    constexpr int kWin = 32;
    const int vw = detail::viewport_width_for_overlay(impl);
    const int vh = detail::viewport_height_for_overlay(impl);
    DropTarget window_edge;
    if (pt.x < kWin) window_edge.zone = DropZone::Left;
    else if (pt.x > vw - kWin) window_edge.zone = DropZone::Right;
    else if (pt.y < kWin) window_edge.zone = DropZone::Top;
    else if (pt.y > vh - kWin) window_edge.zone = DropZone::Bottom;
    if (window_edge.zone != DropZone::None) {
        window_edge.parent = "__document__";
        window_edge.valid = true;
        window_edge.window_edge = true;
        // Window-edge drops target the WORKSPACE ROOT dock, never an inner
        // dock: dock_split's wrapper branch then wraps the ENTIRE workspace,
        // so the new pane spans the full orthogonal axis of that window side
        // (a right-edge drop = a full-height column from the workarea's top
        // to its bottom, crossing every inner row). That span cannot be
        // built by splitting inner panes — this gesture is the only way.
        lxb_dom_element_t* workroot = nullptr;
        if (auto* host = find_first_descendant_with_class(
                detail::document_dom_root(impl), "dcs-dock--floathost")) {
            if (detail::class_list_contains(host, "dcs-dock")) {
                workroot = host;
            } else {
                for (auto* c = lxb_dom_node_first_child(
                         lxb_dom_interface_node(host));
                     c; c = lxb_dom_node_next(c)) {
                    if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                    auto* ce = lxb_dom_interface_element(c);
                    if (detail::class_list_contains(ce, "dcs-dock")) {
                        workroot = ce;
                        break;
                    }
                }
            }
        }
        window_edge.pane =
            workroot ? workroot : edge_owner_dock(impl, window_edge.zone);
        const int sw = hw * 30 / 100, sh = hh * 30 / 100;
        switch (window_edge.zone) {
            case DropZone::Left:   window_edge.x = 0;       window_edge.y = 0;       window_edge.w = sw; window_edge.h = hh; break;
            case DropZone::Right:  window_edge.x = hw - sw; window_edge.y = 0;       window_edge.w = sw; window_edge.h = hh; break;
            case DropZone::Top:    window_edge.x = 0;       window_edge.y = 0;       window_edge.w = hw; window_edge.h = sh; break;
            case DropZone::Bottom: window_edge.x = 0;       window_edge.y = hh - sh; window_edge.w = hw; window_edge.h = sh; break;
            default: break;
        }
    }

    // (Ne) the dock pane under the cursor (same dock-kind). Mirror JS
    // elementFromPoint(...).closest('.dcs-dockpane') instead of scanning all
    // pane rectangles; otherwise stale/overlapping bounds can make a visually
    // unrelated pane own the preview.
    const int hit = detail::hit_test_blocks_for_dock_target(impl, pt.x, pt.y);
    auto* hit_elem = detail::element_for_block_or_ancestor(impl, hit);
    auto* e = detail::ancestor_with_class(hit_elem, "dcs-dockpane");
    detail::dock_trace("drop-probe at=(" + std::to_string(pt.x) + "," +
               std::to_string(pt.y) + ") hit=" + std::to_string(hit) +
               " pane=" + (e ? detail::pane_panel_id(e) : std::string("<none>")) +
               " kind=" + (e ? detail::dock_kind_of(e) : std::string()) +
               " drag_kind=" + std::string(drag_kind));
    if (e) {
        const int bi = detail::block_index_for_exact_element(impl, e);
        if (bi < 0) return window_edge.valid ? window_edge : out;
        const auto& b = impl.blocks[static_cast<std::size_t>(bi)].bounds;
        if (!(pt.x >= b.x && pt.x < b.x + b.w && pt.y >= b.y &&
              pt.y < b.y + b.h)) {
            return window_edge.valid ? window_edge : out;
        }
        // decius dockDropDecision: kinds must match. The source pane IS a
        // target for its own tab: hovering it gives visual feedback ("drop
        // back where it was") and releasing is a no-op — for a single-tab
        // pane the WHOLE pane is that center target. Extension (upstreamed
        // to decius.js too): a MULTI-tab pane's edge zones ARE valid for its
        // own tab — "split Console out of Assets" in one gesture.
        const bool self_drop = e == source_pane;
        if (detail::dock_kind_of(e) != drag_kind) {
            return window_edge.valid ? window_edge : out;
        }
        // The pane ELEMENT is the target (decius.js semantics); the id is
        // best-effort metadata — an app-authored pane with no derivable id
        // is still a perfectly good dock target.
        out.parent = detail::pane_panel_id(e);
        out.pane = e;
        const int lx = b.x - hx, ly = b.y - hy;
        if (detail::ancestor_with_class(e, "dcs-panel--floating") ||
            point_over_pane_tabbar(impl, e, pt) ||
            (self_drop && detail::dock_tabs(e).size() <= 1)) {
            // Floaters and single-tab self-drops are whole-pane center
            // targets (edge-splitting a single-tab pane out of itself is
            // meaningless; the feedback reads "drop back where it was").
            out.zone = DropZone::Tab;
        } else {
            // Edge intent mirrors decius.js edgeZone(): a percentage band
            // decides which edges are eligible, then the closest eligible edge
            // is chosen in pixels. That keeps top/bottom zones usable on very
            // wide shallow shelves, instead of compressing them into a few
            // normalized-distance pixels near the midpoint. The tabbar/title
            // chrome is already a center-tab target, so edge zones start at the
            // stable content slot below chrome; active tab bodies may scroll,
            // overflow, or be lazily recreated and should not warp the zones.
            constexpr double kT = 0.22;
            Rect zone_bounds = dockpane_zone_bounds(impl, e, b);
            const double dx = pt.x - zone_bounds.x;
            const double dy = pt.y - zone_bounds.y;
            const double w = std::max(1, zone_bounds.w);
            const double h = std::max(1, zone_bounds.h);
            const double x_band = w * kT;
            const double y_band = h * kT;
            const bool near_l = dx < x_band;
            const bool near_r = dx > w - x_band;
            const bool near_t = dy < y_band;
            const bool near_b = dy > h - y_band;
            const double inf = std::numeric_limits<double>::infinity();
            const double dists[] = {
                near_l ? dx : inf,
                near_r ? w - dx : inf,
                near_t ? dy : inf,
                near_b ? h - dy : inf,
            };
            const DropZone zones[] = {DropZone::Left, DropZone::Right,
                                      DropZone::Top, DropZone::Bottom};
            double best = inf;
            out.zone = DropZone::Tab;
            for (int zi = 0; zi < 4; ++zi) {
                if (dists[zi] < best) {
                    best = dists[zi];
                    out.zone = zones[zi];
                }
            }
        }
        // Center on your OWN multi-tab pane is a valid CENTER target (the
        // preview shows so the gesture reads as "drop back where it was"), but
        // releasing is a no-op — the tab is already a member here. (Edge zones
        // on the source pane DO split it out; see the kind/size guard above.)
        if (self_drop && out.zone == DropZone::Tab) out.self_noop = true;
        out.valid = true;
        if (out.zone == DropZone::Tab && window_edge.valid) return window_edge;
        switch (out.zone) {
            case DropZone::Left:   out.x = lx;             out.y = ly;             out.w = b.w / 2; out.h = b.h;     break;
            case DropZone::Right:  out.x = lx + b.w - b.w / 2; out.y = ly;         out.w = b.w / 2; out.h = b.h;     break;
            case DropZone::Top:    out.x = lx;             out.y = ly;             out.w = b.w;     out.h = b.h / 2; break;
            case DropZone::Bottom: out.x = lx;             out.y = ly + b.h - b.h / 2; out.w = b.w; out.h = b.h / 2; break;
            default:               out.x = lx;             out.y = ly;             out.w = b.w;     out.h = b.h;     break;
        }
        return out;
    }
    if (window_edge.valid) return window_edge;
    return out;
}

// Position (and show) or hide the drop indicator. Reuses decius .dcs-drop--valid
// (accent inset outline + dim fill); the rect is float-host-relative.
bool set_drop_indicator(detail::DocumentImpl& impl, const DropTarget* t) {
    auto* ind = detail::find_dom_element_by_id(impl, "__dropind");
    if (!ind) return false;
    if (!t || !t->valid) {
        bool changed = detail::set_attribute_on_element(impl, ind, "hidden", "");
        changed = detail::set_attribute_on_element(
                      impl, ind, "style",
                      "position:absolute;pointer-events:none;z-index:200;"
                      "display:none;left:0px;top:0px;width:0px;height:0px") ||
                  changed;
        return changed;
    }
    bool changed = detail::set_attribute_on_element(
        impl, ind, "style",
        "position:absolute;pointer-events:none;z-index:200;border:2px solid "
        "rgb(0,184,212);background:rgba(0,184,212,0.18);"
        "box-sizing:border-box;left:" +
            std::to_string(t->x) + "px;top:" + std::to_string(t->y) +
            "px;width:" + std::to_string(t->w) + "px;height:" +
            std::to_string(t->h) + "px");
    changed = detail::remove_attribute_on_element(impl, ind, "hidden") || changed;
    return changed;
}
}  // namespace detail
namespace {

std::string primary_drag_reanchor_anchor(
    std::string_view panel_id,
    const detail::DocumentImpl::TabDrag* drag) {
    if (!drag || panel_id.empty() || panel_id != drag->source_pane_id ||
        drag->source_tab_ids.size() <= 1 || !drag->source_placement.present) {
        return {};
    }
    for (const auto& id : drag->source_tab_ids) {
        if (id != panel_id) return id;
    }
    return {};
}

bool reanchor_tabs_left_by_primary_drag(
    detail::DocumentImpl& impl,
    std::string_view panel_id,
    const detail::DocumentImpl::TabDrag* drag) {
    const std::string anchor = primary_drag_reanchor_anchor(panel_id, drag);
    if (anchor.empty()) return false;

    std::vector<std::string> remaining;
    remaining.reserve(drag->source_tab_ids.size() - 1);
    for (const auto& id : drag->source_tab_ids) {
        if (id != panel_id) remaining.push_back(id);
    }

    auto anchor_placement = drag->source_placement;
    if (!anchor_placement.floating && anchor_placement.parent == anchor) {
        anchor_placement.parent = "__document__";
    }
    impl.dock_overrides[anchor] = anchor_placement;
    impl.dock_active_tabs.erase(std::string(panel_id));
    impl.dock_active_tabs.erase(anchor);

    for (std::size_t i = 1; i < remaining.size(); ++i) {
        Document::DockPlacement tab;
        tab.present = true;
        tab.floating = false;
        tab.parent = anchor;
        tab.side = 4;
        impl.dock_overrides[remaining[i]] = tab;
    }

    detail::dock_trace("dock-reanchor source=" + std::string(panel_id) +
               " anchor=" + anchor +
               " tabs=" + std::to_string(remaining.size()));
    return true;
}

std::unordered_map<std::string, DockGraphRel> current_dock_graph(
    detail::DocumentImpl& impl) {
    std::unordered_map<std::string, DockGraphRel> graph;
    add_rendered_dock_relations(impl, graph);
    apply_override_dock_relations(impl, graph);
    return graph;
}

bool dock_graph_reaches(
    const std::unordered_map<std::string, DockGraphRel>& graph,
    std::string_view from,
    std::string_view target) {
    if (from.empty() || target.empty()) return false;
    std::string cur(from);
    for (int depth = 0; depth < 32; ++depth) {
        const auto it = graph.find(cur);
        if (it == graph.end() || it->second.floating ||
            it->second.parent.empty() ||
            it->second.parent == "__document__") {
            return false;
        }
        if (it->second.parent == target) return true;
        cur = it->second.parent;
    }
    return false;
}

void prune_stale_dock_active_tabs(detail::DocumentImpl& impl) {
    const auto graph = current_dock_graph(impl);
    for (auto it = impl.dock_active_tabs.begin();
         it != impl.dock_active_tabs.end();) {
        const auto rel = graph.find(it->second);
        if (rel == graph.end() || rel->second.floating ||
            rel->second.side != 4 || rel->second.parent != it->first) {
            it = impl.dock_active_tabs.erase(it);
        } else {
            ++it;
        }
    }
}

bool reparent_descendant_target_out_of_dragged_subtree(
    detail::DocumentImpl& impl,
    std::string_view panel_id,
    std::string_view target_id,
    const detail::DocumentImpl::TabDrag* drag) {
    if (!drag || panel_id.empty() || target_id.empty() ||
        !drag->source_placement.present) {
        return false;
    }
    const auto graph = current_dock_graph(impl);
    if (!dock_graph_reaches(graph, target_id, panel_id)) return false;

    auto target_slot = drag->source_placement;
    if (!target_slot.floating && target_slot.parent == target_id) {
        target_slot.parent = "__document__";
    }
    impl.dock_overrides[std::string(target_id)] = target_slot;
    detail::dock_trace("dock-cycle-break moving=" + std::string(panel_id) +
               " target=" + std::string(target_id) +
               " target-slot=" + detail::dock_placement_summary(target_slot));
    return true;
}

// Record a docked placement override: dock `panel_id` to a side of (or as a tab
// of) the target pane. The app re-resolves the layout on rebuild.
bool apply_dock(detail::DocumentImpl& impl, std::string_view panel_id,
                const DropTarget& t,
                const detail::DocumentImpl::TabDrag* drag = nullptr) {
    if (panel_id.empty() || !t.valid || t.zone == DropZone::None) return false;
    if (t.parent.empty()) {
        detail::dock_trace("dock-noop panel=" + std::string(panel_id) +
                   " target=" + detail::drop_target_summary(t));
        return false;
    }
    std::string parent = t.parent;
    if (parent == panel_id) {
        if (t.zone == DropZone::Tab) {
            detail::dock_trace("dock-noop panel=" + std::string(panel_id) +
                       " target=" + detail::drop_target_summary(t));
            return false;
        }
        parent = primary_drag_reanchor_anchor(panel_id, drag);
        if (parent.empty()) {
            detail::dock_trace("dock-noop panel=" + std::string(panel_id) +
                       " target=" + detail::drop_target_summary(t));
            return false;
        }
    }
    Document::DockPlacement p;
    p.present = true;
    p.floating = false;
    p.parent = parent;
    switch (t.zone) {
        case DropZone::Left:   p.side = 0; break;
        case DropZone::Right:  p.side = 1; break;
        case DropZone::Top:    p.side = 2; break;
        case DropZone::Bottom: p.side = 3; break;
        case DropZone::Tab:    p.side = 4; break;
        default: return false;
    }
    const std::string key(panel_id);
    if (const auto it = impl.dock_overrides.find(key);
        it != impl.dock_overrides.end()) {
        const auto& old = it->second;
        if (old.present && !old.floating && old.parent == p.parent &&
            old.side == p.side && old.size == p.size) {
            detail::dock_trace("dock-noop-same panel=" + key +
                       " parent=" + p.parent +
                       " side=" + std::to_string(p.side));
            return false;
        }
    }
    reanchor_tabs_left_by_primary_drag(impl, panel_id, drag);
    reparent_descendant_target_out_of_dragged_subtree(impl, panel_id, parent,
                                                      drag);
    impl.dock_overrides[key] = p;
    if (p.side == 4) {
        impl.dock_active_tabs[p.parent] = key;
    }
    prune_stale_dock_active_tabs(impl);
    detail::dock_trace("dock panel=" + key + " parent=" + p.parent +
               " side=" + std::to_string(p.side) +
               " zone=" + detail::drop_zone_name(t.zone));
    detail::dock_trace_state(impl, "after-dock");
    return true;
}

int dcs_tree_row_depth(lxb_dom_element_t* row) {
    const std::string depth_value =
        detail::find_decl_value(detail::attr_string(row, "style"), "--depth");
    if (depth_value.empty()) return 0;
    char* end = nullptr;
    const long parsed = std::strtol(depth_value.c_str(), &end, 10);
    if (end == depth_value.c_str()) return 0;
    return static_cast<int>(std::clamp<long>(parsed, 0, 128));
}

void collect_dcs_tree_rows(lxb_dom_element_t* elem,
                           std::vector<lxb_dom_element_t*>& rows) {
    if (!elem) return;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* child_elem = lxb_dom_interface_element(child);
        if (detail::class_list_contains(child_elem, "dcs-tree__row")) {
            rows.push_back(child_elem);
        }
        collect_dcs_tree_rows(child_elem, rows);
    }
}

bool dcs_tree_row_has_direct_child(
    const std::vector<lxb_dom_element_t*>& rows,
    std::size_t row_index,
    int depth) {
    for (std::size_t i = row_index + 1; i < rows.size(); ++i) {
        const int child_depth = dcs_tree_row_depth(rows[i]);
        if (child_depth <= depth) return false;
        if (child_depth == depth + 1) return true;
    }
    return false;
}

bool refresh_dcs_tree_visibility(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* tree) {
    std::vector<lxb_dom_element_t*> rows;
    collect_dcs_tree_rows(tree, rows);
    if (rows.empty()) return false;

    std::vector<bool> open_by_depth(129, true);
    bool changed = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto* row = rows[i];
        const int depth = dcs_tree_row_depth(row);
        bool visible = true;
        for (int d = 0; d < depth && d < static_cast<int>(open_by_depth.size());
             ++d) {
            if (!open_by_depth[static_cast<std::size_t>(d)]) {
                visible = false;
                break;
            }
        }

        changed = visible ? (detail::remove_attribute_on_element(impl, row, "hidden") ||
                             changed)
                          : (detail::set_attribute_on_element(impl, row, "hidden", "") ||
                             changed);

        if (auto* chevron =
                detail::first_descendant_with_class(row, "dcs-tree__chevron")) {
            const bool has_child =
                dcs_tree_row_has_direct_child(rows, i, depth);
            changed = detail::set_attribute_on_element(
                          impl, chevron, "class",
                          detail::class_list_set(chevron, "dcs-tree__chevron--leaf",
                                         !has_child)) ||
                      changed;
            open_by_depth[static_cast<std::size_t>(depth)] =
                has_child && detail::class_list_contains(
                                 chevron, "dcs-tree__chevron--open");
        } else {
            open_by_depth[static_cast<std::size_t>(depth)] = true;
        }
        for (std::size_t d = static_cast<std::size_t>(depth + 1);
             d < open_by_depth.size(); ++d) {
            open_by_depth[d] = true;
        }
    }
    return changed;
}

bool find_dcs_tree_chevron_at(detail::DocumentImpl& impl,
                              int from_idx,
                              lxb_dom_element_t*& out_tree,
                              lxb_dom_element_t*& out_row,
                              lxb_dom_element_t*& out_chevron) {
    out_tree = nullptr;
    out_row = nullptr;
    out_chevron = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        if (detail::class_list_contains(elem, "dcs-tree__chevron")) {
            if (detail::class_list_contains(elem, "dcs-tree__chevron--leaf")) {
                return false;
            }
            out_chevron = elem;
        }
        if (out_chevron && !out_row &&
            detail::class_list_contains(elem, "dcs-tree__row")) {
            out_row = elem;
        }
        if (out_chevron && out_row &&
            detail::class_list_contains(elem, "dcs-tree")) {
            out_tree = elem;
            return true;
        }
    }
    return false;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool toggle_dcs_tree_chevron_control(detail::DocumentImpl& impl, int from_idx) {
    lxb_dom_element_t* tree = nullptr;
    lxb_dom_element_t* row = nullptr;
    lxb_dom_element_t* chevron = nullptr;
    if (!find_dcs_tree_chevron_at(impl, from_idx, tree, row, chevron)) {
        return false;
    }

    std::vector<lxb_dom_element_t*> rows;
    collect_dcs_tree_rows(tree, rows);
    const auto row_it = std::find(rows.begin(), rows.end(), row);
    if (row_it == rows.end()) return false;
    if (!dcs_tree_row_has_direct_child(
            rows, static_cast<std::size_t>(row_it - rows.begin()),
            dcs_tree_row_depth(row))) {
        return false;
    }

    const bool open =
        detail::class_list_contains(chevron, "dcs-tree__chevron--open");
    bool changed = detail::set_attribute_on_element(
        impl, chevron, "class",
        detail::class_list_set(chevron, "dcs-tree__chevron--open", !open));
    changed = refresh_dcs_tree_visibility(impl, tree) || changed;
    detail::emit_widget_change(impl, tree, !open ? "open" : "closed");
    return changed;
}
}  // namespace detail
namespace {

using TreeDropZone = detail::DocumentImpl::TreeDrag::Zone;

std::string_view dcs_tree_drop_class(TreeDropZone zone) {
    switch (zone) {
        case TreeDropZone::Before:
            return "dcs-tree__row--drop-before";
        case TreeDropZone::After:
            return "dcs-tree__row--drop-after";
        case TreeDropZone::Into:
            return "dcs-tree__row--drop-into";
        case TreeDropZone::None:
            break;
    }
    return {};
}

bool clear_dcs_tree_drop_classes(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* row) {
    if (!row) return false;
    bool changed = false;
    for (std::string_view cls :
         {"dcs-tree__row--drop-before", "dcs-tree__row--drop-after",
          "dcs-tree__row--drop-into"}) {
        changed = detail::set_element_class(impl, row, cls, false) || changed;
    }
    return changed;
}

void clear_dcs_tree_drop_classes_raw(lxb_dom_element_t* row) {
    if (!row) return;
    for (std::string_view cls :
         {"dcs-tree__row--drop-before", "dcs-tree__row--drop-after",
          "dcs-tree__row--drop-into"}) {
        dock_set_class(row, cls, false);
    }
}

bool clear_dcs_tree_drop_highlight(detail::DocumentImpl& impl) {
    bool changed = clear_dcs_tree_drop_classes(impl, impl.tree_drag.target);
    impl.tree_drag.target = nullptr;
    impl.tree_drag.zone = TreeDropZone::None;
    return changed;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool find_dcs_tree_row_at(detail::DocumentImpl& impl,
                          int from_idx,
                          lxb_dom_element_t*& out_tree,
                          lxb_dom_element_t*& out_row) {
    out_tree = nullptr;
    out_row = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        if (!out_row && detail::class_list_contains(elem, "dcs-tree__row")) {
            out_row = elem;
        }
        if (out_row && detail::class_list_contains(elem, "dcs-tree")) {
            out_tree = elem;
            return true;
        }
    }
    return false;
}
}  // namespace detail
namespace {

lxb_dom_element_t* find_dcs_tree_row_at_point(detail::DocumentImpl& impl,
                                              lxb_dom_element_t* tree,
                                              Point point) {
    if (!tree) return nullptr;
    const int tree_idx = detail::block_index_for_exact_element(impl, tree);
    if (tree_idx < 0) return nullptr;
    const Rect tree_rect = detail::block_border_visual_rect(impl, tree_idx);

    std::vector<lxb_dom_element_t*> rows;
    collect_dcs_tree_rows(tree, rows);
    for (auto* row : rows) {
        if (!row || detail::has_attr(row, "hidden")) continue;
        const int row_idx = detail::block_index_for_exact_element(impl, row);
        if (row_idx < 0) continue;
        const Rect row_rect = detail::block_border_visual_rect(impl, row_idx);
        if (row_rect.h <= 0) continue;
        const int left =
            tree_rect.w > 0 ? std::min(tree_rect.x, row_rect.x) : row_rect.x;
        const int right = tree_rect.w > 0
            ? std::max(tree_rect.x + tree_rect.w, row_rect.x + row_rect.w)
            : row_rect.x + row_rect.w;
        if (point.y >= row_rect.y && point.y < row_rect.y + row_rect.h &&
            point.x >= left && point.x < right) {
            return row;
        }
    }
    return nullptr;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool dcs_tree_row_draggable(lxb_dom_element_t* row) {
    if (!row) return false;
    if (detail::attr_string(row, "draggable") == "false") return false;
    if (detail::attr_string(row, "aria-disabled") == "true") return false;
    return true;
}
}  // namespace detail
namespace {

std::vector<lxb_dom_element_t*> dcs_tree_row_subtree(
    lxb_dom_element_t* row) {
    std::vector<lxb_dom_element_t*> out;
    if (!row || !detail::class_list_contains(row, "dcs-tree__row")) return out;
    const int root_depth = dcs_tree_row_depth(row);
    for (auto* cur = row; cur != nullptr;
         cur = detail::next_element_sibling(lxb_dom_interface_node(cur))) {
        if (!detail::class_list_contains(cur, "dcs-tree__row")) break;
        if (cur != row && dcs_tree_row_depth(cur) <= root_depth) break;
        out.push_back(cur);
    }
    return out;
}

bool dcs_tree_subtree_contains(
    const std::vector<lxb_dom_element_t*>& subtree,
    lxb_dom_element_t* row) {
    return std::find(subtree.begin(), subtree.end(), row) != subtree.end();
}

TreeDropZone dcs_tree_drop_zone_for_point(
    detail::DocumentImpl& impl,
    lxb_dom_element_t* row,
    Point point) {
    const int idx = detail::block_index_for_exact_element(impl, row);
    if (idx < 0) return TreeDropZone::Into;
    const Rect bounds = detail::block_border_visual_rect(impl, idx);
    if (bounds.h <= 0) return TreeDropZone::Into;
    const double y = static_cast<double>(point.y - bounds.y);
    if (y < bounds.h * 0.3) return TreeDropZone::Before;
    if (y > bounds.h * 0.7) return TreeDropZone::After;
    return TreeDropZone::Into;
}

bool set_dcs_tree_drop_highlight(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* target,
                                 TreeDropZone zone) {
    if (!target || zone == TreeDropZone::None) {
        return clear_dcs_tree_drop_highlight(impl);
    }
    if (impl.tree_drag.target == target && impl.tree_drag.zone == zone) {
        return false;
    }
    bool changed = clear_dcs_tree_drop_highlight(impl);
    impl.tree_drag.target = target;
    impl.tree_drag.zone = zone;
    changed = detail::set_element_class(impl, target, dcs_tree_drop_class(zone),
                                true) ||
              changed;
    return changed;
}

void shift_dcs_tree_subtree_depth_raw(
    const std::vector<lxb_dom_element_t*>& subtree,
    int delta) {
    if (delta == 0) return;
    for (auto* row : subtree) {
        const int next_depth = std::max(0, dcs_tree_row_depth(row) + delta);
        dock_set_attr(row, "style",
                      detail::style_with_properties(
                          detail::attr_string(row, "style"),
                          {{"--depth", std::to_string(next_depth)}}));
    }
}

void refresh_dcs_tree_visibility_raw(lxb_dom_element_t* tree) {
    std::vector<lxb_dom_element_t*> rows;
    collect_dcs_tree_rows(tree, rows);
    if (rows.empty()) return;

    std::vector<bool> open_by_depth(129, true);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto* row = rows[i];
        const int depth = dcs_tree_row_depth(row);
        bool visible = true;
        for (int d = 0; d < depth && d < static_cast<int>(open_by_depth.size());
             ++d) {
            if (!open_by_depth[static_cast<std::size_t>(d)]) {
                visible = false;
                break;
            }
        }
        if (visible) {
            lxb_dom_element_remove_attribute(row, detail::as_lxb("hidden"), 6);
        } else {
            dock_set_attr(row, "hidden", "");
        }

        if (auto* chevron =
                detail::first_descendant_with_class(row, "dcs-tree__chevron")) {
            const bool has_child =
                dcs_tree_row_has_direct_child(rows, i, depth);
            dock_set_class(chevron, "dcs-tree__chevron--leaf", !has_child);
            open_by_depth[static_cast<std::size_t>(depth)] =
                has_child &&
                detail::class_list_contains(chevron, "dcs-tree__chevron--open");
        } else {
            open_by_depth[static_cast<std::size_t>(depth)] = true;
        }
        for (std::size_t d = static_cast<std::size_t>(depth + 1);
             d < open_by_depth.size(); ++d) {
            open_by_depth[d] = true;
        }
    }
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool update_dcs_tree_drag(detail::DocumentImpl& impl, const Event& ev) {
    auto& drag = impl.tree_drag;
    if (!drag.row || !drag.tree) return false;
    bool changed = false;
    if (!drag.dragging) {
        const int dx = ev.pos.x - drag.start_x;
        const int dy = ev.pos.y - drag.start_y;
        if (dx * dx + dy * dy <= 36) return false;
        drag.dragging = true;
        changed = detail::set_element_class(impl, drag.row,
                                    "dcs-tree__row--draggable", true) ||
                  changed;
    }

    lxb_dom_element_t* target =
        find_dcs_tree_row_at_point(impl, drag.tree, ev.pos);
    if (!target || target == drag.row) {
        return clear_dcs_tree_drop_highlight(impl) || changed;
    }
    const auto source_subtree = dcs_tree_row_subtree(drag.row);
    if (dcs_tree_subtree_contains(source_subtree, target)) {
        return clear_dcs_tree_drop_highlight(impl) || changed;
    }
    changed = set_dcs_tree_drop_highlight(
                  impl, target,
                  dcs_tree_drop_zone_for_point(impl, target, ev.pos)) ||
              changed;
    return changed;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool finish_dcs_tree_drag(detail::DocumentImpl& impl, const Event& ev) {
    auto& drag = impl.tree_drag;
    if (!drag.dragging || !drag.row || !drag.tree) return false;

    lxb_dom_element_t* target = drag.target;
    TreeDropZone zone = drag.zone;
    if (auto* point_row = find_dcs_tree_row_at_point(impl, drag.tree, ev.pos);
        point_row && point_row != drag.row) {
        target = point_row;
        zone = dcs_tree_drop_zone_for_point(impl, target, ev.pos);
    }
    if (!target || zone == TreeDropZone::None) return false;

    auto source_subtree = dcs_tree_row_subtree(drag.row);
    if (source_subtree.empty() ||
        dcs_tree_subtree_contains(source_subtree, target)) {
        return false;
    }

    const int target_depth = dcs_tree_row_depth(target);
    const int new_root_depth =
        zone == TreeDropZone::Into ? target_depth + 1 : target_depth;
    const int delta_depth = new_root_depth - dcs_tree_row_depth(drag.row);
    auto target_subtree = dcs_tree_row_subtree(target);
    lxb_dom_node_t* anchor =
        zone == TreeDropZone::Before
            ? lxb_dom_interface_node(target)
            : zone == TreeDropZone::After
                  ? (target_subtree.empty()
                         ? lxb_dom_node_next(lxb_dom_interface_node(target))
                         : lxb_dom_node_next(lxb_dom_interface_node(
                               target_subtree.back())))
                  : lxb_dom_node_next(lxb_dom_interface_node(target));
    auto* parent_node = lxb_dom_node_parent(lxb_dom_interface_node(target));
    if (!parent_node) return false;

    detail::SuppressDomStyleAttach no_eager_attach(impl);
    clear_dcs_tree_drop_classes_raw(drag.target);
    dock_set_class(drag.row, "dcs-tree__row--draggable", false);
    shift_dcs_tree_subtree_depth_raw(source_subtree, delta_depth);
    const bool anchor_in_source =
        anchor && anchor->type == LXB_DOM_NODE_TYPE_ELEMENT &&
        dcs_tree_subtree_contains(source_subtree,
                                  lxb_dom_interface_element(anchor));
    if (!anchor_in_source) {
        for (auto* row : source_subtree) {
            dock_detach_for_move(row);
            if (anchor) {
                lxb_dom_node_insert_before(anchor, lxb_dom_interface_node(row));
            } else {
                lxb_dom_node_insert_child(parent_node,
                                          lxb_dom_interface_node(row));
            }
        }
    }
    refresh_dcs_tree_visibility_raw(drag.tree);
    detail::dock_structure_changed(impl);
    detail::emit_widget_change(impl, drag.tree, "reorder");
    return true;
}

bool cancel_dcs_tree_drag(detail::DocumentImpl& impl) {
    bool changed = clear_dcs_tree_drop_highlight(impl);
    changed = detail::set_element_class(impl, impl.tree_drag.row,
                                "dcs-tree__row--draggable", false) ||
              changed;
    impl.tree_drag = {};
    return changed;
}
}  // namespace detail
namespace {

bool is_open_transient_layer(lxb_dom_element_t* elem) {
    return elem && !detail::has_attr(elem, "hidden") &&
           (detail::class_list_contains(elem, "aui-select__menu") ||
            detail::class_list_contains(elem, "dcs-menu") ||
            detail::class_list_contains(elem, "dcs-popover"));
}

bool point_preserves_transient_layers(detail::DocumentImpl& impl,
                                      Point point) {
    for (std::size_t i = impl.blocks.size(); i-- > 0; ) {
        auto* elem = detail::element_for_block(impl, static_cast<int>(i));
        if (!elem) continue;
        const Rect bounds = detail::block_border_visual_rect(impl, static_cast<int>(i));
        if (bounds.w <= 0 || bounds.h <= 0 ||
            !detail::rect_contains(bounds, point.x, point.y)) {
            continue;
        }
        if (is_open_transient_layer(elem)) return true;
        if (auto* popover = detail::nearest_ancestor_with_class(elem, "dcs-popover");
            popover && !detail::has_attr(popover, "hidden")) {
            return true;
        }
        if (auto* menu = detail::nearest_ancestor_with_class(elem, "dcs-menu");
            menu && !detail::has_attr(menu, "hidden")) {
            return true;
        }
        if (auto* menu =
                detail::nearest_ancestor_with_class(elem, "aui-select__menu");
            menu && !detail::has_attr(menu, "hidden")) {
            return true;
        }
    }
    return false;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool click_preserves_transient_layers(detail::DocumentImpl& impl,
                                      int from_idx,
                                      Point point) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (!elem) continue;
        if (auto* popover = detail::nearest_ancestor_with_class(elem, "dcs-popover");
            popover && !detail::has_attr(popover, "hidden")) {
            return true;
        }
        if (is_open_transient_layer(elem)) return true;
        if (detail::is_dcs_menu_trigger(elem) || detail::is_dcs_popover_trigger(elem)) {
            return true;
        }
        if (detail::attr_string(elem, "data-aui-widget") == "dropdown" ||
            detail::tag_name(elem) == "select") {
            return true;
        }
    }
    return point_preserves_transient_layers(impl, point);
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
lxb_dom_element_t* find_trigger_for_target(detail::DocumentImpl& impl,
                                           std::string_view target_selector) {
    if (target_selector.empty()) return nullptr;
    lxb_dom_element_t* out = nullptr;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (!out && detail::attr_string(elem, "data-dcs-target") == target_selector) {
            out = elem;
        }
    };
    detail::walk_dom_elements(detail::document_dom_root(impl), collect);
    return out;
}
}  // namespace detail
namespace {

}  // namespace

bool Document::take_dock_structure_changed() {
#if !defined(AFFINEUI_STUB_BUILD)
    const bool changed = impl_->dock_structure_dirty;
    impl_->dock_structure_dirty = false;
    return changed;
#else
    return false;
#endif
}

Document::DockLayout Document::dock_layout() const {
    DockLayout out;
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc) return out;
    auto* body = lxb_html_document_body_element(impl_->doc);
    if (!body) return out;
    auto* root_node = lxb_dom_interface_node(body);
    // The workspace host (View emits .dcs-dock--floathost; raw-HTML fixtures
    // may combine it with .dcs-dock). The split tree is the host itself when
    // it is a dock, else its first .dcs-dock child.
    auto* host = find_first_descendant_with_class(root_node,
                                                  "dcs-dock--floathost");
    if (!host) return out;
    lxb_dom_element_t* workdock =
        detail::class_list_contains(host, "dcs-dock")
            ? host
            : dock_child_with_class(host, "dcs-dock");
    if (!workdock) return out;
    out.present = true;
    out.root = dock_layout_node(workdock);
    // Floats: .dcs-panel--floating descendants of the host.
    std::vector<lxb_dom_element_t*> floats;
    auto collect = [&](lxb_dom_element_t* e) {
        if (detail::class_list_contains(e, "dcs-panel--floating")) floats.push_back(e);
    };
    detail::walk_dom_elements(lxb_dom_interface_node(host), collect);
    const int host_idx = detail::block_index_for_exact_element(*impl_, host);
    for (auto* fp : floats) {
        auto* pane = detail::first_descendant_with_class(fp, "dcs-dockpane");
        if (!pane) continue;
        DockLayout::Float f;
        // Inline style is the authority, in WHICHEVER form it uses: drag
        // surgery writes concrete left/top, a corner-anchored seed says
        // right:/bottom:. Keeping the anchored form in the snapshot (instead
        // of converting through the layout rect) means the harvest never
        // depends on layout timing — it can run mid-gesture, pre-settle,
        // and still replay the float exactly where the style puts it. The
        // rendered rect (host-relative) is only the last-resort fallback for
        // raw-HTML floats positioned by external CSS.
        const std::string style = detail::attr_string(fp, "style");
        auto px = [&](std::string_view prop) -> std::optional<int> {
            const std::string v = detail::find_decl_value(style, prop);
            if (v.empty()) return std::nullopt;
            return static_cast<int>(std::strtol(v.c_str(), nullptr, 10));
        };
        const auto sl = px("left"), st = px("top");
        const auto sr = px("right"), sb = px("bottom");
        const auto sw = px("width"), sh = px("height");
        const int fp_idx = detail::block_index_for_exact_element(*impl_, fp);
        const bool have_rect =
            host_idx >= 0 && fp_idx >= 0 &&
            impl_->blocks[static_cast<std::size_t>(fp_idx)].bounds.w > 0 &&
            impl_->blocks[static_cast<std::size_t>(fp_idx)].bounds.h > 0;
        const Rect hb = host_idx >= 0
                            ? impl_->blocks[static_cast<std::size_t>(host_idx)]
                                  .bounds
                            : Rect{};
        const Rect fb = fp_idx >= 0
                            ? impl_->blocks[static_cast<std::size_t>(fp_idx)]
                                  .bounds
                            : Rect{};
        if (sl) {
            f.x = *sl;
        } else if (sr) {
            f.x = *sr;
            f.from_right = true;
        } else {
            f.x = have_rect ? static_cast<int>(std::lround(fb.x - hb.x)) : 0;
        }
        if (st) {
            f.y = *st;
        } else if (sb) {
            f.y = *sb;
            f.from_bottom = true;
        } else {
            f.y = have_rect ? static_cast<int>(std::lround(fb.y - hb.y)) : 0;
        }
        f.w = sw ? *sw
                 : (have_rect ? static_cast<int>(std::lround(fb.w)) : 0);
        f.h = sh ? *sh
                 : (have_rect ? static_cast<int>(std::lround(fb.h)) : 0);
        f.title_only = detail::class_list_contains(pane, "dcs-dockpane--title-only") ||
                       dock_title_tab(pane) != nullptr;
        f.pane = dock_layout_node(pane);
        out.floats.push_back(std::move(f));
    }
#endif
    return out;
}

Rect Document::find_element_rect(std::string_view target) const {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc || target.empty()) return {};
    // Geometry queries must observe laid-out bounds. A structural
    // mutation batch (reconcile, dock surgery) recollects blocks and
    // resets layout state; run the painterless relayout with last-known
    // metrics — the same hidden-relayout contract dispatch() uses — so
    // callers never read zero-sized rects between a mutation and the
    // next frame. Logically const: lazy evaluation of retained state.
    if (impl_->content_size.width == 0 &&
        impl_->media_viewport_width_px > 0) {
        const auto t0 = std::chrono::steady_clock::now();
        const_cast<Document*>(this)->layout(impl_->media_viewport_width_px,
                                            impl_->media_viewport_height_px,
                                            impl_->last_measurer);
        if (detail::MutationTraceTimer::enabled()) {
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            std::fprintf(stderr, "[rect] hidden relayout took %.2f ms\n",
                         ms);
            std::fflush(stderr);
        }
    }
    lxb_dom_element_t* found = nullptr;
    std::string attr_name;
    std::string attr_value;
    if (target.front() == '#') {
        attr_name = "id";
        attr_value = std::string(target.substr(1));
    } else if (target.front() == '[' && target.back() == ']') {
        const auto body = target.substr(1, target.size() - 2);
        const auto eq = body.find('=');
        if (eq == std::string_view::npos) return {};
        attr_name = std::string(body.substr(0, eq));
        attr_value = std::string(body.substr(eq + 1));
    } else {
        attr_name = "data-aui-name";
        attr_value = std::string(target);
    }
    auto* body = lxb_html_document_body_element(impl_->doc);
    if (!body) return {};
    auto collect = [&](lxb_dom_element_t* e) {
        if (!found && detail::attr_string(e, attr_name) == attr_value) found = e;
    };
    detail::walk_dom_elements(lxb_dom_interface_node(body), collect);
    if (!found) return {};
    const int bi = detail::block_index_for_exact_element(*impl_, found);
    if (bi < 0) return {};
    return impl_->blocks[static_cast<std::size_t>(bi)].bounds;
#else
    (void) target;
    return {};
#endif
}
}  // namespace affineui
