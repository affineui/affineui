// affineui::Ui — the "one type" facade. Composes Document + Renderer
// behind a friendlier API: lazy GPU init via Renderer, CSS-style
// click handler registration, file loading.
//
// Handler selector matching (minimal grammar — Phase 2D scope):
//   "#id"   → element's id attribute equals "id"
//   ".cls"  → element has "cls" in its class list
//   "tag"   → element's tag name equals "tag"
//   "a,b"   → any of a/b matches
//
// More expressive selectors (descendant combinators, attribute
// matches, pseudo-classes) ride on a future lexbor-selectors
// integration — the surface (`on_click(selector, cb)`) doesn't
// change.

#include "affineui/ui.h"

#include "affineui/log.h"
#include "core/embed_log.h"

#include <algorithm>
#include <string_view>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace affineui {

namespace {

bool matches_simple(std::string_view sel, const Document::HoverInfo& info) {
    if (sel.empty()) return false;
    if (sel.front() == '#') {
        return info.elem_id == sel.substr(1);
    }
    if (sel.front() == '.') {
        const auto cls = sel.substr(1);
        return std::find(info.classes.begin(), info.classes.end(), cls)
               != info.classes.end();
    }
    // Bare ident → tag name match.
    return info.tag == sel;
}

// Trim ASCII whitespace.
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' ||
                          s.back()  == '\n' || s.back()  == '\r')) s.remove_suffix(1);
    return s;
}

bool matches_selector_list(std::string_view selector_list,
                           const Document::HoverInfo& info) {
    // Comma-separated simple selectors. Any match → true.
    std::size_t i = 0;
    while (i < selector_list.size()) {
        const auto comma = selector_list.find(',', i);
        const auto piece = trim(selector_list.substr(
            i, (comma == std::string_view::npos ? selector_list.size() : comma) - i));
        if (matches_simple(piece, info)) return true;
        if (comma == std::string_view::npos) break;
        i = comma + 1;
    }
    return false;
}

}  // namespace

namespace detail {

struct UiImpl {
    Document document;
    Renderer renderer;

    // Advisory "needs repaint" flag (see Ui::needs_update). Starts true so
    // the first frame always paints; CSS animations are queried separately
    // so an on-demand host renders only while they are actually active.
    bool dirty{true};
    bool animations_active{false};
    std::uint64_t html_epoch{0};
    std::uint64_t render_epoch{0};

    // Click handlers, in insertion order. A single click can fire
    // multiple handlers if multiple selectors match (mirrors DOM
    // event bubbling intuitively at the registration site).
    std::vector<std::pair<std::string, std::function<void()>>> click_handlers;
    std::vector<Ui::EventHandler> event_handlers;
    std::vector<std::function<void(double)>> frame_callbacks;
    std::vector<Document::HoverInfo> hover_chain_scratch;
    bool pointer_captured{false};
};

// ── Internal log sink (embed_log.h) ─────────────────────────────────
namespace {
LogFn g_log_fn   = nullptr;
void* g_log_user = nullptr;
}  // namespace

void set_log_sink(LogFn fn, void* user) noexcept {
    g_log_fn   = fn;
    g_log_user = user;
}

// Exposed to the log facility (log.cpp) so the embedder's raw-pointer sink
// keeps firing alongside the new std::function handler.
LogFn legacy_log_fn() noexcept   { return g_log_fn; }
void* legacy_log_user() noexcept { return g_log_user; }

void log_msg(LogLevel level, const char* msg) noexcept {
    // Route through the public facility: default = console + affinetools,
    // frame-stamped; a host handler / legacy LogFn sink both honored there.
    // noexcept boundary: log() may allocate; swallow to keep callers safe.
    try {
        affineui::log(level, msg ? std::string_view(msg) : std::string_view{});
    } catch (...) {
    }
}

}  // namespace detail

Ui::Ui() : impl_(std::make_unique<detail::UiImpl>()) {}
Ui::~Ui() = default;
Ui::Ui(Ui&&) noexcept            = default;
Ui& Ui::operator=(Ui&&) noexcept = default;

// ── Content ─────────────────────────────────────────────────────────

void Ui::html(std::string_view source) {
    impl_->document.set_html(source);
    ++impl_->html_epoch;
    impl_->dirty = true;
    impl_->animations_active = false;
}

void Ui::css(std::string_view source) {
    impl_->document.set_user_stylesheet(source);
    impl_->dirty = true;
    impl_->animations_active = false;
}

bool Ui::load(std::string_view path) {
    const std::filesystem::path html_path{std::string(path)};
    std::ifstream f{html_path};
    if (!f.good()) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const auto base_dir = html_path.has_parent_path()
                              ? html_path.parent_path()
                              : std::filesystem::path{"."};
    impl_->document.set_resource_loader(
        [base_dir](std::string_view url) -> std::string {
            const std::string raw{url};
            if (raw.empty() || raw.find("://") != std::string::npos ||
                raw.rfind("data:", 0) == 0) {
                return {};
            }

            std::filesystem::path resource_path{raw};
            if (resource_path.is_relative()) {
                resource_path = base_dir / resource_path;
            }

            std::ifstream resource{resource_path, std::ios::binary};
            if (!resource.good()) return {};
            std::stringstream bytes;
            bytes << resource.rdbuf();
            return bytes.str();
        });
    html(ss.str());
    return true;
}

// ── Immediate mode ──────────────────────────────────────────────────

void Ui::mount(std::function<void()> view_fn) {
    impl_->document.set_imm_view(std::move(view_fn));
    impl_->dirty = true;
    impl_->animations_active = false;
}

void Ui::invalidate() {
    impl_->document.invalidate_imm();
    impl_->dirty = true;
    impl_->animations_active = false;
}

// ── Embedding (host-owned GPU) ───────────────────────────────────────

void Ui::init(const InitDesc& desc) {
    if (desc.log) {
        detail::set_log_sink(desc.log, desc.log_user);
    }
    if (desc.resource_loader) {
        impl_->document.set_resource_loader(desc.resource_loader);
    }
    // default_font_family / default_font_size are reserved for a future
    // font-config hook; the embedded default (Roboto) is registered when
    // the renderer brings NanoVG up.
    if (desc.gpu) {
        impl_->renderer.init_embedded(*desc.gpu, desc.allocator);
    }
    impl_->dirty = true;
    impl_->animations_active = false;
}

// ── Rendering ───────────────────────────────────────────────────────

void Ui::render(int fb_w, int fb_h, float dpi_scale) {
    // Run the imm view fn (if mounted + dirty) before paint. The
    // runtime mutates the DOM in place; we re-cascade + re-collect
    // inside tick_imm so layout/paint see the new tree.
    impl_->document.tick_imm();
    impl_->renderer.render(impl_->document, fb_w, fb_h, dpi_scale);
    ++impl_->render_epoch;
    impl_->animations_active = impl_->renderer.stats().animations_active;
    impl_->dirty = false;
}

void Ui::render(const FrameTarget& target) {
    impl_->document.tick_imm();
    impl_->renderer.render_to(impl_->document, target);
    ++impl_->render_epoch;
    impl_->animations_active = impl_->renderer.stats().animations_active;
    impl_->dirty = false;
}

// ── Update scheduling ───────────────────────────────────────────────

bool Ui::needs_update() const {
    return impl_->dirty || impl_->document.imm_dirty() ||
           impl_->animations_active;
}

void Ui::mark_dirty() {
    impl_->dirty = true;
    impl_->animations_active = false;
}

bool Ui::set_attr(std::string_view elem_id, std::string_view name,
                  std::string_view value) {
    const bool changed = impl_->document.set_attribute_by_id(elem_id, name, value);
    if (changed) {
        impl_->dirty = true;
        impl_->animations_active = false;
    }
    return changed;
}

bool Ui::remove_attr(std::string_view elem_id, std::string_view name) {
    const bool changed = impl_->document.remove_attribute_by_id(elem_id, name);
    if (changed) {
        impl_->dirty = true;
        impl_->animations_active = false;
    }
    return changed;
}

bool Ui::set_text(std::string_view elem_id, std::string_view text) {
    const bool changed = impl_->document.set_text_by_id(elem_id, text);
    if (changed) {
        impl_->dirty = true;
        impl_->animations_active = false;
    }
    return changed;
}

std::vector<Rect> Ui::take_dirty_rects() {
    return impl_->document.take_dirty_rects();
}

std::uint64_t Ui::html_epoch() const {
    return impl_->html_epoch;
}

std::uint64_t Ui::render_epoch() const {
    return impl_->render_epoch;
}

void Ui::reset() {
    impl_->document.set_imm_view({});         // drop imm view
    impl_->document.set_user_stylesheet("");  // clear user CSS
    impl_->document.set_html("");             // clear DOM
    impl_->click_handlers.clear();
    impl_->event_handlers.clear();
    impl_->frame_callbacks.clear();
    impl_->hover_chain_scratch.clear();
    impl_->pointer_captured = false;
    impl_->dirty = true;
    impl_->animations_active = false;
    // TODO(embed §7): also release cached GPU resources + the asset cache
    // once those subsystems exist.
}

// ── Input ───────────────────────────────────────────────────────────

bool Ui::dispatch(const Event& e) {
    if (impl_->pointer_captured && e.type == EventType::MouseMove &&
        !impl_->event_handlers.empty()) {
        impl_->document.hovered_info_chain(impl_->hover_chain_scratch);
        bool consumed = false;
        for (const auto& cb : impl_->event_handlers) {
            consumed = cb(e, impl_->hover_chain_scratch) || consumed;
        }
        if (consumed) return true;
    }

    if (e.type == EventType::Resize) {
        impl_->dirty = true;
    }
    const auto result = impl_->document.dispatch(e);
    if (result.redraw_requested || result.invalidate_view) {
        impl_->dirty = true;  // a hover/focus/state change needs a repaint
    }

    const bool mouse_up_left =
        e.type == EventType::MouseUp && e.button == MouseButton::Left;
    const bool needs_chain =
        !impl_->event_handlers.empty() || mouse_up_left;
    if (needs_chain) {
        impl_->document.hovered_info_chain(impl_->hover_chain_scratch);
    } else {
        impl_->hover_chain_scratch.clear();
    }
    const auto& chain = impl_->hover_chain_scratch;
    bool event_consumed = false;
    for (const auto& cb : impl_->event_handlers) {
        event_consumed = cb(e, chain) || event_consumed;
    }
    if (event_consumed) return true;

    // Click routing: on MouseUp, check (1) user-registered selectors
    // via on_click across the hovered ancestor chain, then (2) imm-mode
    // handlers for elements stamped with `aui-imm-{hash}` ids. The
    // user-handler path runs first so explicitly-bound handlers (e.g.
    // retained-mode UI atop an imm-mode island) override.
    if (mouse_up_left) {
        if (!chain.empty()) {
            bool consumed = false;
            for (const auto& [selector, cb] : impl_->click_handlers) {
                const bool matched = std::any_of(
                    chain.begin(), chain.end(),
                    [&](const Document::HoverInfo& info) {
                        return matches_selector_list(selector, info);
                    });
                if (matched) {
                    cb();
                    consumed = true;
                }
            }
            if (consumed) return true;
            // imm-mode handler hit?
            for (const auto& info : chain) {
                if (impl_->document.invoke_imm_click(info.elem_id)) {
                    return true;
                }
            }
        }
    }
    return false;
}

int Ui::hovered_cursor() const {
    return impl_->document.hovered_cursor();
}

bool Ui::text_input_active() const {
    return impl_->document.text_input_active();
}

Rect Ui::caret_rect() const {
    return impl_->document.caret_rect();
}

std::vector<Document::HoverInfo> Ui::hovered_info_chain() const {
    return impl_->document.hovered_info_chain();
}

// ── Handlers ────────────────────────────────────────────────────────

void Ui::capture_pointer() {
    impl_->pointer_captured = true;
}

void Ui::release_pointer() {
    impl_->pointer_captured = false;
}

bool Ui::pointer_captured() const {
    return impl_->pointer_captured;
}

void Ui::on_click(std::string_view selector, std::function<void()> cb) {
    impl_->click_handlers.emplace_back(std::string(selector), std::move(cb));
}

void Ui::on_event(EventHandler cb) {
    impl_->event_handlers.emplace_back(std::move(cb));
}

void Ui::on_frame(std::function<void(double)> cb) {
    impl_->frame_callbacks.emplace_back(std::move(cb));
}

void Ui::run_frame_callbacks(double dt_seconds) {
    for (const auto& cb : impl_->frame_callbacks) {
        cb(dt_seconds);
    }
}

// ── Config ──────────────────────────────────────────────────────────

void  Ui::set_clear_color(Color c)  { impl_->renderer.set_clear_color(c); impl_->dirty = true; }
Color Ui::clear_color() const       { return impl_->renderer.clear_color(); }

// ── Access ──────────────────────────────────────────────────────────

Document&       Ui::document()       { return impl_->document; }
const Document& Ui::document() const { return impl_->document; }
Renderer&       Ui::renderer()       { return impl_->renderer; }
const Renderer& Ui::renderer() const { return impl_->renderer; }

Size Ui::content_size() const { return impl_->document.content_size(); }

}  // namespace affineui
