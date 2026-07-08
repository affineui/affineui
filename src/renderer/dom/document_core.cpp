// document_core.cpp — affineui::Document lifecycle + public surface.
//
// The document implementation is split across focused TUs (see
// dom/document_impl.h for the shared types and cross-file helpers):
//   document_core.cpp      lifecycle, set_html, stylesheets, weak handles,
//                          scripts/widgets plumbing, view-sink batching (this file)
//   document_style.cpp     CSS/stylesheet machinery + attr/text primitives
//   document_layout.cpp    block collection + layout + CSS animation sampling
//   document_draw.cpp      the paint pass
//   document_restyle.cpp   incremental restyle + DOM mutation engine
//   document_controls.cpp  live-control gestures (sliders, menus, colorfields)
//   document_dock.cpp      dock-layout surgery + dcs-tree drag
//   document_text.cpp      text controls: focus, caret, selection, clipboard
//   document_dispatch.cpp  event dispatch + hover/cursor queries

#include "affineui/document.h"

#include "affineui/memory.h"
#include "affineui/painter.h"
#include "affineui/themes.h"
#include "affineui/view.h"
#include "framework/imm/imm_runtime.h"
#include "renderer/style/animated_style.h"
#include "renderer/style/computed_style.h"
#include "core/diag.h"
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
#include <cstring>
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

// Shared document-internal types (Block, DocumentImpl, CSS side tables).
#include "renderer/dom/document_impl.h"

namespace affineui {

namespace {

std::string env_value(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

// Automation capture: UiScript (affineui/automation.h) installs a callback to
// record dock-trace lines for script assertions. Process-global, like the env
// switches; guarded by the same mutex as the file sink.
std::function<void(const std::string&)>& dock_trace_capture_slot() {
    static std::function<void(const std::string&)> slot;
    return slot;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool dock_trace_enabled() {
    if (dock_trace_capture_slot()) return true;
    const std::string flag = env_value("AFFINEUI_DOCK_TRACE");
    if (!flag.empty() && flag != "0") return true;
    return !env_value("AFFINEUI_DOCK_TRACE_FILE").empty();
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void dock_trace(std::string msg) {
    if (!detail::dock_trace_enabled()) return;
    static std::atomic<std::uint64_t> sequence{1};
    msg.insert(0, "[affineui:dock #" +
                      std::to_string(sequence.fetch_add(
                          1, std::memory_order_relaxed)) +
                      "] ");
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (auto& capture = dock_trace_capture_slot()) capture(msg);
    const std::string path = env_value("AFFINEUI_DOCK_TRACE_FILE");
    if (!path.empty()) {
        std::ofstream out(path, std::ios::app);
        if (out) {
            out << msg << '\n';
            return;
        }
    }
    detail::log_msg(LogLevel::debug, msg.c_str());
}
}  // namespace detail
namespace {

}  // namespace

namespace detail {
// Bridge for the automation layer (src/app/automation.cpp).
void set_dock_trace_capture(std::function<void(const std::string&)> fn) {
    dock_trace_capture_slot() = std::move(fn);
}
}  // namespace detail

namespace detail {

std::uint32_t next_document_id() {
    static std::atomic<std::uint32_t> next{1};
    std::uint32_t id = next.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) id = next.fetch_add(1, std::memory_order_relaxed);
    return id == 0 ? 1 : id;
}

#if !defined(AFFINEUI_STUB_BUILD)

void advance_generation(DomWeakSlot& slot) {
    ++slot.generation;
    if (slot.generation == 0) slot.generation = 1;
}

void invalidate_dom_weak_slot(DocumentImpl& impl, lxb_dom_node_t* node) {
    if (node == nullptr) return;
    impl.live_text_values.erase(node);
    impl.live_text_carets.erase(node);
    impl.live_text_selections.erase(node);
    impl.user_textarea_sizes.erase(node);
    impl.dcs_select_anchors.erase(node);
    for (auto it = impl.dcs_select_anchors.begin();
         it != impl.dcs_select_anchors.end(); ) {
        if (it->second == node) {
            it = impl.dcs_select_anchors.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& slot : impl.dom_weak_slots) {
        if (slot.node == node) {
            slot.node = nullptr;
            advance_generation(slot);
        }
    }
}

DocumentImpl* document_impl_from_node(lxb_dom_node_t* node) {
    if (node == nullptr || node->owner_document == nullptr) return nullptr;
    return static_cast<DocumentImpl*>(node->owner_document->user);
}

lxb_status_t affineui_dom_event_remove(lxb_dom_node_t* node) {
    auto* impl = document_impl_from_node(node);
    if (impl != nullptr) {
        invalidate_dom_weak_slot(*impl, node);
        if (impl->lexbor_ev_remove != nullptr) {
            return impl->lexbor_ev_remove(node);
        }
    }
    return LXB_STATUS_OK;
}

lxb_status_t affineui_dom_event_destroy(lxb_dom_node_t* node) {
    auto* impl = document_impl_from_node(node);
    if (impl != nullptr) {
        invalidate_dom_weak_slot(*impl, node);
        if (impl->lexbor_ev_destroy != nullptr) {
            return impl->lexbor_ev_destroy(node);
        }
    }
    return LXB_STATUS_OK;
}

void install_dom_event_hooks(DocumentImpl& impl) {
    if (impl.doc == nullptr) return;
    auto& dom_doc = impl.doc->dom_document;
    impl.lexbor_ev_remove = dom_doc.ev_remove;
    impl.lexbor_ev_destroy = dom_doc.ev_destroy;
    dom_doc.user = &impl;
    dom_doc.ev_remove = affineui_dom_event_remove;
    dom_doc.ev_destroy = affineui_dom_event_destroy;
}

#endif

}  // namespace detail

Document::Document() : impl_{std::make_unique<detail::DocumentImpl>()} {}
Document::~Document() = default;

Document::Document(Document&&) noexcept            = default;
Document& Document::operator=(Document&&) noexcept = default;

void Document::set_html(std::string_view html) {
    const auto previous_scroll =
        detail::snapshot_scroll_state(*impl_, /*include_elements=*/false);
    // The whole DOM is being replaced — any view-reconcile node mapping
    // is now stale.
    if (impl_->view_sink_reset) impl_->view_sink_reset();
    impl_->html.assign(html);
    impl_->blocks.clear();
    impl_->style_store.reset();
    impl_->paint_dirty = true;
    impl_->content_size = Size{0, 0};
    impl_->dirty_rects.clear();
    impl_->pending_dirty_roots.clear();
    impl_->activated_widgets.clear();
    impl_->changed_widgets.clear();
    impl_->animation_candidate_count = 0;
#if !defined(AFFINEUI_STUB_BUILD)
    impl_->scrollbar_drag = {};
    impl_->splitter_drag = {};
    impl_->float_drag = {};
    impl_->float_resize = {};
    impl_->tab_drag = {};
    impl_->tab_drag_ghost = nullptr;
    impl_->live_drag = {};
    impl_->colorfield_drag = {};
    impl_->tree_drag = {};
#else
    impl_->scrollbar_drag = {};
#endif
    for (auto& slot : impl_->dom_weak_slots) {
#if !defined(AFFINEUI_STUB_BUILD)
        slot.node = nullptr;
#endif
        ++slot.generation;
        if (slot.generation == 0) slot.generation = 1;
    }

#if !defined(AFFINEUI_STUB_BUILD)
    // Tear down the previous document; its CSS pool owns the
    // attached stylesheets, so destroying doc tears them down too.
    impl_->resolver.reset();
    impl_->sheets.clear();
    impl_->attr_subtree_local_cache.clear();
    impl_->attr_subject_confined_cache.clear();
    // Every element in the old document is destroyed here, so the
    // pointer-keyed SVG geometry cache must go with them.
    impl_->svg_path_cache.clear();
    impl_->pseudo_rules.clear();
    impl_->rule_fills.clear();
    impl_->generated_content_rules.clear();
    impl_->font_faces.clear();
    impl_->media_blocks.clear();
    impl_->keyframes.clear();
    impl_->live_text_values.clear();
    impl_->live_text_carets.clear();
    impl_->live_text_selections.clear();
    impl_->dcs_select_anchors.clear();
    impl_->text_layout_cache.clear();
    impl_->text_layout_signatures.clear();
    impl_->text_selection_drag_idx = -1;
    impl_->last_text_click_valid = false;
    impl_->hovered_chain.clear();
    impl_->active_chain.clear();
    impl_->hovered_idx = -1;
    impl_->active_idx = -1;
    impl_->focused_idx = -1;
    impl_->animation_epoch = std::chrono::steady_clock::now();
    if (impl_->doc) {
        lxb_html_document_destroy(impl_->doc);
        impl_->doc = nullptr;
    }

    // Route lexbor's global allocator through affineui::mem before its first
    // allocation (idempotent). Captures the DOM + CSS arenas — the bulk of our
    // heap traffic — for host-allocator routing and leak/UAF tracking.
    mem::install_lexbor_hooks();

    impl_->doc = lxb_html_document_create();
    if (!impl_->doc) return;

    // Initialise CSS subsystems on the document BEFORE parsing HTML so
    // any <style>/style="..." inline declarations are kept.
    if (lxb_html_document_css_init(impl_->doc) != LXB_STATUS_OK) {
        return;
    }
    detail::install_dom_event_hooks(*impl_);

    if (lxb_html_document_parse(
            impl_->doc,
            reinterpret_cast<const lxb_char_t*>(impl_->html.data()),
            impl_->html.size()) != LXB_STATUS_OK) {
        return;
    }

    // Cascade order (lower â†’ higher specificity, ties to last):
    //   1. User-agent baseline
    //   2. Author <style> blocks from the page
    //   3. User stylesheet (App-supplied, often a framework/theme)
    // Matching @media blocks are attached by detail::attach_stylesheet() after the
    // stylesheet that owns them once a viewport is known.
    detail::attach_stylesheet(*impl_, theme::ua_default());

    std::string author_css;
    detail::collect_author_stylesheets(lxb_dom_interface_node(impl_->doc), *impl_, author_css);
    detail::attach_stylesheet(*impl_, author_css);

    detail::attach_stylesheet(*impl_, impl_->user_stylesheet,
                      impl_->user_stylesheet_base_url);

    // Resolver runs against the now fully-cascade-attached document.
    impl_->resolver = detail::make_lexbor_resolver(
        impl_->doc, impl_->media_viewport_width_px,
        impl_->media_viewport_height_px);
    impl_->media_match_signature =
        detail::media_match_signature(*impl_, impl_->media_viewport_width_px);

    // Establish a root inheritance baseline. Reasonable initial values
    // for the implicit document root â€” anything not overridden by CSS
    // gets these. AnimatedStyle's foreground defaults to near-white
    // (#dcdce6) so unstyled docs are readable on the dark clear color.
    impl_->root_style                       = detail::ResolvedStyle{};
    impl_->root_style.animated.color_rgba   = 0xDCDCE6FFu;
    impl_->root_style.computed.font_size_px = 16;
    impl_->root_style.computed.font_weight  = 400;

    // Use <body>'s resolved style as the parent for all blocks so
    // body-level CSS (e.g. `body { color: ... }`) inherits down.
    // Store the resolved body style BACK into root_style so the
    // dispatch-time restyle path (parent_resolved for parent_idx=-1)
    // sees the same parent the initial collect used. Without this,
    // hover-triggered restyles silently reset top-level blocks to
    // the placeholder root color, leaking grey into things like h1
    // that should be inheriting body's color.
    auto* body = lxb_html_document_body_element(impl_->doc);
    if (body) {
        // Resolve <html> first so `:root { --custom: ... }` properties
        // (e.g. Bootstrap's whole --bs-* palette) are collected and
        // inherited down through <body> into every block. <html> is
        // <body>'s parent node.
        detail::ResolvedStyle html_style = impl_->root_style;
        auto* body_node = lxb_dom_interface_node(body);
        if (body_node->parent != nullptr &&
            body_node->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            html_style = impl_->resolver->resolve(
                lxb_dom_interface_element(body_node->parent), impl_->root_style);
        }
        auto* body_elem = lxb_dom_interface_element(body_node);
        impl_->root_style = impl_->resolver->resolve(body_elem, html_style);
        detail::apply_font_family_fills(*impl_, "body", detail::attr_string(body_elem, "id"),
                                detail::split_classes(detail::attr_string(body_elem, "class")),
                                /*parent_idx=*/-1,
                                /*state_bits=*/0,
                                impl_->root_style);
    }
    detail::collect_blocks(*impl_,
                   body ? lxb_dom_interface_node(body)
                        : lxb_dom_interface_node(impl_->doc),
                   impl_->root_style,
                   /*parent_idx=*/-1);
    detail::restore_scroll_state(*impl_, previous_scroll);
    for (const auto& b : impl_->blocks) {
        if (b.animation.active && b.animation.name_hash != 0) {
            ++impl_->animation_candidate_count;
        }
    }
#endif
}

void Document::attach_script(DocumentScript script) {
    switch (script) {
        case DocumentScript::UiControls:
            impl_->ui_control_script_attached = true;
            break;
    }
}

void Document::detach_script(DocumentScript script) {
    switch (script) {
        case DocumentScript::UiControls:
            impl_->ui_control_script_attached = false;
#if !defined(AFFINEUI_STUB_BUILD)
            impl_->live_drag = {};
            impl_->colorfield_drag = {};
            impl_->tree_drag = {};
#endif
            break;
    }
}

void Document::clear_scripts() {
    impl_->ui_control_script_attached = false;
#if !defined(AFFINEUI_STUB_BUILD)
    impl_->live_drag = {};
    impl_->colorfield_drag = {};
    impl_->tree_drag = {};
#endif
}

std::vector<std::string> Document::take_activated_widgets() {
    auto out = std::move(impl_->activated_widgets);
    impl_->activated_widgets.clear();
    return out;
}

std::vector<Document::WidgetChange> Document::take_widget_changes() {
    auto out = std::move(impl_->changed_widgets);
    impl_->changed_widgets.clear();
    return out;
}

std::vector<std::pair<std::string, int>> Document::dock_pane_sizes() const {
    std::vector<std::pair<std::string, int>> out;
    for (const auto& b : impl_->blocks) {
        if (std::find(b.classes.begin(), b.classes.end(), "dcs-dockpane") ==
            b.classes.end())
            continue;
        std::string_view name;
        std::string_view style;
        for (const auto& a : b.attrs) {
            if (a.first == "data-aui-name") name = a.second;
            else if (a.first == "style") style = a.second;
        }
        if (name.rfind("pane-", 0) != 0) continue;  // engine names panes pane-<id>
        // Parse the fixed flex-basis: "flex:0 0 <N>px".
        const auto pos = style.find("flex:0 0 ");
        if (pos == std::string_view::npos) continue;  // flexible center pane
        const char* first = style.data() + pos + 9;
        const char* last = style.data() + style.size();
        int px = 0;
        const auto [ptr, ec] = std::from_chars(first, last, px);
        if (ec != std::errc{} || px <= 0) continue;
        (void) ptr;
        out.emplace_back(std::string(name.substr(5)), px);
    }
    return out;
}

Document::DockPlacement Document::dock_override(std::string_view panel_id) const {
    const auto it = impl_->dock_overrides.find(std::string(panel_id));
    return it == impl_->dock_overrides.end() ? DockPlacement{} : it->second;
}

std::vector<std::pair<std::string, Document::DockPlacement>>
Document::dock_overrides() const {
    std::vector<std::pair<std::string, DockPlacement>> out;
    out.reserve(impl_->dock_overrides.size());
    for (const auto& [id, p] : impl_->dock_overrides) out.emplace_back(id, p);
    return out;
}

std::string Document::dock_active_tab(std::string_view pane_id) const {
    const auto it = impl_->dock_active_tabs.find(std::string(pane_id));
    return it == impl_->dock_active_tabs.end() ? std::string{} : it->second;
}

void Document::set_user_stylesheet(std::string_view css) {
    set_user_stylesheet(css, {});
}

void Document::set_user_stylesheet(std::string_view css,
                                   std::string_view base_url) {
    impl_->user_stylesheet.assign(css);
    impl_->user_stylesheet_base_url.assign(base_url);
    if (!impl_->html.empty()) {
        set_html(impl_->html);
    } else {
        impl_->paint_dirty = true;
    }
}

void Document::reload_stylesheets() {
    if (!impl_->html.empty()) set_html(impl_->html);
}

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void attach_matching_media_blocks_for_viewport(detail::DocumentImpl& impl) {
    if (!impl.doc || impl.media_viewport_width_px <= 0) return;
    const std::size_t media_count = impl.media_blocks.size();
    for (std::size_t i = 0; i < media_count; ++i) {
        if (impl.media_blocks[i].matches(impl.media_viewport_width_px)) {
            detail::attach_media_block(impl, impl.media_blocks[i]);
        }
    }
    impl.resolver = detail::make_lexbor_resolver(
        impl.doc, impl.media_viewport_width_px,
        impl.media_viewport_height_px);
    impl.media_match_signature =
        detail::media_match_signature(impl, impl.media_viewport_width_px);
    detail::recollect_blocks_from_current_dom(impl);
}
}  // namespace detail

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Numeric data-* attribute straight off the element (paint-time read;
// block.attrs can lag live-control writes within a frame).
double elem_attr_num(lxb_dom_element_t* elem, std::string_view name,
                     double fallback) {
    const auto v = detail::attr_view(elem, name);
    if (v.empty()) return fallback;
    char buf[48];
    const auto n = std::min(v.size(), sizeof(buf) - 1);
    std::memcpy(buf, v.data(), n);
    buf[n] = '\0';
    char* end = nullptr;
    const double parsed = std::strtod(buf, &end);
    return end == buf ? fallback : parsed;
}
}  // namespace detail
namespace {

}  // namespace

namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Sum of scroll offsets contributed by scrollable ancestors of
// `idx`. Used by hit-test and paint to convert document-space block
// bounds into the effective on-screen position.
int scroll_offset_y_for(const std::vector<Block>& blocks,
#if !defined(AFFINEUI_STUB_BUILD)
                        const detail::StyleStore& styles,
#endif
                        int idx) {
    int sum = 0;
#if !defined(AFFINEUI_STUB_BUILD)
    using O = detail::ComputedStyle::Overflow;
    using P = detail::ComputedStyle::Position;
    if (idx >= 0 && static_cast<std::size_t>(idx) < blocks.size() &&
        styles.computed(blocks[static_cast<std::size_t>(idx)].id).position ==
            P::Fixed) {
        return 0;
    }
    int p = (idx >= 0) ? blocks[static_cast<std::size_t>(idx)].parent_idx : -1;
    while (p >= 0) {
        const auto& pb = blocks[static_cast<std::size_t>(p)];
        const auto& pcs = styles.computed(pb.id);
        const auto ov = pcs.overflow_y;
        if ((ov == O::Scroll || ov == O::Auto) && pb.scroll_y != 0) {
            sum += pb.scroll_y;
        }
        if (pcs.position == P::Fixed) break;
        p = pb.parent_idx;
    }
#else
    (void)blocks; (void)idx;
#endif
    return sum;
}
}  // namespace detail
namespace {

#if !defined(AFFINEUI_STUB_BUILD)
}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
int effective_z_index(const detail::DocumentImpl& impl, int idx) {
    int z = 0;
    for (int cur = idx;
         cur >= 0 && cur < static_cast<int>(impl.blocks.size());
         cur = impl.blocks[static_cast<std::size_t>(cur)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(cur)];
        z = std::max<int>(z, impl.style_store.computed(block.id).z_index_low);
    }
    return z;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
const KeyframeBlock* find_keyframes(const detail::DocumentImpl& impl,
                                    std::uint32_t name_hash) {
    if (name_hash == 0) return nullptr;
    auto it = std::find_if(impl.keyframes.begin(), impl.keyframes.end(),
        [name_hash](const KeyframeBlock& kf) {
            return kf.name_hash == name_hash;
        });
    return it == impl.keyframes.end() ? nullptr : &*it;
}
}  // namespace detail
namespace {

#endif

#if !defined(AFFINEUI_STUB_BUILD)
bool invert_transform(const Mat2x3& m, Mat2x3& out) {
    const float det = m.a * m.d - m.b * m.c;
    if (std::abs(det) < 1e-6f) return false;
    const float inv_det = 1.0f / det;
    out.a =  m.d * inv_det;
    out.b = -m.b * inv_det;
    out.c = -m.c * inv_det;
    out.d =  m.a * inv_det;
    out.tx = -(out.a * m.tx + out.c * m.ty);
    out.ty = -(out.b * m.tx + out.d * m.ty);
    return true;
}

bool rect_contains_float(const Rect& r, float x, float y) noexcept {
    return x >= static_cast<float>(r.x) &&
           x <  static_cast<float>(r.x + r.w) &&
           y >= static_cast<float>(r.y) &&
           y <  static_cast<float>(r.y + r.h);
}
#endif

bool hit_test_skip_for_pointer(const Block& block) {
    if (detail::block_has_class(block, "dcs-panel__resize-zones") ||
        detail::block_has_class(block, "dcs-panel__resize")) {
        return true;
    }
    // Honor inline `pointer-events:none` — overlay layers (patch-cable
    // SVG, ghosts, HUDs) opt out of hit-testing the standard CSS way.
    if (const auto* style = detail::block_attr_value(block, "style")) {
        if (style->find("pointer-events:none") != std::string::npos ||
            style->find("pointer-events: none") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hit_test_skip_for_dock_target(const Block& block) {
    return hit_test_skip_for_pointer(block) ||
           block.elem_id == "__dropind" || block.elem_id == "__dockghost" ||
           detail::block_has_class(block, "dcs-drop") ||
           detail::block_has_class(block, "dcs-dockpane__tab-ghost") ||
           detail::block_has_class(block, "dcs-panel__resize-zone");
}

int hit_test_blocks_impl(const detail::DocumentImpl& impl,
                         int x,
                         int y,
                         bool skip_dock_overlay) {
    const auto& blocks = impl.blocks;
    int hit = -1;
    int hit_z = std::numeric_limits<int>::min();
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (hit_test_skip_for_pointer(blocks[i])) continue;
        if (skip_dock_overlay && hit_test_skip_for_dock_target(blocks[i]))
            continue;
#if !defined(AFFINEUI_STUB_BUILD)
        const int dy = detail::scroll_offset_y_for(
            blocks, impl.style_store, static_cast<int>(i));
#else
        const int dy = 0;
#endif
        Rect eff = blocks[i].bounds;
        eff.y -= dy;
#if !defined(AFFINEUI_STUB_BUILD)
        const Mat2x3 transform =
            effective_transform_for(impl, static_cast<int>(i));
        bool contains = false;
        if (!transform.is_identity()) {
            Mat2x3 inverse{};
            if (!invert_transform(transform, inverse)) continue;
            const Vec2 local = inverse.apply(
                Vec2{static_cast<float>(x), static_cast<float>(y)});
            contains = rect_contains_float(eff, local.x, local.y);
        } else {
            contains = detail::rect_contains(eff, x, y);
        }
        if (contains) {
            const int z = detail::effective_z_index(impl, static_cast<int>(i));
            if (z > hit_z || (z == hit_z && static_cast<int>(i) > hit)) {
                hit = static_cast<int>(i);
                hit_z = z;
            }
        }
#else
        if (detail::rect_contains(eff, x, y)) {
            hit = static_cast<int>(i);
        }
#endif
    }
    return hit;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Deepest block whose effective border-box (after applying ancestor scroll
// offsets and CSS transforms) contains (x, y), or -1 if none. z-index buckets
// win first, then normal document order breaks ties.
int hit_test_blocks(const detail::DocumentImpl& impl, int x, int y) {
    return hit_test_blocks_impl(impl, x, y, false);
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
int hit_test_blocks_for_dock_target(const detail::DocumentImpl& impl,
                                    int x,
                                    int y) {
    return hit_test_blocks_impl(impl, x, y, true);
}
}  // namespace detail
namespace {

std::string block_trace_name(const Block& b) {
    std::string out = b.tag.empty() ? "?" : b.tag;
    if (!b.elem_id.empty()) out += "#" + b.elem_id;
    for (const auto& attr : b.attrs) {
        if (attr.first == "data-aui-name") {
            out += "[" + attr.second + "]";
            break;
        }
    }
    if (!b.classes.empty()) {
        out += ".";
        const std::size_t n = std::min<std::size_t>(b.classes.size(), 3);
        for (std::size_t i = 0; i < n; ++i) {
            if (i > 0) out += ".";
            out += b.classes[i];
        }
    }
    out += "@(" + std::to_string(b.bounds.x) + "," +
           std::to_string(b.bounds.y) + "," + std::to_string(b.bounds.w) +
           "x" + std::to_string(b.bounds.h) + ")";
    return out;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::string hit_chain_summary(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return "none";
    std::string out;
    int depth = 0;
    while (idx >= 0 && idx < static_cast<int>(impl.blocks.size()) && depth < 8) {
        if (!out.empty()) out += " <- ";
        out += block_trace_name(impl.blocks[static_cast<std::size_t>(idx)]);
        idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx;
        ++depth;
    }
    return out;
}
}  // namespace detail
namespace {

}  // namespace

void Document::set_resource_loader(ResourceLoader loader) {
    impl_->resource_loader = std::move(loader);
}

void Document::set_clipboard(ClipboardGet get, ClipboardSet set) {
    impl_->clipboard_get = std::move(get);
    impl_->clipboard_set = std::move(set);
}

Size Document::content_size() const { return impl_->content_size; }

DomHandle Document::weak_handle_for_id(std::string_view elem_id) {
    DomHandle out{};
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc || elem_id.empty()) return out;
    auto* elem = detail::find_dom_element_by_id(*impl_, elem_id);
    if (!elem) return out;
    auto* node = lxb_dom_interface_node(elem);
    if (!node) return out;

    out.document_id = impl_->document_id;
    for (std::size_t i = 0; i < impl_->dom_weak_slots.size(); ++i) {
        auto& slot = impl_->dom_weak_slots[i];
        if (slot.node == node) {
            out.node_slot = static_cast<std::uint32_t>(i + 1);
            out.generation = slot.generation;
            return out;
        }
    }

    std::size_t slot_index = impl_->dom_weak_slots.size();
    for (std::size_t i = 0; i < impl_->dom_weak_slots.size(); ++i) {
        if (impl_->dom_weak_slots[i].node == nullptr) {
            slot_index = i;
            break;
        }
    }
    if (slot_index == impl_->dom_weak_slots.size()) {
        impl_->dom_weak_slots.push_back({});
    }
    auto& slot = impl_->dom_weak_slots[slot_index];
    slot.node = node;
    if (slot.generation == 0) slot.generation = 1;
    out.node_slot = static_cast<std::uint32_t>(slot_index + 1);
    out.generation = slot.generation;
#else
    (void)elem_id;
#endif
    return out;
}

bool Document::weak_handle_valid(DomHandle handle) const {
    if (handle.document_id != impl_->document_id || handle.node_slot == 0) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(handle.node_slot - 1);
    if (index >= impl_->dom_weak_slots.size()) return false;
    const auto& slot = impl_->dom_weak_slots[index];
    if (slot.generation != handle.generation) return false;
#if !defined(AFFINEUI_STUB_BUILD)
    return slot.node != nullptr;
#else
    return false;
#endif
}

// ── View reconciliation sink ────────────────────────────────────────
// Applies View builder mutations directly to the retained DOM — the
// App fast path that replaces to_html + reparse per rebuild. Attribute
// and text writes ride the same live-mutation classification as
// set_attribute_by_id (svg-child geometry stays paint-only, class /
// style changes restyle a subtree); structural changes are applied
// with lexbor's eager style attach suppressed and settled by ONE
// restyle + box recollect in end_view_mutations().
//
// Creations are always appends: the View reconciler truncates the
// mismatched tail of a child list (emitting removes) before recreating,
// so by the time create_* fires, the DOM parent's children exactly
// match the widget indices below `index`.
#if !defined(AFFINEUI_STUB_BUILD)
namespace {

// Batched view mutations run with lexbor's ev_insert suppressed, which
// also silences its inline-style hook: an element that ACQUIRES a
// `style` attribute inside the batch would never get the declarations
// parsed into its style list (ev_set_value still covers value changes
// on an existing attribute). Re-run the parse explicitly — mirrors
// lxb_html_document_event_insert_attribute for the fresh-attr case.
void parse_inline_style_attr(lxb_dom_element_t* e) {
    if (!e) return;
    auto* node = lxb_dom_interface_node(e);
    if (node->ns != LXB_NS_HTML) return;
    lxb_dom_attr_t* attr = lxb_dom_element_attr_by_id(e, LXB_DOM_ATTR_STYLE);
    if (!attr || !attr->value || !attr->value->data) return;
    lxb_html_element_style_parse(lxb_html_interface_element(e),
                                 attr->value->data, attr->value->length);
}

void parse_inline_styles_deep(lxb_dom_node_t* n) {
    if (!n) return;
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        parse_inline_style_attr(lxb_dom_interface_element(n));
    }
    for (auto* child = lxb_dom_node_first_child(n); child != nullptr;
         child = lxb_dom_node_next(child)) {
        parse_inline_styles_deep(child);
    }
}

void invalidate_resolver_deep(detail::DocumentImpl& impl, lxb_dom_node_t* n);

class DocumentViewSink final : public ViewSink {
public:
    explicit DocumentViewSink(detail::DocumentImpl& impl) : impl_(impl) {}

    void reset() {
        elems_.clear();
        texts_.clear();
        raw_groups_.clear();
        raw_parents_.clear();
    }

    void create_element(const WidgetNode& node, const WidgetNode* parent,
                        std::size_t /*index*/) override {
        auto* p = parent_dom(parent);
        if (!p || !impl_.doc) return;
        auto* e = lxb_dom_document_create_element(
            lxb_dom_interface_document(impl_.doc), detail::as_lxb(node.tag),
            node.tag.size(), nullptr);
        if (!e) return;
        // Replay path: a fully-built node arrives with attrs/text in
        // place. Live-reconcile creates arrive empty and receive them
        // as set_attribute / set_text events right after.
        for (const auto& attr : node.attrs) {
            lxb_dom_element_set_attribute(e, detail::as_lxb(attr.name),
                                          attr.name.size(),
                                          detail::as_lxb(attr.value),
                                          attr.value.size());
        }
        if (!node.text.empty()) {
            lxb_dom_node_text_content_set(
                lxb_dom_interface_node(e),
                reinterpret_cast<const lxb_char_t*>(node.text.c_str()),
                node.text.size());
        }
        parse_inline_style_attr(e);
        lxb_dom_node_insert_child(p, lxb_dom_interface_node(e));
        elems_[node.remote_id] = e;
        // Recycled-pointer insurance: a freed element's address can be
        // reused; make sure no stale resolver entry shadows this one.
        if (impl_.resolver) impl_.resolver->invalidate(e);
        note_structure_change(p);
    }

    void create_text(const WidgetNode& node, const WidgetNode* parent,
                     std::size_t /*index*/) override {
        auto* p = parent_dom(parent);
        if (!p || !impl_.doc) return;
        auto* t = lxb_dom_document_create_text_node(
            lxb_dom_interface_document(impl_.doc),
            reinterpret_cast<const lxb_char_t*>(node.text.c_str()),
            node.text.size());
        if (!t) return;
        lxb_dom_node_insert_child(p, lxb_dom_interface_node(t));
        texts_[node.remote_id] = lxb_dom_interface_node(t);
        note_structure_change(p);
    }

    void create_raw_html(const WidgetNode& node, const WidgetNode* parent,
                         std::size_t /*index*/) override {
        // Markup arrives in the set_raw_html that follows; remember the
        // parent so it can parse the fragment into the right place.
        raw_parents_[node.remote_id] =
            parent ? parent->remote_id : std::string{};
        raw_groups_[node.remote_id];  // ensure (empty) group exists
    }

    void set_raw_html(const WidgetNode& node,
                      std::string_view markup) override {
        if (!impl_.doc) return;
        const auto parent_it = raw_parents_.find(node.remote_id);
        if (parent_it == raw_parents_.end()) return;
        lxb_dom_node_t* parent =
            parent_it->second.empty()
                ? root_dom()
                : elem_node(parent_it->second);
        if (!parent) return;

        auto& group = raw_groups_[node.remote_id];
        lxb_dom_node_t* anchor = nullptr;  // insert new nodes before this
        if (!group.empty()) {
            anchor = lxb_dom_node_next(group.back());
            for (auto* old : group) {
                invalidate_resolver_deep(impl_, old);  // pre-destroy
                lxb_dom_node_remove(old);
                lxb_dom_node_destroy_deep(old);
            }
            group.clear();
        }

        auto* frag = lxb_html_document_parse_fragment(
            impl_.doc, lxb_dom_interface_element(parent),
            detail::as_lxb(markup), markup.size());
        if (frag) {
            while (auto* child = lxb_dom_node_first_child(frag)) {
                lxb_dom_node_remove(child);
                if (anchor) {
                    lxb_dom_node_insert_before(anchor, child);
                } else {
                    lxb_dom_node_insert_child(parent, child);
                }
                parse_inline_styles_deep(child);
                group.push_back(child);
            }
        }
        // Paint-only lane: raw html swapped INSIDE an <svg> subtree changes
        // vector content only — svg children carry no blocks, so restyle/
        // recollect/layout cannot be affected. Repaint the host block
        // instead of declaring structural dirt (the structural settle made
        // every viewport camera move a full-document event).
        bool svg_lane = false;
        for (auto* a = parent; a != nullptr; a = a->parent) {
            if (a->type != LXB_DOM_NODE_TYPE_ELEMENT) break;
            if (detail::tag_name(lxb_dom_interface_element(a)) == "svg") {
                svg_lane = true;
                break;
            }
        }
        if (svg_lane) {
            const int idx = detail::block_index_for_element_or_ancestor(
                impl_, lxb_dom_interface_element(parent));
            if (idx >= 0) {
                detail::mark_live_mutation_dirty(impl_, idx,
                                         detail::subtree_visual_rect(impl_, idx),
                                         /*needs_layout=*/false);
            } else {
                impl_.view_structure_dirty = true;  // no host block yet
            }
        } else {
            note_structure_change(parent);
        }
    }

    void remove(const WidgetNode& node) override {
        lxb_dom_node_t* scope = nullptr;  // parent captured pre-removal
        if (node.kind == WidgetKind::RawHtml) {
            if (auto it = raw_groups_.find(node.remote_id);
                it != raw_groups_.end()) {
                for (auto* n : it->second) {
                    if (scope == nullptr) scope = n->parent;
                    invalidate_resolver_deep(impl_, n);
                    lxb_dom_node_remove(n);
                    lxb_dom_node_destroy_deep(n);
                }
            }
        } else if (node.kind == WidgetKind::Text) {
            if (auto it = texts_.find(node.remote_id); it != texts_.end()) {
                scope = it->second->parent;
                lxb_dom_node_remove(it->second);
                lxb_dom_node_destroy_deep(it->second);
            }
        } else if (auto it = elems_.find(node.remote_id);
                   it != elems_.end()) {
            auto* dom_node = lxb_dom_interface_node(it->second);
            scope = dom_node->parent;
            invalidate_resolver_deep(impl_, dom_node);
            lxb_dom_node_remove(dom_node);
            lxb_dom_node_destroy_deep(dom_node);
        }
        evict(node);
        note_structure_change(scope);
    }

    void set_text(const WidgetNode& node, std::string_view value) override {
        if (node.kind == WidgetKind::Text) {
            if (auto it = texts_.find(node.remote_id); it != texts_.end()) {
                lxb_dom_node_t* text_node = it->second;
                lxb_dom_node_t* parent = text_node->parent;
                // The common shape — a lone text child under a live
                // element block (labels, inspector values) — is a LOCAL
                // change: set_text_on_element refreshes that block's text
                // and marks a scoped remeasure. Only mixed-content parents
                // fall back to the structural settle. NOTE: the element
                // text-set replaces its children, so re-point the map at
                // the freshly created text node.
                const bool lone_text =
                    parent != nullptr &&
                    parent->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                    parent->first_child == text_node &&
                    text_node->next == nullptr;
                if (lone_text && detail::node_text(parent) == value) return;
                if (lone_text &&
                    detail::set_text_on_element(
                        impl_, lxb_dom_interface_element(parent), value)) {
                    it->second = parent->first_child != nullptr
                                     ? parent->first_child
                                     : nullptr;
                    if (it->second == nullptr) texts_.erase(it);
                    return;
                }
                lxb_dom_node_text_content_set(
                    text_node,
                    reinterpret_cast<const lxb_char_t*>(value.data()),
                    value.size());
                note_structure_change(parent);
            }
            return;
        }
        auto* e = elem(node.remote_id);
        if (!e) return;
        if (!detail::set_text_on_element(impl_, e, value)) {
            // No live block yet (element created this batch, or an svg
            // child): write the DOM directly; the batch-end recollect
            // (or the svg paint walk) picks it up.
            lxb_dom_node_text_content_set(
                lxb_dom_interface_node(e),
                reinterpret_cast<const lxb_char_t*>(value.data()),
                value.size());
            note_structure_change(lxb_dom_interface_node(e));
        }
    }

    void set_attribute(const WidgetNode& node, std::string_view name,
                       std::string_view value) override {
        if (auto* e = elem(node.remote_id)) {
            const bool fresh_style =
                name == "style" && !detail::has_attr(e, name);
            detail::set_attribute_on_element(impl_, e, name, value);
            // ev_set_value keeps an EXISTING style attribute parsed; a
            // fresh one arrives through the suppressed ev_insert hook.
            if (fresh_style) parse_inline_style_attr(e);
        }
    }

    void remove_attribute(const WidgetNode& node,
                          std::string_view name) override {
        if (auto* e = elem(node.remote_id)) {
            detail::remove_attribute_on_element(impl_, e, name);
        }
    }

private:
    lxb_dom_element_t* elem(const std::string& rid) {
        auto it = elems_.find(rid);
        return it == elems_.end() ? nullptr : it->second;
    }
    lxb_dom_node_t* elem_node(const std::string& rid) {
        auto* e = elem(rid);
        return e ? lxb_dom_interface_node(e) : nullptr;
    }
    lxb_dom_node_t* root_dom() {
        auto* root = detail::find_dom_element_by_id(impl_, "aui-root");
        return root ? lxb_dom_interface_node(root) : nullptr;
    }
    lxb_dom_node_t* parent_dom(const WidgetNode* parent) {
        return parent ? elem_node(parent->remote_id) : root_dom();
    }
    // Record a structural change scoped to the nearest BLOCK ancestor of
    // `scope` (the mutation's parent). No block scope → global settle
    // fallback (bootstrap into the empty shell, orphan splices).
    // AFFINEUI_SETTLE_GLOBAL=1 forces the global path — the bisect lever
    // for "is a missing scoped rematch causing this style bug?".
    void note_structure_change(lxb_dom_node_t* scope) {
        static const bool force_global =
            std::getenv("AFFINEUI_SETTLE_GLOBAL") != nullptr;
        if (force_global) {
            impl_.view_structure_dirty = true;
            return;
        }
        for (auto* n = scope; n != nullptr; n = n->parent) {
            if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) break;
            const int idx = detail::block_index_for_exact_element(
                impl_, lxb_dom_interface_element(n));
            if (idx >= 0) {
                impl_.view_batch_structure_roots.push_back(idx);
                return;
            }
        }
        impl_.view_structure_dirty = true;
    }

    // Map-entry eviction only — DOM destruction of descendants is
    // implicit in the ancestor's destroy_deep.
    void evict(const WidgetNode& n) {
        elems_.erase(n.remote_id);
        texts_.erase(n.remote_id);
        raw_groups_.erase(n.remote_id);
        raw_parents_.erase(n.remote_id);
        for (const auto& child : n.children) evict(child);
    }

    detail::DocumentImpl& impl_;
    std::unordered_map<std::string, lxb_dom_element_t*> elems_;
    std::unordered_map<std::string, lxb_dom_node_t*>    texts_;
    std::unordered_map<std::string, std::vector<lxb_dom_node_t*>>
        raw_groups_;
    std::unordered_map<std::string, std::string> raw_parents_;
};

// Invalidate the resolver's cached computed styles for every element in
// a DOM subtree. Used by the scoped structural settle: removed subtrees
// must leave no cache entries behind (their pointers can be recycled by
// later allocations), and a changed root's existing descendants may have
// different match sets after the rematch.
void invalidate_resolver_deep(detail::DocumentImpl& impl, lxb_dom_node_t* n) {
    if (impl.resolver == nullptr || n == nullptr) return;
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        impl.resolver->invalidate(lxb_dom_interface_element(n));
    }
    for (auto* c = lxb_dom_node_first_child(n); c != nullptr;
         c = lxb_dom_node_next(c)) {
        invalidate_resolver_deep(impl, c);
    }
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Apply everything the current view batch has recorded — the structural
// settle (global fallback, or SCOPED to the recorded roots), or the §8.2
// scoped attr settle (dedupe root cover → one rematch pass → one resolver
// clear → restyle + reveal per root). Called from end_view_mutations, and
// EARLY from Document::layout when a geometry consumer (the P4
// find_element_rect hidden relayout, a builder measuring mid-build) needs
// fresh boxes while the batch is still open: layout over the un-settled
// block tree would walk blocks whose elements the batch already destroyed.
// Safe to run repeatedly; with nothing recorded it is a no-op.
void settle_view_batch(detail::DocumentImpl& impl) {
    if (impl.view_structure_dirty) {
        detail::TraceSpan span("settle.global");
        impl.view_structure_dirty = false;
        // The full structural settle re-matches/restyles/recollects the
        // whole document — recorded roots are superseded by it.
        impl.view_batch_attr_roots.clear();
        impl.view_batch_structure_roots.clear();
        const auto t0 = std::chrono::steady_clock::now();
        detail::dock_structure_changed(impl);
        if (detail::MutationTraceTimer::enabled()) {
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            std::fprintf(stderr, "[batch] structure-changed took %.2f ms\n",
                         ms);
            std::fflush(stderr);
        }
    } else if (!impl.view_batch_structure_roots.empty()) {
        detail::TraceSpan span("settle.scoped");
        // Scoped structural settle: selector rematch ONLY over the changed
        // subtrees (the resolver cache stays warm — removed/inserted
        // elements were invalidated at op time, and each root's live
        // subtree is invalidated here because its match sets may have
        // changed). The recollect still rebuilds the flat block tree, but
        // against a warm cache the resolve per element is a lookup.
        const auto t0 = std::chrono::steady_clock::now();
        auto roots = std::move(impl.view_batch_structure_roots);
        impl.view_batch_structure_roots.clear();
        impl.view_batch_attr_roots.clear();  // recollect re-resolves all
        std::sort(roots.begin(), roots.end());
        std::vector<int> kept;
        for (const int r : roots) {
            bool covered = false;
            for (const int k : kept) {
                if (k == r || detail::is_descendant_of_or_self(impl.blocks, r, k)) {
                    covered = true;
                    break;
                }
            }
            if (!covered) kept.push_back(r);
        }
        const Rect old_rect = detail::document_visual_rect(impl);
        const auto t1 = std::chrono::steady_clock::now();
        for (const int r : kept) {
            if (auto* e = detail::element_for_block(impl, r)) {
                invalidate_resolver_deep(impl, lxb_dom_interface_node(e));
            }
            (void) detail::rematch_stylesheet_matches_for_subtree(impl, r);
        }
        const auto t2 = std::chrono::steady_clock::now();
        detail::recollect_blocks_from_current_dom(impl);
        const auto t3 = std::chrono::steady_clock::now();
        // Block indices shifted — stale interaction indices are the
        // dangling-pointer class of bug (same contract as
        // dock_structure_changed).
        impl.hovered_idx = -1;
        impl.active_idx = -1;
        impl.hovered_chain.clear();
        impl.active_chain.clear();
        detail::mark_live_mutation_dirty(impl, -1, old_rect, /*needs_layout=*/true);
        impl.paint_dirty = true;
        if (detail::MutationTraceTimer::enabled()) {
            const auto ms = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a)
                    .count();
            };
            std::fprintf(stderr,
                         "[batch] SCOPED structure roots=%zu rematch=%.1f "
                         "recollect=%.1f total=%.1f ms\n",
                         kept.size(), ms(t1, t2), ms(t2, t3), ms(t0, t3));
            std::fflush(stderr);
        }
    } else if (!impl.view_batch_attr_roots.empty()) {
        detail::TraceSpan span("settle.attr");
        // §8.2 batch settle: at most one of each — subtree rematch over the
        // deduped root cover, resolver clear, restyle per root, reveal check
        // per root (against the now-warm resolver cache), dirty rects.
        const auto t0 = std::chrono::steady_clock::now();
        auto roots = std::move(impl.view_batch_attr_roots);
        impl.view_batch_attr_roots.clear();
        std::sort(roots.begin(), roots.end(),
                  [](const auto& a, const auto& b) {
                      return a.root_idx < b.root_idx;
                  });
        std::vector<detail::DocumentImpl::ViewBatchAttrRoot> kept;
        for (const auto& r : roots) {
            bool covered = false;
            for (auto& k : kept) {
                if (k.root_idx == r.root_idx ||
                    (k.root_idx >= 0 && r.root_idx >= 0 &&
                     detail::is_descendant_of_or_self(impl.blocks, r.root_idx,
                                              k.root_idx))) {
                    k.needs_subtree_rematch |= r.needs_subtree_rematch;
                    k.force_layout |= r.force_layout;
                    covered = true;
                    break;
                }
            }
            if (!covered) kept.push_back(r);
        }
        for (const auto& k : kept) {
            if (k.needs_subtree_rematch) {
                (void) detail::rematch_stylesheet_matches_for_subtree(impl,
                                                              k.root_idx);
            }
        }
        if (impl.resolver) impl.resolver->clear();
        for (auto& k : kept) {
            const bool restyle_layout =
                k.root_idx >= 0 ? detail::restyle_subtree(impl, k.root_idx)
                                : detail::restyle_all_blocks(impl);
            k.force_layout = k.force_layout || restyle_layout;
        }
        bool recollected = false;
        for (const auto& k : kept) {
            if (detail::selector_mutation_reveals_hidden_subtree(impl,
                                                         k.root_idx)) {
                detail::recollect_blocks_from_current_dom(impl);
                recollected = true;
                break;
            }
        }
        for (const auto& k : kept) {
            detail::mark_live_mutation_dirty(impl, k.root_idx, k.old_rect,
                                     k.force_layout || recollected);
        }
        if (detail::MutationTraceTimer::enabled()) {
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            std::fprintf(stderr,
                         "[batch] attr settle ops=%zu roots=%zu took %.2f ms\n",
                         roots.size(), kept.size(), ms);
            std::fflush(stderr);
        }
    }
}
}  // namespace detail
namespace {

}  // namespace
#endif  // !AFFINEUI_STUB_BUILD

ViewSink* Document::begin_view_mutations() {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc) return nullptr;
    detail::debug_validate_attr_lists(*impl_, "begin-view-mutations");
    if (!impl_->view_sink) {
        auto sink = std::make_unique<DocumentViewSink>(*impl_);
        impl_->view_sink_reset = [raw = sink.get()] { raw->reset(); };
        impl_->view_sink = std::move(sink);
    }
    if (!impl_->view_batch_active) {
        impl_->view_batch_active = true;
        impl_->view_structure_dirty = false;
        impl_->view_batch_attr_roots.clear();
        impl_->view_batch_structure_roots.clear();
        // Suppress lexbor's eager per-insert selector matching for the
        // batch — end_view_mutations rebuilds style state once (same
        // contract as the dock-gesture surgery).
        impl_->view_saved_ev_insert = impl_->doc->dom_document.ev_insert;
        impl_->doc->dom_document.ev_insert = nullptr;
    }
    return impl_->view_sink.get();
#else
    return nullptr;
#endif
}

void Document::end_view_mutations() {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->view_batch_active) {
        if (std::getenv("AFFINEUI_ATTR_CHECK")) {
            std::fprintf(stderr, "[attrcheck] end_view_mutations: batch "
                                 "NOT active (early return)\n");
            std::fflush(stderr);
        }
        return;
    }
    impl_->view_batch_active = false;
    if (impl_->doc) {
        impl_->doc->dom_document.ev_insert = impl_->view_saved_ev_insert;
    }
    impl_->view_saved_ev_insert = nullptr;
    detail::settle_view_batch(*impl_);
    detail::debug_validate_attr_lists(*impl_, "end-view-mutations");
#endif
}

void Document::set_custom_paint(std::string_view name, CustomPaintFn fn) {
    if (name.empty()) return;
    if (fn) {
        impl_->paint_handlers[std::string(name)] = std::move(fn);
    } else {
        impl_->paint_handlers.erase(std::string(name));
    }
}

bool Document::request_custom_repaint(std::string_view name) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (name.empty()) return false;
    bool any = false;
    // Match against the block's CACHED attrs — element_of() is a linear
    // reverse lookup, so touching the element here made this scan
    // quadratic in document size (12 ms per camera move on DENDER).
    for (const auto& block : impl_->blocks) {
        for (const auto& [attr_name, attr_value] : block.attrs) {
            if (attr_name != "data-aui-paint") continue;
            if (attr_value == name) {
                detail::add_dirty_rect(*impl_, block.bounds);
                any = true;
            }
            break;
        }
    }
    // Geometry-only invalidation: the display list re-records (the
    // handler's ops changed) but no restyle/layout/reconcile runs.
    if (any) impl_->paint_dirty = true;
    return any;
#else
    (void)name;
    return false;
#endif
}

bool Document::set_attribute_by_id(std::string_view elem_id,
                                   std::string_view name,
                                   std::string_view value) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc || name.empty()) return false;
    auto* elem = detail::find_dom_element_by_id(*impl_, elem_id);
    if (!elem) return false;
    return detail::set_attribute_on_element(*impl_, elem, name, value);
#else
    (void)elem_id; (void)name; (void)value;
    return false;
#endif
}

bool Document::remove_attribute_by_id(std::string_view elem_id,
                                      std::string_view name) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc || name.empty()) return false;
    auto* elem = detail::find_dom_element_by_id(*impl_, elem_id);
    if (!elem) return false;
    return detail::remove_attribute_on_element(*impl_, elem, name);
#else
    (void)elem_id; (void)name;
    return false;
#endif
}

bool Document::set_text_by_id(std::string_view elem_id,
                              std::string_view text) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc) return false;
    auto* elem = detail::find_dom_element_by_id(*impl_, elem_id);
    if (!elem || detail::element_has_element_child(elem)) return false;

    int target_idx = -1;
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        if (impl_->style_store.element_of(impl_->blocks[i].id) == elem) {
            target_idx = static_cast<int>(i);
            break;
        }
    }
    if (target_idx < 0) return false;

    auto& block = impl_->blocks[static_cast<std::size_t>(target_idx)];
    int text_idx = -1;
    bool has_unsupported_descendant = false;
    for (int idx = target_idx + 1;
         idx < static_cast<int>(impl_->blocks.size()); ++idx) {
        if (!detail::is_descendant_of_or_self(impl_->blocks, idx, target_idx)) continue;
        const auto& child = impl_->blocks[static_cast<std::size_t>(idx)];
        if (child.synthetic) continue;
        if (child.tag == "#text") {
            if (text_idx < 0) text_idx = idx;
            continue;
        }
        has_unsupported_descendant = true;
        break;
    }
    if (has_unsupported_descendant) return false;

    if (text_idx < 0 && block.text == text) return true;
    if (text_idx >= 0 &&
        impl_->blocks[static_cast<std::size_t>(text_idx)].text == text) {
        return true;
    }

    const Rect old_rect = detail::subtree_visual_rect(*impl_, target_idx);
    auto* node = lxb_dom_interface_node(elem);
    if (lxb_dom_node_text_content_set(node, detail::as_lxb(text), text.size())
            != LXB_STATUS_OK) {
        return false;
    }

    if (text_idx >= 0) {
        auto& text_block = impl_->blocks[static_cast<std::size_t>(text_idx)];
        const auto& cs = impl_->style_store.computed(text_block.id);
        text_block.text = detail::apply_text_transform(
            detail::node_text(node, cs.white_space), cs.text_transform);
        for (int idx = text_idx + 1;
             idx < static_cast<int>(impl_->blocks.size()); ++idx) {
            if (!detail::is_descendant_of_or_self(impl_->blocks, idx, target_idx))
                continue;
            auto& child = impl_->blocks[static_cast<std::size_t>(idx)];
            if (child.tag == "#text") child.text.clear();
        }
    } else {
        const auto& cs = impl_->style_store.computed(block.id);
        block.text = detail::apply_text_transform(
            detail::node_text(node, cs.white_space), cs.text_transform);
        block.placeholder_visible = false;
    }
    detail::mark_live_mutation_dirty(*impl_, target_idx, old_rect,
                             /*needs_layout=*/true);
    return true;
#else
    (void)elem_id; (void)text;
    return false;
#endif
}

std::vector<Rect> Document::take_dirty_rects() {
    std::vector<Rect> out;
    out.swap(impl_->dirty_rects);
    return out;
}

bool Document::take_paint_dirty() {
    const bool dirty = impl_->paint_dirty;
    impl_->paint_dirty = false;
    return dirty;
}

bool Document::has_active_animations() const {
#if !defined(AFFINEUI_STUB_BUILD)
    if (impl_->animation_candidate_count == 0) return false;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& b : impl_->blocks) {
        if (!b.animation.active || !detail::find_keyframes(*impl_, b.animation.name_hash))
            continue;
        const double elapsed_s = std::chrono::duration<double>(
            now - b.animation_epoch).count();
        float t = 0.0f;
        bool applies = false;
        if (animation_progress_at(b.animation, elapsed_s, t, applies)) {
            return true;
        }
    }
#endif
    return false;
}

void Document::set_animation_time_for_testing(double seconds) {
#if !defined(AFFINEUI_STUB_BUILD)
    const double clamped = std::max(0.0, seconds);
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(clamped));
    impl_->animation_epoch = std::chrono::steady_clock::now() - elapsed;
    for (auto& b : impl_->blocks) {
        b.animation_epoch = impl_->animation_epoch;
    }
#else
    (void)seconds;
#endif
}

// â”€â”€ Immediate mode â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void Document::set_imm_view(std::function<void()> view_fn) {
    if (!impl_->imm) impl_->imm = std::make_unique<detail::ImmRuntime>();
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc) {
        // No DOM yet â€” establish a minimal empty document so the
        // runtime has a body to mutate. set_html("") goes through the
        // normal parse path and ends with an empty <body>.
        set_html("");
    }
    impl_->imm->bind(this, impl_->doc);
#endif
    impl_->imm->set_view_fn(std::move(view_fn));
}

bool Document::imm_dirty() const {
    return impl_->imm && impl_->imm->dirty() && impl_->imm->has_view_fn();
}

void Document::invalidate_imm() {
    if (impl_->imm) impl_->imm->mark_dirty();
}

void Document::tick_imm() {
    if (!impl_->imm || !impl_->imm->has_view_fn()) return;
    if (!impl_->imm->dirty()) return;

#if !defined(AFFINEUI_STUB_BUILD)
    // 1. Run the view fn â€” it mutates lexbor's DOM directly via the
    //    runtime, replacing the body's children.
    impl_->imm->run_view_fn();

    // 2. Re-cascade + re-collect. This is the same tail as set_html
    //    after parsing â€” minus the stylesheet re-attach (those are
    //    still bound to impl_->doc from the original set_html).
    detail::recollect_blocks_from_current_dom(*impl_);
#endif
}

bool Document::invoke_imm_click(std::string_view elem_id) {
    return impl_->imm && impl_->imm->invoke_click(elem_id);
}

}  // namespace affineui
