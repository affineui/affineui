// affineui::Document â€” Phase 2A.
//
// Parses HTML with lexbor, attaches stylesheets (user-agent + user +
// any embedded `<style>` blocks) through lexbor's cascade, then for
// each block-level element collects a `ResolvedStyle` (ComputedStyle
// + AnimatedStyle) via the StyleResolver. The Phase 1 `style_for(tag)`
// fallback is gone â€” real CSS now drives every visible attribute we
// expose this phase.
//
// Lifetime: the lxb_html_document_t and the resolver live for the
// lifetime of DocumentImpl. Element pointers in our Block list remain
// valid until the next set_html() (which replaces the document).
//
// What's intentionally still simple (Phase 2B-2E plan):
//   - One flat list of block-level elements; no nested boxes.
//   - Painter is invoked directly each frame (no DisplayList yet).
//   - Resolver is uncached. We pay the walk on every set_html, never
//     per frame.

#include "affineui/document.h"

#include "affineui/memory.h"
#include "affineui/painter.h"
#include "affineui/themes.h"
#include "affineui/view.h"
#include "imm/imm_runtime.h"
#include "internal/animated_style.h"
#include "internal/computed_style.h"
#include "internal/diag.h"
#include "internal/element_id.h"
#include "internal/embed_log.h"
#include "internal/style_resolver.h"
#include "internal/style_store.h"
#include "layout/yoga_adapter.h"

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

// Shared document-internal types (Block, DocumentImpl, CSS side tables).
#include "dom/document_impl.h"

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

bool dock_trace_enabled() {
    if (dock_trace_capture_slot()) return true;
    const std::string flag = env_value("AFFINEUI_DOCK_TRACE");
    if (!flag.empty() && flag != "0") return true;
    return !env_value("AFFINEUI_DOCK_TRACE_FILE").empty();
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void dock_trace(std::string msg) {
    if (!dock_trace_enabled()) return;
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

int hit_test_blocks_for_dock_target(const detail::DocumentImpl& impl,
                                    int x,
                                    int y) {
    return hit_test_blocks_impl(impl, x, y, true);
}

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

namespace {
#if !defined(AFFINEUI_STUB_BUILD)
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
// state_bits) on top of `rs`. Shared by restyle_block (dispatch
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
        if (!is_descendant_of_or_self(impl.blocks, idx, root_idx)) break;
        needs_layout = restyle_block(impl, idx) || needs_layout;
    }
    return needs_layout;
}

bool restyle_all_blocks(detail::DocumentImpl& impl) {
    bool needs_layout = false;
    for (int idx = 0; idx < static_cast<int>(impl.blocks.size()); ++idx) {
        needs_layout = restyle_block(impl, idx) || needs_layout;
    }
    return needs_layout;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
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
        if (!is_descendant_of_or_self(impl.blocks, idx, root_idx)) break;
        out = union_rect(out, detail::block_visual_rect(impl, idx));
    }
    return out;
}
}  // namespace detail
namespace {


Rect document_visual_rect(const detail::DocumentImpl& impl) {
    Rect out{};
    for (int idx = 0; idx < static_cast<int>(impl.blocks.size()); ++idx) {
        out = union_rect(out, detail::block_visual_rect(impl, idx));
    }
    return out;
}

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

lxb_dom_element_t* element_for_block_or_ancestor(detail::DocumentImpl& impl,
                                                 int idx) {
    for (int cur = idx;
         cur >= 0 && cur < static_cast<int>(impl.blocks.size());
         cur = impl.blocks[static_cast<std::size_t>(cur)].parent_idx) {
        if (auto* elem = detail::element_for_block(impl, cur)) return elem;
    }
    return nullptr;
}

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
        if (auto* found = first_descendant_with_class(
                lxb_dom_interface_element(child), cls)) {
            return found;
        }
    }
    return nullptr;
}

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

lxb_dom_element_t* first_descendant_input(lxb_dom_element_t* elem) {
    if (!elem) return nullptr;
    if (detail::tag_name(elem) == "input") return elem;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (auto* found = first_descendant_input(
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
        if (auto* found = first_descendant_tag(
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
        if (!is_descendant_of_or_self(impl.blocks, idx, root_idx)) break;
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
        if (!is_descendant_of_or_self(impl.blocks, idx, root_idx)) break;
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

struct MutationTraceTimer {
    // AFFINEUI_MENU_TRACE=1: report any attribute mutation that costs
    // real time — the menu-lag class of bug is whole-document work
    // hiding inside these helpers.
    const char* op;
    std::string_view name;
    std::chrono::steady_clock::time_point t0;
    static bool enabled() {
        static const bool on = std::getenv("AFFINEUI_MENU_TRACE") != nullptr;
        return on;
    }
    MutationTraceTimer(const char* op_, std::string_view name_)
        : op(op_), name(name_) {
        if (enabled()) t0 = std::chrono::steady_clock::now();
    }
    ~MutationTraceTimer() {
        if (!enabled()) return;
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        if (ms >= 0.5) {
            std::fprintf(stderr, "[attr] %s '%s' took %.2f ms\n", op,
                         std::string(name).c_str(), ms);
        }
    }
};

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

bool set_attribute_on_element(detail::DocumentImpl& impl,
                              lxb_dom_element_t* elem,
                              std::string_view name,
                              std::string_view value) {
    if (!elem || name.empty()) return false;
    const bool already_present = detail::has_attr(elem, name);
    const std::string old_value =
        already_present ? detail::attr_string(elem, name) : std::string();
    if (already_present && old_value == value) return false;
    MutationTraceTimer trace_timer{"set", name};

    const int target_idx = detail::block_index_for_exact_element(impl, elem);
    const int dirty_root_idx =
        target_idx >= 0 ? target_idx
                        : block_index_for_element_or_ancestor(impl, elem);
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
                              : document_visual_rect(impl);
    const bool recollect_generated_subtree =
        selector_affecting &&
        generated_content_depends_on_attribute(impl, name, elem, old_value,
                                               value);

    if (!lxb_dom_element_set_attribute(elem, detail::as_lxb(name), name.size(),
                                       detail::as_lxb(value), value.size())) {
        return false;
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
            if (lxb_dom_interface_node(elem)->ns == LXB_NS_HTML &&
                attribute_matches_confined_to_subject(impl, name)) {
                // Element-local rematch is cheap — run it now so batch end
                // only re-matches subtrees for attrs whose rules escape the
                // subject.
                (void) lxb_html_document_element_styles_rematch(
                    lxb_html_interface_element(lxb_dom_interface_node(elem)));
                needs_subtree_rematch = false;
            }
        }
        bool force_layout = false;
        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            refresh_block_metadata_from_element(block, elem);
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
        if (lxb_dom_interface_node(elem)->ns == LXB_NS_HTML &&
            attribute_matches_confined_to_subject(impl, name)) {
            if (lxb_html_document_element_styles_rematch(
                    lxb_html_interface_element(lxb_dom_interface_node(elem)))
                != LXB_STATUS_OK) {
                return false;
            }
        } else if (!rematch_stylesheet_matches_for_subtree(
                       impl, mutation_dirty_root_idx)) {
            return false;
        }
        const double rematch_ms = phase();
        if (impl.resolver) impl.resolver->clear();

        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            refresh_block_metadata_from_element(block, elem);
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
            mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                     /*needs_layout=*/true);
            return true;
        }

        phase();
        needs_layout = mutation_dirty_root_idx >= 0
                           ? restyle_subtree(impl, mutation_dirty_root_idx)
                           : restyle_all_blocks(impl);
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
        if (selector_mutation_reveals_hidden_subtree(impl,
                                                     mutation_dirty_root_idx)) {
            detail::recollect_blocks_from_current_dom(impl);
            needs_layout = true;
        }
        const double reveal_ms = phase();
        if (MutationTraceTimer::enabled() &&
            rematch_ms + restyle_ms + reveal_ms >= 1.0) {
            std::fprintf(stderr,
                         "[attr]   set '%s' root=%d rematch=%.2f "
                         "restyle=%.2f reveal=%.2f\n",
                         std::string(name).c_str(), mutation_dirty_root_idx,
                         rematch_ms, restyle_ms, reveal_ms);
        }
        mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                 needs_layout);
        return true;
    }

    if (target_idx >= 0) {
        if (impl.resolver) impl.resolver->invalidate(elem);
        auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
        refresh_block_metadata_from_element(block, elem);
        needs_layout = restyle_subtree(impl, target_idx);
        if (block.tag == "img" && name == "src") needs_layout = true;
    }
    mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                             needs_layout);
    return true;
}

bool remove_attribute_on_element(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* elem,
                                 std::string_view name) {
    if (!elem || name.empty() || !detail::has_attr(elem, name)) return false;
    const std::string old_value = detail::attr_string(elem, name);
    MutationTraceTimer trace_timer{"remove", name};

    const int target_idx = detail::block_index_for_exact_element(impl, elem);
    const int dirty_root_idx =
        target_idx >= 0 ? target_idx
                        : block_index_for_element_or_ancestor(impl, elem);
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
                              : document_visual_rect(impl);
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
            if (lxb_dom_interface_node(elem)->ns == LXB_NS_HTML &&
                attribute_matches_confined_to_subject(impl, name)) {
                (void) lxb_html_document_element_styles_rematch(
                    lxb_html_interface_element(lxb_dom_interface_node(elem)));
                needs_subtree_rematch = false;
            }
        }
        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            refresh_block_metadata_from_element(block, elem);
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
        if (lxb_dom_interface_node(elem)->ns == LXB_NS_HTML &&
            attribute_matches_confined_to_subject(impl, name)) {
            if (lxb_html_document_element_styles_rematch(
                    lxb_html_interface_element(lxb_dom_interface_node(elem)))
                != LXB_STATUS_OK) {
                return false;
            }
        } else if (!rematch_stylesheet_matches_for_subtree(
                       impl, mutation_dirty_root_idx)) {
            return false;
        }
        const double rematch_ms = phase();
        if (impl.resolver) impl.resolver->clear();

        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            refresh_block_metadata_from_element(block, elem);
        }

        // Same policy as set_attribute_on_element: no unconditional box
        // rebuild for `hidden` — restyle the retained boxes, and let the
        // reveal check below recollect only when this removal exposes a
        // subtree whose boxes were never created (a menu's first open).
        if (recollect_generated_subtree) {
            detail::recollect_blocks_from_current_dom(impl);
            mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                     /*needs_layout=*/true);
            return true;
        }

        phase();
        needs_layout = mutation_dirty_root_idx >= 0
                           ? restyle_subtree(impl, mutation_dirty_root_idx)
                           : restyle_all_blocks(impl);
        const double restyle_ms = phase();
        // Removing an attribute can reveal a previously display:none
        // subtree ([hidden] most of all) whose boxes were never collected;
        // restyle can't create boxes, only a recollect can. This mirrors
        // the reveal check on the set_attribute path.
        phase();
        if (selector_mutation_reveals_hidden_subtree(impl,
                                                     mutation_dirty_root_idx)) {
            detail::recollect_blocks_from_current_dom(impl);
            needs_layout = true;
        }
        const double reveal_ms = phase();
        if (MutationTraceTimer::enabled() &&
            rematch_ms + restyle_ms + reveal_ms >= 1.0) {
            std::fprintf(stderr,
                         "[attr]   remove '%s' root=%d rematch=%.2f "
                         "restyle=%.2f reveal=%.2f\n",
                         std::string(name).c_str(), mutation_dirty_root_idx,
                         rematch_ms, restyle_ms, reveal_ms);
        }
        mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                 needs_layout);
        return true;
    }

    if (target_idx >= 0) {
        if (impl.resolver) impl.resolver->invalidate(elem);
        auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
        refresh_block_metadata_from_element(block, elem);
        needs_layout = restyle_subtree(impl, target_idx);
    }
    mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                             needs_layout);
    return true;
}

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
    mark_live_mutation_dirty(impl, target_idx, old_rect,
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

std::string widget_event_name(lxb_dom_element_t* elem) {
    if (!elem) return {};
    if (auto name = detail::attr_string(elem, "data-aui-name"); !name.empty()) {
        return name;
    }
    return detail::attr_string(elem, "id");
}

void emit_widget_change(detail::DocumentImpl& impl,
                        lxb_dom_element_t* elem,
                        std::string_view value) {
    auto name = widget_event_name(elem);
    if (name.empty()) return;
    impl.changed_widgets.push_back({std::move(name), std::string(value)});
}

void set_live_text_value(detail::DocumentImpl& impl,
                         int idx,
                         Block& block,
                         std::string value);
void set_live_text_state(detail::DocumentImpl& impl,
                         int idx,
                         Block& block,
                         std::string value,
                         std::size_t caret);
std::string detail::emitted_text_control_value(const Block& block);

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

constexpr int kLiveDragThresholdPx = 3;
constexpr int kTextareaResizeGripPx = 16;

bool pointer_moved_past_threshold(const Event& ev,
                                  const detail::DocumentImpl::LiveControlDrag& drag) {
    return std::abs(ev.pos.x - drag.start_x) >= kLiveDragThresholdPx ||
           std::abs(ev.pos.y - drag.start_y) >= kLiveDragThresholdPx;
}

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

bool update_live_control_value(detail::DocumentImpl& impl,
                               lxb_dom_element_t* elem,
                               LiveControlKind kind,
                               double min,
                               double max,
                               double value,
                               bool bipolar) {
    if (!elem) return false;
    if (max <= min) max = min + 1.0;
    const double clamped = std::clamp(value, min, max);
    const std::string value_text = detail::compact_number(clamped);

    bool changed = false;
    const bool value_changed =
        set_attribute_on_element(impl, elem, "value", value_text);
    changed = value_changed || changed;
    if (value_changed && kind == LiveControlKind::NumericInput) {
        const int idx = detail::block_index_for_exact_element(impl, elem);
        if (idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(idx)];
            if (block.text_control) {
                set_live_text_value(impl, idx, block, value_text);
            }
        }
        auto* combo = nearest_ancestor_with_class(elem, "dcs-combo");
        if (!combo && detail::has_attr(elem, "data-dcs-combo")) combo = elem;
        if (combo) {
            changed =
                set_attribute_on_element(impl, combo, "data-value", value_text) ||
                changed;
            changed =
                set_attribute_on_element(impl, combo, "aria-valuenow", value_text) ||
                changed;
            const double fill_min =
                element_attr_double(elem, "data-fill-min", min);
            const double fill_max =
                element_attr_double(elem, "data-fill-max", max);
            changed = set_attribute_on_element(
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
            set_attribute_on_element(impl, elem, "data-value", value_text) ||
            changed;
    }
    if (value_changed) emit_widget_change(impl, elem, value_text);

    if (kind == LiveControlKind::DeciusSlider) {
        if (auto* fill = first_descendant_with_class(elem, "dcs-slider__fill")) {
            changed = set_attribute_on_element(
                impl, fill, "style",
                decius_slider_fill_style(min, max, clamped, bipolar)) || changed;
        }
        if (auto* thumb = first_descendant_with_class(elem, "dcs-slider__thumb")) {
            changed = set_attribute_on_element(
                impl, thumb, "style",
                decius_slider_thumb_style(min, max, clamped)) || changed;
        }
    } else if (kind == LiveControlKind::DeciusFader) {
        const double fader_pos =
            1.0 - detail::normalized_control_value(clamped, min, max);
        changed = set_attribute_on_element(
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
        if (auto* label = first_descendant_with_class(elem, value_class)) {
            changed = set_text_on_element(impl, label, value_text) || changed;
        }
    }

    return changed;
}

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
        return set_attribute_on_element(
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
            ? nearest_ancestor_with_class(elem, "dcs-combo")
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
        out.min = element_attr_double(
            combo, "data-min",
            element_attr_double(elem, "min",
                element_attr_double(elem, "data-min", 0.0)));
        out.max = element_attr_double(
            combo, "data-max",
            element_attr_double(elem, "max",
                element_attr_double(elem, "data-max", 1.0)));
        if (has_fill_min_attr) {
            out.min = element_attr_double(
                combo, "data-fill-min",
                element_attr_double(elem, "data-fill-min", out.min));
        }
        if (has_fill_max_attr) {
            out.max = element_attr_double(
                combo, "data-fill-max",
                element_attr_double(elem, "data-fill-max", out.max));
        }
        out.start_value = element_attr_double(
            elem, "value", element_attr_double(elem, "data-value", out.min));
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
            out.step = element_attr_double(
                elem, "step",
                element_attr_double(
                    elem, "data-step",
                    element_attr_double(combo, "data-step", 0.01)));
            if (out.step <= 0.0) out.step = 0.01;
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
    mark_live_mutation_dirty(impl, drag.block_idx, old_rect,
                             /*needs_layout=*/true);
    return true;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool update_active_live_control(detail::DocumentImpl& impl, const Event& ev) {
    auto& drag = impl.live_drag;
    if (drag.kind == LiveControlKind::None || !drag.elem) return false;

    if (!drag.moved && pointer_moved_past_threshold(ev, drag)) {
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
        value = value_from_x(drag.bounds, ev.pos.x, drag.min, drag.max);
    } else if (drag.kind == LiveControlKind::NumericInput) {
        if (drag.bounded) {
            value = value_from_x(drag.bounds, ev.pos.x, drag.min, drag.max);
        } else {
            const double mult = ev.shift ? 4.0 : 1.0;
            const double current = element_attr_double(
                drag.elem, "value",
                element_attr_double(drag.elem, "data-value", drag.start_value));
            const double scaled_step =
                std::max(drag.step, std::abs(current) / 100.0);
            value = current +
                    static_cast<double>(ev.pos.x - drag.last_x) *
                        scaled_step * mult;
            drag.last_x = ev.pos.x;
        }
    } else if (drag.kind == LiveControlKind::DeciusFader) {
        value = value_from_y(drag.bounds, ev.pos.y, drag.min, drag.max);
    } else if (drag.kind == LiveControlKind::AuiKnob ||
               drag.kind == LiveControlKind::DeciusKnob) {
        value = drag.start_value +
                (static_cast<double>(drag.start_y - ev.pos.y) / 150.0) *
                    (drag.max - drag.min);
    }
    return update_live_control_value(impl, drag.elem, drag.kind, drag.min,
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
namespace {

bool class_list_contains(lxb_dom_element_t* elem, std::string_view cls);

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
    const bool peer_radio = class_list_contains(peer, "dcs-radio") ||
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
                changed = remove_attribute_on_element(
                    impl, elem, "checked") || changed;
            } else {
                changed = set_attribute_on_element(
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
        block.tag == "input" ? elem : first_descendant_input(elem);
    lxb_dom_element_t* decius_control = nullptr;
    if (class_list_contains(elem, "dcs-check") ||
        class_list_contains(elem, "dcs-radio") ||
        class_list_contains(elem, "dcs-switch")) {
        decius_control = elem;
    } else {
        decius_control = first_descendant_with_class(elem, "dcs-check");
        if (!decius_control) {
            decius_control = first_descendant_with_class(elem, "dcs-radio");
        }
        if (!decius_control) {
            decius_control = first_descendant_with_class(elem, "dcs-switch");
        }
    }
    lxb_dom_element_t* visual_check =
        first_descendant_with_class(elem, "dcs-check__box");
    if (!visual_check) {
        visual_check = detail::block_has_class(block, "dcs-check") ||
                       detail::block_has_class(block, "dcs-radio")
            ? elem
            : first_descendant_with_class(elem, "dcs-check");
    }
    const bool radio = (input && detail::attr_string(input, "type") == "radio") ||
                       detail::block_has_class(block, "dcs-radio") ||
                       class_list_contains(elem, "dcs-radio") ||
                       (decius_control != nullptr &&
                        class_list_contains(decius_control, "dcs-radio"));
    const bool old_checked = input
        ? detail::has_attr(input, "checked")
        : (decius_control
               ? element_attr_true(decius_control, "aria-checked")
               : element_attr_true(elem, "aria-checked"));
    const bool checked = radio ? true : !old_checked;

    bool changed = false;
    if (radio && checked) {
        changed = uncheck_radio_peers(
            impl, elem, radio_group_name(elem, input)) || changed;
    }
    if (input) {
        changed = checked
            ? (set_attribute_on_element(impl, input, "checked", "checked") || changed)
            : (remove_attribute_on_element(impl, input, "checked") || changed);
    }
    if (input != elem || detail::has_attr(elem, "aria-checked") ||
        detail::block_has_class(block, "dcs-check") ||
        detail::block_has_class(block, "dcs-radio") ||
        detail::block_has_class(block, "dcs-switch") ||
        detail::block_attr_value(block, "data-aui-widget")) {
        changed = set_attribute_on_element(
            impl, elem, "aria-checked", checked ? "true" : "false") ||
            changed;
    }
    if (decius_control != nullptr && decius_control != elem) {
        changed = set_attribute_on_element(
            impl, decius_control, "aria-checked",
            checked ? "true" : "false") || changed;
    }
    if (visual_check != nullptr && visual_check != elem) {
        changed = set_attribute_on_element(
            impl, visual_check, "aria-checked",
            checked ? "true" : "false") || changed;
    }
    auto* wrapper = nearest_checkbox_wrapper(elem);
    if (wrapper != nullptr && wrapper != elem &&
        detail::attr_string(wrapper, "data-aui-widget") == "checkbox") {
        changed = checked
            ? (set_attribute_on_element(impl, wrapper, "aria-checked", "true") || changed)
            : (remove_attribute_on_element(impl, wrapper, "aria-checked") || changed);
    }
    if (changed) {
        emit_widget_change(impl, wrapper, checked ? "true" : "false");
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

bool class_list_contains(lxb_dom_element_t* elem, std::string_view cls) {
    return detail::class_tokens_contain(detail::attr_view(elem, "class"), cls);
}

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

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Add/remove a class on a live element (re-matches selectors, restyles).
bool set_element_class(detail::DocumentImpl& impl,
                       lxb_dom_element_t* elem,
                       std::string_view cls,
                       bool present) {
    if (!elem) return false;
    if (class_list_contains(elem, cls) == present) return false;
    return set_attribute_on_element(impl, elem, "class",
                                    class_list_set(elem, cls, present));
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
    if (class_list_contains(elem, cls) == present) return false;
    const std::string next = class_list_set(elem, cls, present);
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
        refresh_block_metadata_from_element(
            impl.blocks[static_cast<std::size_t>(block_idx)], elem);
        (void) restyle_subtree(impl, block_idx);
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
            class_list_contains(vec_elem, "dcs-vec--stacked");
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

struct HsvColor {
    double h{210.0};
    double s{0.7};
    double v{0.85};
};

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
    if (auto* chip = first_descendant_with_class(
            field, "dcs-colorfield__chip")) {
        if (try_value(detail::attr_string(chip, "data-dcs-color"), out)) return out;
    }
    if (auto* input = first_descendant_with_class(
            field, "dcs-colorfield__hex")) {
        if (try_value(detail::attr_string(input, "value"), out)) return out;
    }
    return fallback;
}

bool sync_dcs_colorfield(detail::DocumentImpl& impl,
                         lxb_dom_element_t* field,
                         HsvColor hsv,
                         bool emit) {
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
    changed = set_attribute_on_element(impl, field, "data-value", hex) ||
              changed;
    if (owner && owner != field) {
        changed = set_attribute_on_element(impl, owner, "data-value", hex) ||
                  changed;
    }
    if (auto* chip = first_descendant_with_class(
            field, "dcs-colorfield__chip")) {
        changed = set_attribute_on_element(impl, chip, "data-dcs-color", hex) ||
                  changed;
        changed = set_attribute_on_element(
                      impl, chip, "style",
                      detail::style_with_properties(
                          detail::attr_string(chip, "style"),
                          {{"--c", hex}, {"background", hex}})) ||
                  changed;
    }
    if (auto* input = first_descendant_with_class(
            field, "dcs-colorfield__hex")) {
        changed = set_attribute_on_element(impl, input, "value", hex) ||
                  changed;
        const int idx = detail::block_index_for_exact_element(impl, input);
        if (idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(idx)];
            if (block.text_control) {
                set_live_text_value(impl, idx, block, hex);
            }
        }
    }
    if (auto* preview_chip = first_descendant_with_class(
            field, "dcs-colorfield__picker-chip")) {
        changed = set_attribute_on_element(
                      impl, preview_chip, "style",
                      detail::style_with_properties(
                          detail::attr_string(preview_chip, "style"),
                          {{"--c", hex}, {"background", hex}})) ||
                  changed;
    }
    if (auto* preview_input = first_descendant_with_class(
            field, "dcs-colorfield__picker-input")) {
        changed = set_attribute_on_element(impl, preview_input, "value", hex) ||
                  changed;
        const int idx = detail::block_index_for_exact_element(impl, preview_input);
        if (idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(idx)];
            if (block.text_control) {
                set_live_text_value(impl, idx, block, hex);
            }
        }
    }
    if (auto* square = first_descendant_with_class(
            field, "dcs-color-square")) {
        changed = set_attribute_on_element(
                      impl, square, "style",
                      detail::style_with_properties(
                          detail::attr_string(square, "style"),
                          {{"--hue", hue_hex},
                           {"aspect-ratio", "1.4 / 1"}})) ||
                  changed;
        if (auto* cursor = first_descendant_with_class(
                square, "dcs-color-square__cursor")) {
            changed = set_attribute_on_element(
                          impl, cursor, "style",
                          detail::style_with_properties(
                              detail::attr_string(cursor, "style"),
                              {{"left", detail::percent_string(hsv.s)},
                               {"top", detail::percent_string(1.0 - hsv.v)}})) ||
                      changed;
        }
    }
    if (auto* hue = first_descendant_with_class(field, "dcs-hue-bar")) {
        if (auto* cursor = first_descendant_with_class(
                hue, "dcs-hue-bar__cursor")) {
            changed = set_attribute_on_element(
                          impl, cursor, "style",
                          detail::style_with_properties(
                              detail::attr_string(cursor, "style"),
                              {{"left", detail::percent_string(hsv.h / 360.0)}})) ||
                      changed;
        }
    }

    if (emit && hex != previous) {
        emit_widget_change(impl, owner ? owner : field, hex);
    }
    return changed;
}

bool sync_dcs_colorfield(detail::DocumentImpl& impl,
                         lxb_dom_element_t* field,
                         std::string_view raw_hex,
                         bool emit) {
    const std::string hex = normalize_hex_color(raw_hex);
    if (hex.empty()) return false;
    return sync_dcs_colorfield(impl, field,
                               hsv_from_hex(hex, HsvColor{}), emit);
}

bool colorfield_part_kind(lxb_dom_element_t* elem,
                          detail::DocumentImpl::ColorfieldDrag::Kind& kind) {
    using Kind = detail::DocumentImpl::ColorfieldDrag::Kind;
    if (class_list_contains(elem, "dcs-colorfield__chip")) {
        kind = Kind::Chip;
        return true;
    }
    if (class_list_contains(elem, "dcs-color-square")) {
        kind = Kind::Square;
        return true;
    }
    if (class_list_contains(elem, "dcs-hue-bar")) {
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
        if (out_part && class_list_contains(elem, "dcs-colorfield")) {
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
        auto* field = nearest_ancestor_with_class(elem, "dcs-colorfield");
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
    const HsvColor hsv = current_dcs_colorfield_hsv(field);
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
    return sync_dcs_colorfield(impl, drag.field, next, /*emit=*/true);
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
         class_list_contains(elem, "dcs-btn"))) {
        const bool active = button_group_option_value(elem) == selected;
        changed = set_attribute_on_element(
            impl, elem, "aria-pressed", active ? "true" : "false") ||
            changed;
        if (class_list_contains(elem, "btn")) {
            changed = set_attribute_on_element(
                impl, elem, "class",
                active ? "btn btn-primary" : "btn btn-outline-primary") || changed;
        }
        if (class_list_contains(elem, "dcs-btn")) {
            changed = set_attribute_on_element(
                impl, elem, "class",
                class_list_set(elem, "dcs-btn--primary", active)) || changed;
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
        set_attribute_on_element(impl, group, "data-value", selected);
    changed = update_button_group_option_states(impl, group, selected) || changed;
    if (changed) emit_widget_change(impl, group, selected);
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
            set_attribute_on_element(impl, elem, "value", selected) || changed;
    } else if (tag == "option" && detail::has_attr(elem, "value")) {
        const bool active = detail::attr_string(elem, "value") == selected;
        changed = active
            ? (set_attribute_on_element(impl, elem, "selected", "selected") || changed)
            : (remove_attribute_on_element(impl, elem, "selected") || changed);
    } else if (tag == "button" && detail::has_attr(elem, "value") &&
               detail::attr_string(elem, "role") == "option") {
        const bool active = detail::attr_string(elem, "value") == selected;
        changed = active
            ? (set_attribute_on_element(impl, elem, "aria-selected", "true") || changed)
            : (remove_attribute_on_element(impl, elem, "aria-selected") || changed);
        if (class_list_contains(elem, "dropdown-item")) {
            changed = set_attribute_on_element(
                impl, elem, "class", class_list_set(elem, "active", active)) || changed;
        }
        if (class_list_contains(elem, "dcs-menu__item")) {
            changed = set_attribute_on_element(
                impl, elem, "class",
                class_list_set(elem, "dcs-menu__item--active", active)) || changed;
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

int viewport_width_for_overlay(const detail::DocumentImpl& impl) {
    if (impl.media_viewport_width_px > 0) return impl.media_viewport_width_px;
    return std::max(1, impl.content_size.width);
}

int viewport_height_for_overlay(const detail::DocumentImpl& impl) {
    if (impl.media_viewport_height_px > 0) return impl.media_viewport_height_px;
    return std::max(1, impl.content_size.height);
}

int overlay_item_count(lxb_dom_element_t* elem) {
    if (!elem) return 0;
    int count = 0;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* child_elem = lxb_dom_interface_element(child);
        if (class_list_contains(child_elem, "dcs-menu__item") ||
            class_list_contains(child_elem, "dropdown-item") ||
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
    const int viewport_w = viewport_width_for_overlay(impl);
    const int viewport_h = viewport_height_for_overlay(impl);
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
    auto* menu = first_descendant_with_class(group, "aui-select__menu");
    if (!menu) return false;
    bool changed = set_attribute_on_element(impl, menu, "hidden", "");
    changed = remove_attribute_on_element(impl, menu, "style") || changed;
    return changed;
}

std::string dropdown_menu_open_style(const detail::DocumentImpl& impl,
                                     lxb_dom_element_t* group,
                                     lxb_dom_element_t* menu) {
    int width = 160;
    Rect anchor_rect{0, 0, width, 1};
    auto* anchor = first_descendant_tag(group, "select");
    if (!anchor) anchor = group;
    const int anchor_idx = block_index_for_element_or_ancestor(impl, anchor);
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
    auto* menu = first_descendant_with_class(group, "aui-select__menu");
    if (!menu) return false;
    if (!detail::has_attr(menu, "hidden")) {
        return hide_dropdown_menu(impl, group);
    }
    const std::string open_style = dropdown_menu_open_style(impl, group, menu);
    bool changed = detail::close_transient_layers(impl, menu);
    changed = remove_attribute_on_element(impl, menu, "hidden") || changed;
    changed = set_attribute_on_element(
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
        set_attribute_on_element(impl, group, "data-value", selected);
    changed = update_dropdown_selection_states(impl, group, selected) || changed;
    changed = hide_dropdown_menu(impl, group) || changed;
    if (changed) emit_widget_change(impl, group, selected);
    return changed;
}
}  // namespace detail
namespace {

template <class Fn>
void walk_dom_elements(lxb_dom_node_t* node, Fn& fn) {
    if (!node) return;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        fn(lxb_dom_interface_element(node));
    }
    for (auto* child = lxb_dom_node_first_child(node);
         child != nullptr; child = lxb_dom_node_next(child)) {
        walk_dom_elements(child, fn);
    }
}

lxb_dom_node_t* document_dom_root(detail::DocumentImpl& impl) {
    if (!impl.doc) return nullptr;
    auto* body = lxb_html_document_body_element(impl.doc);
    return body ? lxb_dom_interface_node(body) : lxb_dom_interface_node(impl.doc);
}

bool close_dropdown_menu_element(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* menu) {
    if (!menu) return false;
    bool changed = set_attribute_on_element(impl, menu, "hidden", "");
    changed = remove_attribute_on_element(impl, menu, "style") || changed;
    return changed;
}

bool close_all_dropdown_menus(detail::DocumentImpl& impl,
                              lxb_dom_element_t* except = nullptr) {
    std::vector<lxb_dom_element_t*> menus;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (elem != except && class_list_contains(elem, "aui-select__menu") &&
            !detail::has_attr(elem, "hidden")) {
            menus.push_back(elem);
        }
    };
    walk_dom_elements(document_dom_root(impl), collect);

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

lxb_dom_element_t* dcs_target_for_trigger(detail::DocumentImpl& impl,
                                          lxb_dom_element_t* trigger) {
    if (!trigger) return nullptr;
    auto selector = detail::attr_string(trigger, "data-dcs-target");
    if (selector.empty()) selector = detail::attr_string(trigger, "href");
    const auto target_id = target_id_from_selector(selector);
    return target_id.empty() ? nullptr : detail::find_dom_element_by_id(impl, target_id);
}

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
                             : class_list_contains(e, name);
        if (m) return e;
    }
    return nullptr;
}

Rect root_float_host_bounds(detail::DocumentImpl& impl) {
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* e = detail::element_for_block(impl, i);
        if (e && class_list_contains(e, "dcs-dock--floathost")) {
            return impl.blocks[static_cast<std::size_t>(i)].bounds;
        }
    }
    return {};
}

Rect document_float_host_bounds(detail::DocumentImpl& impl) {
    Rect fallback = root_float_host_bounds(impl);
    Rect best{};
    long long best_area = 0;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* e = detail::element_for_block(impl, i);
        if (!e || !detail::has_attr(e, "data-dcs-float-host")) continue;
        if (class_list_contains(e, "dcs-dock--floathost")) continue;
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
    if (class_list_contains(elem, "dcs-panel--floating")) return true;
    return class_list_contains(elem, "dcs-toolbar--floating") &&
           detail::attr_string(elem, "data-dcs-resize") == "true";
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool find_float_resize_at(detail::DocumentImpl& impl, int from_idx, Point point,
                          detail::DocumentImpl::FloatResize& out) {
    int explicit_dir = 0;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = detail::element_for_block(impl, idx);
        if (elem && (class_list_contains(elem, "dcs-dockpane__tab") ||
                     class_list_contains(elem, "dcs-dockpane__tab-close") ||
                     class_list_contains(elem, "dcs-panel__title--dock-tab"))) {
            return false;
        }
        if (elem && explicit_dir == 0 &&
            class_list_contains(elem, "dcs-panel__resize-zone")) {
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
        const auto cs = impl.style_store.computed(blk.id);
        const int cur_left =
            (cs.inset_has.left && !cs.inset_has.left_pct) ? cs.inset_left : 0;
        const int cur_top =
            (cs.inset_has.top && !cs.inset_has.top_pct) ? cs.inset_top : 0;
        out = {};
        out.elem = elem;
        out.dir = dir;
        out.start_x = point.x;
        out.start_y = point.y;
        out.elem_doc_x = blk.bounds.x;
        out.elem_doc_y = blk.bounds.y;
        out.elem_w = blk.bounds.w;
        out.elem_h = blk.bounds.h;
        out.cb_x = blk.bounds.x - cur_left;
        out.cb_y = blk.bounds.y - cur_top;
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
            const Rect hb = document_float_host_bounds(impl);
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
        if (class_list_contains(elem, "dcs-dockpane__tab") ||
            class_list_contains(elem, "dcs-dockpane__tab-close")) {
            return false;
        }
        const std::string tag = detail::tag_name(elem);
        if (tag == "button" || tag == "a" || tag == "input" ||
            tag == "select" || tag == "textarea" || tag == "label" ||
            class_list_contains(elem, "dcs-btn") ||
            class_list_contains(elem, "dcs-select") ||
            class_list_contains(elem, "dcs-slider") ||
            class_list_contains(elem, "dcs-fader") ||
            class_list_contains(elem, "dcs-knob") ||
            class_list_contains(elem, "dcs-combo")) {
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
        // The element's current left/top may come from the cascade (a class
        // rule) or a prior inline write, so read them from the COMPUTED style.
        // Deriving the containing-block origin as (doc pos - computed inset)
        // makes the drag math independent of where left/top was authored.
        const auto cs = impl.style_store.computed(blk.id);
        const int cur_left =
            (cs.inset_has.left && !cs.inset_has.left_pct) ? cs.inset_left : 0;
        const int cur_top =
            (cs.inset_has.top && !cs.inset_has.top_pct) ? cs.inset_top : 0;
        out = {};
        out.elem = elem;
        out.start_x = point.x;
        out.start_y = point.y;
        out.elem_doc_x = blk.bounds.x;
        out.elem_doc_y = blk.bounds.y;
        out.cb_x = blk.bounds.x - cur_left;
        out.cb_y = blk.bounds.y - cur_top;
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
            const Rect hb = document_float_host_bounds(impl);
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

bool update_float_drag(detail::DocumentImpl& impl, const Event& ev) {
    auto& d = impl.float_drag;
    if (!d.elem) return false;
    int x = d.elem_doc_x + (ev.pos.x - d.start_x);
    int y = d.elem_doc_y + (ev.pos.y - d.start_y);
    if (d.bounds_w > 0 && d.bounds_h > 0) {
        x = std::clamp(x, d.bounds_x,
                       std::max(d.bounds_x, d.bounds_x + d.bounds_w - d.elem_w));
        y = std::clamp(y, d.bounds_y,
                       std::max(d.bounds_y, d.bounds_y + d.bounds_h - d.elem_h));
    }
    return set_attribute_on_element(
        impl, d.elem, "style",
        with_float_position(detail::attr_string(d.elem, "style"), x - d.cb_x,
                            y - d.cb_y));
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
    return set_attribute_on_element(
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
        bool changed = set_attribute_on_element(
            impl, impl.tab_drag_ghost, "style", style);
        changed = remove_attribute_on_element(impl, impl.tab_drag_ghost,
                                              "hidden") || changed;
        return changed;
    }
    if (auto* existing = detail::find_dom_element_by_id(impl, "__dockghost")) {
        impl.tab_drag_ghost = existing;
        bool changed = set_attribute_on_element(impl, existing, "style",
                                                style);
        changed = remove_attribute_on_element(impl, existing, "hidden") ||
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
    bool changed = set_attribute_on_element(impl, ghost, "hidden", "");
    changed = set_attribute_on_element(
                  impl, ghost, "style",
                  "position:fixed;z-index:1000;pointer-events:none;"
                  "display:none;left:0px;top:0px") ||
              changed;
    impl.paint_dirty = impl.paint_dirty || changed;
    return changed;
}
}  // namespace detail
namespace {

bool is_dcs_menu_trigger(lxb_dom_element_t* elem) {
    return elem && detail::attr_string(elem, "data-dcs-toggle") == "menu" &&
           !detail::has_attr(elem, "disabled");
}

bool set_all_dcs_menu_triggers_expanded(detail::DocumentImpl& impl,
                                        std::string_view value) {
    std::vector<lxb_dom_element_t*> triggers;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (is_dcs_menu_trigger(elem)) triggers.push_back(elem);
    };
    walk_dom_elements(document_dom_root(impl), collect);

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
            set_attribute_on_element(impl, trigger, "aria-expanded", value) ||
            changed;
    }
    return changed;
}

bool close_dcs_menu(detail::DocumentImpl& impl, lxb_dom_element_t* menu) {
    if (!menu) return false;
    bool changed = set_attribute_on_element(impl, menu, "hidden", "");
    changed = remove_attribute_on_element(impl, menu, "style") || changed;
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
        if (elem != except && class_list_contains(elem, "dcs-menu") &&
            !class_list_contains(elem, "dcs-menu__sub") &&
            !detail::has_attr(elem, "hidden")) {
            menus.push_back(elem);
        }
    };
    walk_dom_elements(document_dom_root(impl), collect);

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
    int overlay_width = class_list_contains(menu, "aui-color-menu") ? 160 : 180;
    const bool stretch_to_anchor = class_list_contains(menu, "aui-color-menu");
    const int trigger_idx =
        block_index_for_element_or_ancestor(impl, trigger);
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
            set_attribute_on_element(impl, trigger, "aria-expanded", "false") ||
            changed;
        return changed;
    }

    const std::string open_style = dcs_menu_open_style(impl, trigger, menu);
    bool changed = detail::close_transient_layers(impl, menu);
    changed = remove_attribute_on_element(impl, menu, "hidden") || changed;
    changed =
        set_attribute_on_element(impl, menu, "style", open_style) ||
        changed;
    changed =
        set_attribute_on_element(impl, trigger, "aria-expanded", "true") ||
        changed;
    return changed;
}
}  // namespace detail
namespace {

bool is_dcs_popover_trigger(lxb_dom_element_t* elem) {
    return elem && detail::attr_string(elem, "data-dcs-toggle") == "popover" &&
           !detail::has_attr(elem, "disabled");
}

bool set_all_dcs_popover_triggers_expanded(detail::DocumentImpl& impl,
                                           std::string_view value) {
    std::vector<lxb_dom_element_t*> triggers;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (is_dcs_popover_trigger(elem)) triggers.push_back(elem);
    };
    walk_dom_elements(document_dom_root(impl), collect);

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
            set_attribute_on_element(impl, trigger, "aria-expanded", value) ||
            changed;
    }
    return changed;
}

bool close_dcs_popover(detail::DocumentImpl& impl, lxb_dom_element_t* popover) {
    if (!popover) return false;
    bool changed = set_attribute_on_element(impl, popover, "hidden", "");
    if (detail::has_attr(popover, "data-dcs-base-style")) {
        const std::string base = detail::attr_string(popover, "data-dcs-base-style");
        changed = base.empty()
            ? (remove_attribute_on_element(impl, popover, "style") || changed)
            : (set_attribute_on_element(impl, popover, "style", base) ||
               changed);
        changed =
            remove_attribute_on_element(impl, popover, "data-dcs-base-style") ||
            changed;
    } else {
        changed = remove_attribute_on_element(impl, popover, "style") ||
                  changed;
    }
    return changed;
}

bool close_all_dcs_popovers(detail::DocumentImpl& impl,
                            lxb_dom_element_t* except = nullptr) {
    std::vector<lxb_dom_element_t*> popovers;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (elem != except && class_list_contains(elem, "dcs-popover") &&
            !detail::has_attr(elem, "hidden")) {
            popovers.push_back(elem);
        }
    };
    walk_dom_elements(document_dom_root(impl), collect);

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
    if (auto* field = nearest_ancestor_with_class(trigger, "dcs-colorfield")) {
        anchor_elem = field;
    }
    const int trigger_idx =
        block_index_for_element_or_ancestor(impl, anchor_elem);
    if (trigger_idx >= 0) {
        anchor_rect = detail::block_border_visual_rect(impl, trigger_idx);
        if (class_list_contains(anchor_elem, "dcs-colorfield") &&
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
            set_attribute_on_element(impl, trigger, "aria-expanded", "false") ||
            changed;
        return changed;
    }

    if (auto* field = nearest_ancestor_with_class(trigger, "dcs-colorfield")) {
        sync_dcs_colorfield(impl, field, current_dcs_colorfield_hsv(field),
                            /*emit=*/false);
    }
    const std::string base_style = detail::attr_string(popover, "style");
    const std::string open_style =
        dcs_popover_open_style(impl, trigger, popover);
    bool changed = detail::close_transient_layers(impl, popover);
    changed = set_attribute_on_element(impl, popover, "data-dcs-base-style",
                                       base_style) ||
              changed;
    changed = remove_attribute_on_element(impl, popover, "hidden") || changed;
    changed =
        set_attribute_on_element(impl, popover, "style", open_style) ||
        changed;
    changed =
        set_attribute_on_element(impl, trigger, "aria-expanded", "true") ||
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
        if (!is_dcs_menu_trigger(elem)) continue;
        auto* menu = dcs_target_for_trigger(impl, elem);
        if (!menu || !class_list_contains(menu, "dcs-menu")) continue;
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
        if (!is_dcs_popover_trigger(elem)) continue;
        auto* popover = dcs_target_for_trigger(impl, elem);
        if (!popover || !class_list_contains(popover, "dcs-popover")) continue;
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
           class_list_contains(elem, "dcs-menu__item--disabled");
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
        if (!out_item && class_list_contains(elem, "dcs-menu__item")) {
            if (is_disabled_dcs_menu_item(elem)) return false;
            out_item = elem;
        }
        if (out_item && class_list_contains(elem, "dcs-menu")) {
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
        class_list_contains(item, "dcs-menu__item--active");
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
        if (class_list_contains(el, "dcs-menu")) menu = el;
    }
    if (detail::has_attr(menu, "data-aui-colorfield") &&
        detail::has_attr(item, "data-dcs-value")) {
        const auto value = detail::attr_string(item, "data-dcs-value");
        auto* colorfield =
            detail::find_dom_element_by_id(impl, detail::attr_string(menu, "data-aui-colorfield"));
        bool changed = false;
        if (colorfield) {
            changed =
                set_attribute_on_element(impl, colorfield, "data-value", value) ||
                changed;
            if (auto* chip =
                    first_descendant_with_class(colorfield,
                                                "dcs-colorfield__chip")) {
                changed = set_attribute_on_element(
                    impl, chip, "style",
                    "--c:" + value + ";background:" + value) || changed;
            }
            if (auto* hex =
                    first_descendant_with_class(colorfield,
                                                "dcs-colorfield__hex")) {
                changed = set_text_on_element(impl, hex, value) || changed;
            }
            emit_widget_change(impl, colorfield, value);
        }
        changed = close_dcs_menu(impl, menu) || changed;
        changed = set_all_dcs_menu_triggers_expanded(impl, "false") || changed;
        return changed;
    }
    if (detail::has_attr(item, "data-dcs-value")) {
        emit_widget_change(impl, menu, detail::attr_string(item, "data-dcs-value"));
    }
    if (class_list_contains(item, "dcs-menu__item--has-sub")) return false;

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
    return elem && (class_list_contains(elem, "dcs-list__item") ||
                    class_list_contains(elem, "dcs-tree__row"));
}

bool is_disabled_dcs_select_row(lxb_dom_element_t* elem) {
    return detail::has_attr(elem, "disabled") ||
           detail::attr_string(elem, "aria-disabled") == "true" ||
           class_list_contains(elem, "dcs-list__item--disabled") ||
           class_list_contains(elem, "dcs-tree__row--disabled");
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
    return set_attribute_on_element(
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

    emit_widget_change(impl, box, dcs_selected_rows_value(rows));
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
        if (class_list_contains(elem, "dcs-subpanel__close") ||
            class_list_contains(elem, "dcs-foldout__tools")) {
            return false;
        }
        if (class_list_contains(elem, "dcs-subpanel__header")) {
            auto* block = nearest_ancestor_with_class(elem, "dcs-subpanel");
            if (!block) return false;
            out_block = block;
            out_chevron =
                first_descendant_with_class(block, "dcs-subpanel__chevron");
            out_collapsed_class = "dcs-subpanel--collapsed";
            out_chevron_open_class = "dcs-subpanel__chevron--open";
            return true;
        }
        if (class_list_contains(elem, "dcs-foldout__header")) {
            auto* block = nearest_ancestor_with_class(elem, "dcs-foldout");
            if (!block) return false;
            out_block = block;
            out_chevron =
                first_descendant_with_class(block, "dcs-foldout__chevron");
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

    const bool collapsed = class_list_contains(block, collapsed_class);
    const bool next_collapsed = !collapsed;
    bool changed = set_attribute_on_element(
        impl, block, "class",
        class_list_set(block, collapsed_class, next_collapsed));
    if (chevron) {
        changed = set_attribute_on_element(
            impl, chevron, "class",
            class_list_set(chevron, chevron_open_class, !next_collapsed)) ||
                  changed;
    }
    // Flipping the collapsed class on the block is all decius does — the rule
    // `.dcs-foldout--collapsed > .dcs-foldout__body{display:none}` hides the
    // body via the cascade. set_attribute_on_element() restyles the subtree, so
    // that descendant rule must re-match the body. (If it doesn't, that is a
    // renderer cascade bug to fix in the renderer, not to paper over here.)
    emit_widget_change(impl, block, next_collapsed ? "closed" : "open");
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
        if (elem && class_list_contains(elem, "dcs-dockpane__tab")) {
            out_tab = elem;
            return true;
        }
    }
    return false;
}
}  // namespace detail
namespace {

std::string detail::pane_panel_id(lxb_dom_element_t* pane);
std::string detail::dockpane_tab_panel_id(lxb_dom_element_t* tab);

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
        if (class_list_contains(e, "dcs-dockpane")) {
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
        if (!class_list_contains(e, "dcs-dockpane__tab")) continue;
        changed = set_attribute_on_element(impl, e, "aria-selected",
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
                           ? remove_attribute_on_element(impl, e, "hidden")
                           : set_attribute_on_element(impl, e, "hidden", "")) ||
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
        walk_dom_elements(lxb_dom_interface_node(pane), collect);
        for (auto* tb : toolbars) {
            const bool match = detail::attr_string(tb, "data-dcs-tabtoolbar") == sel;
            changed = (match ? remove_attribute_on_element(impl, tb, "hidden")
                             : set_attribute_on_element(impl, tb, "hidden", "")) ||
                      changed;
        }
    }
    if (!pane_id.empty() && !active_id.empty()) {
        if (active_id == pane_id) impl.dock_active_tabs.erase(pane_id);
        else impl.dock_active_tabs[pane_id] = active_id;
        detail::dock_trace("active-tab pane=" + pane_id + " active=" + active_id);
        detail::dock_trace_state(impl, "after-active-tab");
    }
    emit_widget_change(impl, tab, "tab");
    return changed;
}

// Nearest ancestor (inclusive) carrying a class.
lxb_dom_element_t* ancestor_with_class(lxb_dom_element_t* e,
                                       std::string_view cls) {
    for (auto* n = e ? lxb_dom_interface_node(e) : nullptr; n;
         n = lxb_dom_node_parent(n)) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* el = lxb_dom_interface_element(n);
        if (class_list_contains(el, cls)) return el;
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
        if (!is_dcs_menu_trigger(elem)) return;
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
        if (!m || !class_list_contains(m, "dcs-menu") ||
            detail::has_attr(m, "hidden")) {
            return;
        }
        open_trigger = elem;
        open_menu = m;
    };
    walk_dom_elements(lxb_dom_interface_node(bar), collect);
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

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
lxb_dom_element_t* find_dockpane_tab_for_panel_id(detail::DocumentImpl& impl,
                                                  std::string_view panel_id) {
    if (panel_id.empty()) return nullptr;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* elem = detail::element_for_block(impl, i);
        if (!elem || !class_list_contains(elem, "dcs-dockpane__tab")) {
            continue;
        }
        const std::string tab_panel_id = detail::dockpane_tab_panel_id(elem);
        if (std::string_view(tab_panel_id) == panel_id) return elem;
    }
    return nullptr;
}
}  // namespace detail
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
        if (class_list_contains(e, cls)) return e;
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
        if (class_list_contains(e, "dcs-dockpane__tab") &&
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
            if (class_list_contains(e, "dcs-dockpane__tab")) out.push_back(e);
        }
    }
    return out;
}
}  // namespace detail
namespace {

std::string detail::dock_kind_of(lxb_dom_element_t* pane);  // defined with the
                                                    // drop-zone geometry below

bool is_floating_dock(lxb_dom_element_t* dock) {
    return dock && detail::dock_kind_of(dock) == "panels" &&
           detail::ancestor_with_class(dock, "dcs-panel--floating") != nullptr;
}

// Raw class toggle — surgery runs on possibly-detached nodes mid-gesture; the
// one finisher recollect restyles everything, so no impl-aware setter here.
void dock_set_class(lxb_dom_element_t* e, std::string_view cls, bool on) {
    if (!e) return;
    dock_set_attr(e, "class", class_list_set(e, cls, on));
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
        if (class_list_contains(e, "dcs-dockpane__tab")) last_tab = e;
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
    walk_dom_elements(lxb_dom_interface_node(dock), collect);
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
                walk_dom_elements(lxb_dom_interface_node(source), collect);
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
// splitter; collapse empty .dcs-dock wrappers; rebalance survivors to
// `flex:1 1 0` so the freed slice is reclaimed (not left as dead space).
void dock_unsplit_from_layout(detail::DocumentImpl& impl,
                              lxb_dom_element_t* node) {
    if (!node) return;
    auto* parent_node = lxb_dom_node_parent(lxb_dom_interface_node(node));
    auto* prev = detail::previous_element_sibling(lxb_dom_interface_node(node));
    auto* next = detail::next_element_sibling(lxb_dom_interface_node(node));
    if (prev && class_list_contains(prev, "dcs-splitter")) {
        lxb_dom_node_remove(lxb_dom_interface_node(prev));
    } else if (next && class_list_contains(next, "dcs-splitter")) {
        lxb_dom_node_remove(lxb_dom_interface_node(next));
    }
    lxb_dom_node_remove(lxb_dom_interface_node(node));
    if (!parent_node || parent_node->type != LXB_DOM_NODE_TYPE_ELEMENT) return;
    auto* parent = lxb_dom_interface_element(parent_node);
    if (!class_list_contains(parent, "dcs-dock")) return;
    std::vector<lxb_dom_element_t*> live;
    for (auto* c = lxb_dom_node_first_child(parent_node); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (!class_list_contains(e, "dcs-splitter")) live.push_back(e);
    }
    if (live.empty()) {
        // Never collapse the workspace root itself.
        if (!class_list_contains(parent, "dcs-dock--floathost")) {
            dock_unsplit_from_layout(impl, parent);
        }
        return;
    }
    // decius.js leaves single-child docks AS-IS: a dock holding one pane is the
    // normal one-panel state, not a degenerate wrapper to unwrap. We only
    // reclaim the freed slice by rebalancing the survivors to `flex:1 1 0`
    // (verbatim unsplitFromLayout), never by removing a nesting level.
    //
    // (An earlier build collapsed single-child wrappers to satisfy a "no
    // single-child split" invariant that decius does NOT have — and that
    // collapse ate the workspace-root dock after a tearoff-to-one-pane, leaving
    // floathost > pane, read back as "no dock present". The invariant, the
    // collapse, and the replay/validator special-cases it forced are all gone;
    // single-child docks are valid here exactly as in the JS.)
    for (auto* c : live) {
        dock_set_attr(c, "style", "flex:1 1 0;min-width:0;min-height:0");
    }
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
        parent && class_list_contains(parent, "dcs-dock");
    const bool parent_vertical =
        parent_is_dock && class_list_contains(parent, "dcs-dock--v");
    const bool need_vertical = !horizontal;

    const int target_idx = detail::block_index_for_exact_element(impl, target);
    const Rect tb = target_idx >= 0
                        ? impl.blocks[static_cast<std::size_t>(target_idx)].bounds
                        : Rect{};
    const int t_size = horizontal ? tb.w : tb.h;
    const int default_new = horizontal ? kDockNewPxH : kDockNewPxV;
    const int cap = std::max(96, t_size * 20 / 100);
    const int new_px =
        window_edge ? std::min(default_new, cap) : std::max(20, (t_size - 1) / 2);
    const int target_px =
        window_edge ? std::max(120, t_size - new_px - 1) : new_px;
    const std::string flex_min = ";min-width:0;min-height:0";
    auto flex_basis = [&](int px) {
        return "flex:1 1 " + std::to_string(px) + "px" + flex_min;
    };

    auto* splitter = dock_create_el(impl, "div", splitter_cls);
    dock_set_attr(splitter, "data-dcs-splitter", horizontal ? "" : "h");
    if (parent_is_dock && parent_vertical == need_vertical) {
        // Same-direction parent: lock the OTHER siblings to their current px so
        // the redistribution only carves the target's slice.
        for (auto* c = lxb_dom_node_first_child(parent_node); c;
             c = lxb_dom_node_next(c)) {
            if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            auto* e = lxb_dom_interface_element(c);
            if (e == target || class_list_contains(e, "dcs-splitter")) continue;
            const int bi = detail::block_index_for_exact_element(impl, e);
            if (bi < 0) continue;
            const auto& bb = impl.blocks[static_cast<std::size_t>(bi)].bounds;
            const int sz = horizontal ? bb.w : bb.h;
            if (sz > 0) dock_set_attr(e, "style", flex_basis(sz));
        }
        dock_set_attr(target, "style", flex_basis(target_px));
        dock_set_attr(fresh, "style", flex_basis(new_px));
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
        // of the matching direction; the wrapper INHERITS the target's slot.
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
        dock_set_attr(target, "style", flex_basis(target_px));
        dock_set_attr(fresh, "style", flex_basis(new_px));
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
    const Rect host = document_float_host_bounds(impl);
    const Rect root = root_float_host_bounds(impl);
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
        if (e && class_list_contains(e, "dcs-dock--floathost")) {
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
    const Rect old_rect = document_visual_rect(impl);
    const auto st0 = std::chrono::steady_clock::now();
    if (impl.resolver) impl.resolver->clear();
    // Elements created or re-parented by the surgery have no (or stale)
    // lexbor stylesheet attachments; rebuild the match lists before the
    // resolver walks them (same contract as set_attribute_on_element).
    rematch_stylesheet_matches_for_subtree(impl, -1);
    const auto st1 = std::chrono::steady_clock::now();
    detail::recollect_blocks_from_current_dom(impl);
    const auto st2 = std::chrono::steady_clock::now();
    if (MutationTraceTimer::enabled()) {
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
    mark_live_mutation_dirty(impl, -1, old_rect, /*needs_layout=*/true);
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
    const Rect host = document_float_host_bounds(impl);
    const Rect root = root_float_host_bounds(impl);
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
    if (class_list_contains(e, "dcs-dock")) {
        n.split = true;
        n.vertical = class_list_contains(e, "dcs-dock--v");
        for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(e)); c;
             c = lxb_dom_node_next(c)) {
            if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            auto* ce = lxb_dom_interface_element(c);
            if (class_list_contains(ce, "dcs-splitter")) continue;
            if (class_list_contains(ce, "dcs-dock") ||
                class_list_contains(ce, "dcs-dockpane")) {
                n.children.push_back(dock_layout_node(ce));
            }
        }
        return n;
    }
    // Leaf: a dockpane.
    n.kind = detail::dock_kind_of(e);
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
        if (!found && class_list_contains(e, cls)) found = e;
    };
    walk_dom_elements(root, collect);
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
        if (class_list_contains(e, "dcs-dock")) docks.push_back(e);
    };
    walk_dom_elements(document_dom_root(impl), collect);
    auto is_match = [&](lxb_dom_element_t* d) {
        return class_list_contains(d, "dcs-dock") &&
               class_list_contains(d, "dcs-dock--v") == want_v;
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
            if (class_list_contains(lxb_dom_interface_element(n), "dcs-dock")) {
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
    return class_list_contains(pane, "dcs-dockpane--center") ? "documents"
                                                             : "panels";
}

std::string pane_panel_id(lxb_dom_element_t* pane) {
    const std::string n = detail::attr_string(pane, "data-aui-name");  // pane-<id>
    return n.rfind("pane-", 0) == 0 ? n.substr(5) : std::string();
}
}  // namespace detail
namespace {

std::vector<std::string> dockpane_tab_ids(detail::DocumentImpl& impl,
                                          lxb_dom_element_t* pane) {
    std::vector<std::string> out;
    if (!pane) return out;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* elem = detail::element_for_block(impl, i);
        if (!elem || !class_list_contains(elem, "dcs-dockpane__tab")) {
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
        if (!pane || !class_list_contains(pane, "dcs-dockpane")) continue;
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
                    int_attr(pane, "data-aui-dock-side", 0)};
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
        if (!pane || !class_list_contains(pane, "dcs-dockpane")) continue;
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
                            int_attr(pane, "data-aui-dock-side", 0));
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
    if (!dock_trace_enabled()) return;
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
    if (!dock_trace_enabled()) return;
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
        p.x = int_attr(pane, "data-aui-dock-x", bounds.x);
        p.y = int_attr(pane, "data-aui-dock-y", bounds.y);
        p.w = int_attr(pane, "data-aui-dock-w", bounds.w);
        p.h = int_attr(pane, "data-aui-dock-h", bounds.h);
        return p;
    }
    if (!detail::has_attr(pane, "data-aui-dock-side")) return p;
    p.present = true;
    p.floating = false;
    p.parent = detail::attr_string(pane, "data-aui-dock-parent");
    p.side = int_attr(pane, "data-aui-dock-side", 0);
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
        if (!class_list_contains(e, "dcs-dockpane__tabbar")) continue;
        const int bi = detail::block_index_for_exact_element(impl, e);
        if (bi < 0) return false;
        const auto& b = impl.blocks[static_cast<std::size_t>(bi)].bounds;
        return pt.x >= b.x && pt.x < b.x + b.w && pt.y >= b.y && pt.y < b.y + b.h;
    }
    return false;
}

bool is_dockpane_top_chrome(lxb_dom_element_t* elem) {
    return elem &&
           (class_list_contains(elem, "dcs-dockpane__tabbar") ||
            class_list_contains(elem, "dcs-dockpane__titlebar") ||
            class_list_contains(elem, "dcs-dockpane__shelf"));
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
        if (e && class_list_contains(e, "dcs-dock--floathost")) {
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
    const int vw = viewport_width_for_overlay(impl);
    const int vh = viewport_height_for_overlay(impl);
    DropTarget window_edge;
    if (pt.x < kWin) window_edge.zone = DropZone::Left;
    else if (pt.x > vw - kWin) window_edge.zone = DropZone::Right;
    else if (pt.y < kWin) window_edge.zone = DropZone::Top;
    else if (pt.y > vh - kWin) window_edge.zone = DropZone::Bottom;
    if (window_edge.zone != DropZone::None) {
        window_edge.parent = "__document__";
        window_edge.valid = true;
        window_edge.window_edge = true;
        window_edge.pane = edge_owner_dock(impl, window_edge.zone);
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
    const int hit = hit_test_blocks_for_dock_target(impl, pt.x, pt.y);
    auto* hit_elem = element_for_block_or_ancestor(impl, hit);
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
        // decius dockDropDecision: kinds must match, and the source pane is
        // not a CENTER target for its own tab. Extension (upstreamed to
        // decius.js too): a MULTI-tab pane's edge zones ARE valid for its own
        // tab — "split Console out of Assets" in one gesture. Single-tab
        // self-drops stay free-space (tearoff).
        const bool self_drop = e == source_pane;
        if (detail::dock_kind_of(e) != drag_kind ||
            (self_drop && detail::dock_tabs(e).size() <= 1)) {
            return window_edge.valid ? window_edge : out;
        }
        const std::string id = detail::pane_panel_id(e);
        if (id.empty()) return window_edge.valid ? window_edge : out;
        out.parent = id;
        out.pane = e;
        const int lx = b.x - hx, ly = b.y - hy;
        if (detail::ancestor_with_class(e, "dcs-panel--floating") ||
            point_over_pane_tabbar(impl, e, pt)) {
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
        bool changed = set_attribute_on_element(impl, ind, "hidden", "");
        changed = set_attribute_on_element(
                      impl, ind, "style",
                      "position:absolute;pointer-events:none;z-index:200;"
                      "display:none;left:0px;top:0px;width:0px;height:0px") ||
                  changed;
        return changed;
    }
    bool changed = set_attribute_on_element(
        impl, ind, "style",
        "position:absolute;pointer-events:none;z-index:200;border:2px solid "
        "rgb(0,184,212);background:rgba(0,184,212,0.18);"
        "box-sizing:border-box;left:" +
            std::to_string(t->x) + "px;top:" + std::to_string(t->y) +
            "px;width:" + std::to_string(t->w) + "px;height:" +
            std::to_string(t->h) + "px");
    changed = remove_attribute_on_element(impl, ind, "hidden") || changed;
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
        if (class_list_contains(child_elem, "dcs-tree__row")) {
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

        changed = visible ? (remove_attribute_on_element(impl, row, "hidden") ||
                             changed)
                          : (set_attribute_on_element(impl, row, "hidden", "") ||
                             changed);

        if (auto* chevron =
                first_descendant_with_class(row, "dcs-tree__chevron")) {
            const bool has_child =
                dcs_tree_row_has_direct_child(rows, i, depth);
            changed = set_attribute_on_element(
                          impl, chevron, "class",
                          class_list_set(chevron, "dcs-tree__chevron--leaf",
                                         !has_child)) ||
                      changed;
            open_by_depth[static_cast<std::size_t>(depth)] =
                has_child && class_list_contains(
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
        if (class_list_contains(elem, "dcs-tree__chevron")) {
            if (class_list_contains(elem, "dcs-tree__chevron--leaf")) {
                return false;
            }
            out_chevron = elem;
        }
        if (out_chevron && !out_row &&
            class_list_contains(elem, "dcs-tree__row")) {
            out_row = elem;
        }
        if (out_chevron && out_row &&
            class_list_contains(elem, "dcs-tree")) {
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
        class_list_contains(chevron, "dcs-tree__chevron--open");
    bool changed = set_attribute_on_element(
        impl, chevron, "class",
        class_list_set(chevron, "dcs-tree__chevron--open", !open));
    changed = refresh_dcs_tree_visibility(impl, tree) || changed;
    emit_widget_change(impl, tree, !open ? "open" : "closed");
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
        if (!out_row && class_list_contains(elem, "dcs-tree__row")) {
            out_row = elem;
        }
        if (out_row && class_list_contains(elem, "dcs-tree")) {
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
    if (!row || !class_list_contains(row, "dcs-tree__row")) return out;
    const int root_depth = dcs_tree_row_depth(row);
    for (auto* cur = row; cur != nullptr;
         cur = detail::next_element_sibling(lxb_dom_interface_node(cur))) {
        if (!class_list_contains(cur, "dcs-tree__row")) break;
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
                first_descendant_with_class(row, "dcs-tree__chevron")) {
            const bool has_child =
                dcs_tree_row_has_direct_child(rows, i, depth);
            dock_set_class(chevron, "dcs-tree__chevron--leaf", !has_child);
            open_by_depth[static_cast<std::size_t>(depth)] =
                has_child &&
                class_list_contains(chevron, "dcs-tree__chevron--open");
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
    emit_widget_change(impl, drag.tree, "reorder");
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
           (class_list_contains(elem, "aui-select__menu") ||
            class_list_contains(elem, "dcs-menu") ||
            class_list_contains(elem, "dcs-popover"));
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
        if (auto* popover = nearest_ancestor_with_class(elem, "dcs-popover");
            popover && !detail::has_attr(popover, "hidden")) {
            return true;
        }
        if (auto* menu = nearest_ancestor_with_class(elem, "dcs-menu");
            menu && !detail::has_attr(menu, "hidden")) {
            return true;
        }
        if (auto* menu =
                nearest_ancestor_with_class(elem, "aui-select__menu");
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
        if (auto* popover = nearest_ancestor_with_class(elem, "dcs-popover");
            popover && !detail::has_attr(popover, "hidden")) {
            return true;
        }
        if (is_open_transient_layer(elem)) return true;
        if (is_dcs_menu_trigger(elem) || is_dcs_popover_trigger(elem)) {
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

lxb_dom_element_t* find_trigger_for_target(detail::DocumentImpl& impl,
                                           std::string_view target_selector) {
    if (target_selector.empty()) return nullptr;
    lxb_dom_element_t* out = nullptr;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (!out && detail::attr_string(elem, "data-dcs-target") == target_selector) {
            out = elem;
        }
    };
    walk_dom_elements(document_dom_root(impl), collect);
    return out;
}

}  // namespace

Document::TransientState Document::capture_transient_state() const {
    TransientState state;
#if !defined(AFFINEUI_STUB_BUILD)
    auto collect = [&](lxb_dom_element_t* elem) {
        if (!elem || detail::has_attr(elem, "hidden")) return;
        const bool popover = class_list_contains(elem, "dcs-popover");
        // Submenu cascades are hover-CSS state, not open layers — capturing
        // one would pin it "open" across a reload.
        const bool menu = (class_list_contains(elem, "dcs-menu") &&
                           !class_list_contains(elem, "dcs-menu__sub")) ||
                          class_list_contains(elem, "aui-select__menu");
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
    walk_dom_elements(document_dom_root(*impl_), collect);
#endif
    return state;
}

void Document::restore_transient_state(const TransientState& state) {
#if !defined(AFFINEUI_STUB_BUILD)
    for (const auto& layer : state.open_layers) {
        auto* elem = detail::find_dom_element_by_id(*impl_, layer.id);
        if (!elem) continue;
        if (layer.popover && !class_list_contains(elem, "dcs-popover")) {
            continue;
        }
        if (layer.menu && !(class_list_contains(elem, "dcs-menu") ||
                            class_list_contains(elem, "aui-select__menu"))) {
            continue;
        }
        bool changed = false;
        changed = remove_attribute_on_element(*impl_, elem, "hidden") || changed;
        if (!layer.style.empty()) {
            changed = set_attribute_on_element(*impl_, elem, "style",
                                               layer.style) ||
                      changed;
        }
        if (!layer.base_style.empty()) {
            changed = set_attribute_on_element(*impl_, elem,
                                               "data-dcs-base-style",
                                               layer.base_style) ||
                      changed;
        }
        if (auto* trigger =
                find_trigger_for_target(*impl_, layer.target_selector)) {
            changed = set_attribute_on_element(*impl_, trigger,
                                               "aria-expanded", "true") ||
                      changed;
        }
        if (layer.popover) {
            if (auto* field =
                    nearest_ancestor_with_class(elem, "dcs-colorfield")) {
                // A re-opened colorfield picker must show the field's CURRENT
                // value, exactly like a fresh caret-open does. The rebuild
                // re-emits the picker's SV/hue cursors at their defaults, so
                // without this sync a picker kept open across a reload (e.g.
                // right after a pick commits) snaps to a zeroed color even
                // though the committed value is safely in the model.
                changed = sync_dcs_colorfield(
                              *impl_, field, current_dcs_colorfield_hsv(field),
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
    auto new_chain = build_hover_chain(impl.blocks, target_idx);
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
            pseudo_state_reveals_hidden_subtree(impl, new_idx)) {
            *out_needs_recollect = true;
        }
    }
    for (int root_idx : changed_roots) {
        bool covered_by_ancestor = false;
        for (int other_idx : changed_roots) {
            if (other_idx == root_idx) continue;
            if (is_descendant_of_or_self(impl.blocks, root_idx, other_idx)) {
                covered_by_ancestor = true;
                break;
            }
        }
        if (covered_by_ancestor) continue;
        const Rect old_rect = detail::subtree_visual_rect(impl, root_idx);
        const bool needs_layout = restyle_subtree(impl, root_idx);
        mark_live_mutation_dirty(impl, root_idx, old_rect, needs_layout);
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
        const bool needs_layout = restyle_block(impl, old_idx);
        mark_live_mutation_dirty(impl, old_idx, old_rect, needs_layout);
    }
    if (target_idx >= 0 && target_idx < static_cast<int>(impl.blocks.size())) {
        const Rect old_rect = detail::subtree_visual_rect(impl, target_idx);
        const auto id = impl.blocks[static_cast<std::size_t>(target_idx)].id;
        impl.style_store.state_bits(id) |= kFocusStateBit;
        const bool needs_layout = restyle_block(impl, target_idx);
        mark_live_mutation_dirty(impl, target_idx, old_rect, needs_layout);
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

void set_live_text_value(detail::DocumentImpl& impl,
                         int idx,
                         Block& block,
                         std::string value) {
    const std::size_t caret = value.size();
    set_live_text_state(impl, idx, block, std::move(value), caret);
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
        if (class_list_contains(elem, "dcs-colorfield__hex") ||
            class_list_contains(elem, "dcs-colorfield__picker-input")) {
            if (auto* field = nearest_ancestor_with_class(elem,
                                                          "dcs-colorfield")) {
                sync_dcs_colorfield(impl, field,
                                    detail::emitted_text_control_value(block),
                                    /*emit=*/true);
            }
            return;
        }
        emit_widget_change(impl, elem, detail::emitted_text_control_value(block));
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
    set_live_text_state(impl, idx, block, std::move(next), caret);
    mark_live_mutation_dirty(impl, idx, old_rect, /*needs_layout=*/true);
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
    set_live_text_state(impl, idx, block, std::move(next), caret);
    mark_live_mutation_dirty(impl, idx, old_rect, /*needs_layout=*/true);
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
    bool changed = detail::set_focus(impl, drag.focus_idx);
    changed = detail::set_text_caret_from_point(impl, drag.focus_idx, point) || changed;
    return changed;
}
}  // namespace detail
namespace {

#else  // stub build â€” no DOM, no pseudo / scroll bookkeeping
}  // namespace

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
void set_live_text_value(detail::DocumentImpl&, int, Block&, std::string) {}
void set_live_text_state(detail::DocumentImpl&, int, Block&, std::string,
                         std::size_t) {}
}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::string emitted_text_control_value(const Block&) { return {}; }
}  // namespace detail
namespace {

#endif
}  // namespace

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
        class_list_contains(host, "dcs-dock")
            ? host
            : dock_child_with_class(host, "dcs-dock");
    if (!workdock) return out;
    out.present = true;
    out.root = dock_layout_node(workdock);
    // Floats: .dcs-panel--floating descendants of the host.
    std::vector<lxb_dom_element_t*> floats;
    auto collect = [&](lxb_dom_element_t* e) {
        if (class_list_contains(e, "dcs-panel--floating")) floats.push_back(e);
    };
    walk_dom_elements(lxb_dom_interface_node(host), collect);
    for (auto* fp : floats) {
        auto* pane = first_descendant_with_class(fp, "dcs-dockpane");
        if (!pane) continue;
        DockLayout::Float f;
        const std::string style = detail::attr_string(fp, "style");
        auto px = [&](std::string_view prop) {
            const std::string v = detail::find_decl_value(style, prop);
            return v.empty()
                       ? 0
                       : static_cast<int>(std::strtol(v.c_str(), nullptr, 10));
        };
        f.x = px("left");
        f.y = px("top");
        f.w = px("width");
        f.h = px("height");
        f.title_only = class_list_contains(pane, "dcs-dockpane--title-only") ||
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
        if (MutationTraceTimer::enabled()) {
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
    walk_dom_elements(lxb_dom_interface_node(body), collect);
    if (!found) return {};
    const int bi = detail::block_index_for_exact_element(*impl_, found);
    if (bi < 0) return {};
    return impl_->blocks[static_cast<std::size_t>(bi)].bounds;
#else
    (void) target;
    return {};
#endif
}

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
            const int idx = block_index_for_element_or_ancestor(
                impl_, lxb_dom_interface_element(parent));
            if (idx >= 0) {
                mark_live_mutation_dirty(impl_, idx,
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
                    set_text_on_element(
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
        if (!set_text_on_element(impl_, e, value)) {
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
            set_attribute_on_element(impl_, e, name, value);
            // ev_set_value keeps an EXISTING style attribute parsed; a
            // fresh one arrives through the suppressed ev_insert hook.
            if (fresh_style) parse_inline_style_attr(e);
        }
    }

    void remove_attribute(const WidgetNode& node,
                          std::string_view name) override {
        if (auto* e = elem(node.remote_id)) {
            remove_attribute_on_element(impl_, e, name);
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
        if (MutationTraceTimer::enabled()) {
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
                if (k == r || is_descendant_of_or_self(impl.blocks, r, k)) {
                    covered = true;
                    break;
                }
            }
            if (!covered) kept.push_back(r);
        }
        const Rect old_rect = document_visual_rect(impl);
        const auto t1 = std::chrono::steady_clock::now();
        for (const int r : kept) {
            if (auto* e = detail::element_for_block(impl, r)) {
                invalidate_resolver_deep(impl, lxb_dom_interface_node(e));
            }
            (void) rematch_stylesheet_matches_for_subtree(impl, r);
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
        mark_live_mutation_dirty(impl, -1, old_rect, /*needs_layout=*/true);
        impl.paint_dirty = true;
        if (MutationTraceTimer::enabled()) {
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
                     is_descendant_of_or_self(impl.blocks, r.root_idx,
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
                (void) rematch_stylesheet_matches_for_subtree(impl,
                                                              k.root_idx);
            }
        }
        if (impl.resolver) impl.resolver->clear();
        for (auto& k : kept) {
            const bool restyle_layout =
                k.root_idx >= 0 ? restyle_subtree(impl, k.root_idx)
                                : restyle_all_blocks(impl);
            k.force_layout = k.force_layout || restyle_layout;
        }
        bool recollected = false;
        for (const auto& k : kept) {
            if (selector_mutation_reveals_hidden_subtree(impl,
                                                         k.root_idx)) {
                detail::recollect_blocks_from_current_dom(impl);
                recollected = true;
                break;
            }
        }
        for (const auto& k : kept) {
            mark_live_mutation_dirty(impl, k.root_idx, k.old_rect,
                                     k.force_layout || recollected);
        }
        if (MutationTraceTimer::enabled()) {
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
    return set_attribute_on_element(*impl_, elem, name, value);
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
    return remove_attribute_on_element(*impl_, elem, name);
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
    if (!elem || element_has_element_child(elem)) return false;

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
        if (!is_descendant_of_or_self(impl_->blocks, idx, target_idx)) continue;
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
            if (!is_descendant_of_or_self(impl_->blocks, idx, target_idx))
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
    mark_live_mutation_dirty(*impl_, target_idx, old_rect,
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
