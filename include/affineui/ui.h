#pragma once

// affineui::Ui — the "one type" facade game engineers actually use.
//
// Composes a Document (parsed HTML/CSS + layout + paint output) and a
// Renderer (graphics-API-specific drawing). Adds a friendly surface:
// CSS-selector click handlers, lazy GPU init, a single `render(w, h,
// dpi)` call. Designed to be IDE-discoverable without reading docs.
//
// Typical use through a windowing-toolkit adapter:
//
//     #define AFFINEUI_WITH_SOKOL
//     #include <affineui/affineui.h>
//
//     affineui::Ui ui;
//     ui.html("<button id='quit'>Quit</button>");
//     ui.on_click("#quit", []{ sapp_request_quit(); });
//
//     sapp_desc d{};
//     affineui::sokol::wire(d, ui);
//     sapp_run(&d);
//
// Threading: single-threaded. All methods must be called on the same
// thread that owns your graphics context.

#include "affineui/document.h"
#include "affineui/embed.h"
#include "affineui/renderer.h"
#include "affineui/types.h"

#include <functional>
#include <memory>
#include <cstdint>
#include <string_view>
#include <vector>

namespace affineui {

namespace detail { struct UiImpl; }

class Ui {
public:
    Ui();
    ~Ui();

    Ui(const Ui&)            = delete;
    Ui& operator=(const Ui&) = delete;
    Ui(Ui&&) noexcept;
    Ui& operator=(Ui&&) noexcept;

    // ── Content ─────────────────────────────────────────────────────

    /// Replace the document HTML. Re-parses, re-cascades, re-lays-out
    /// on the next render(). Cheap to call infrequently.
    void html(std::string_view source);

    /// Set the user stylesheet — sits above the document's <style>
    /// blocks in the cascade. Call this once at setup for app theming.
    void css(std::string_view source);

    /// Load HTML from a file. Returns true on success. Linked local
    /// resources such as `<link rel="stylesheet" href="...">` resolve
    /// relative to the HTML file's directory.
    bool load(std::string_view path);

    // ── Embedding (host-owned GPU) ──────────────────────────────────

    /// Initialize for embedded mode against the host's graphics objects.
    /// Pass an InitDesc whose `gpu` points at a complete GpuContext for
    /// your backend; AffineUI brings up sokol_gfx on those objects (it
    /// does NOT create a window or swapchain). Call once before the first
    /// render(FrameTarget). With `gpu == nullptr` this only applies the
    /// non-GPU settings (fonts, resource loader) and leaves GPU init to
    /// the lazy path used by the windowing adapters.
    void init(const InitDesc& desc);

    // ── Rendering ───────────────────────────────────────────────────

    /// Render one frame of the document into the current framebuffer.
    /// You supply the framebuffer dimensions in pixels and the DPI
    /// scale (pixels-per-point — 1.0 standard, 2.0 Retina). Call this
    /// once per game-frame from inside your render pass.
    ///
    /// First call lazily initializes GPU resources against the
    /// currently bound graphics context — no explicit init needed.
    /// Use this from the sokol_app / SDL adapters, which open the pass
    /// for you.
    void render(int fb_w, int fb_h, float dpi_scale);

    /// Render one frame into host-provided render-target views (embedded
    /// mode). AffineUI opens its own sokol_gfx pass into `target`, draws
    /// the document, and ends+commits — it does NOT present. The host
    /// owns the views and the flip. Requires a prior init() with a
    /// GpuContext. See the state contract in embed.h.
    void render(const FrameTarget& target);

    // ── Update scheduling ───────────────────────────────────────────

    /// Whether the UI changed since the last render and should be
    /// repainted. A host that renders on demand can skip render() when
    /// this is false. CSS animations keep this true while they are active.
    /// (Advisory; render() is always safe to call.)
    bool needs_update() const;

    /// Force needs_update() true — call when host-side state the UI
    /// depends on changed outside AffineUI's knowledge.
    void mark_dirty();

    /// Live DOM mutation helpers. These are the retained-mode analog of
    /// small JS state updates: mutate one element, invalidate only the
    /// affected subtree, and keep the parsed document intact. Return true
    /// only when the document actually changed.
    bool set_attr(std::string_view elem_id, std::string_view name,
                  std::string_view value);
    bool remove_attr(std::string_view elem_id, std::string_view name);
    bool set_text(std::string_view elem_id, std::string_view text);

    /// Drain dirty document rectangles reported by live mutations.
    std::vector<Rect> take_dirty_rects();

    /// Monotonic counters useful for perf instrumentation. `html_epoch`
    /// increments on every retained DOM replacement; `render_epoch`
    /// increments after every document render pass.
    std::uint64_t html_epoch() const;
    std::uint64_t render_epoch() const;

    /// Drop all content state (DOM, user CSS, click handlers, imm view)
    /// and return to a clean, reusable state, keeping the GPU/device
    /// binding so the Ui can be refilled without re-init. TODO: also
    /// release cached GPU resources / asset cache once those exist.
    void reset();

    // ── Immediate mode ──────────────────────────────────────────────

    /// Install an immediate-mode view function. The function is
    /// called on the next render (and whenever state mutates / a click
    /// handler runs / `invalidate()` is called). Inside it, call
    /// affineui::imm::div(), button(), use_state(), etc. to describe
    /// the desired UI. The Document mutates its DOM in place to
    /// match.
    void mount(std::function<void()> view_fn);

    /// Mark the imm view as needing re-evaluation before the next
    /// render. Safe to call from anywhere on the same thread; useful
    /// when external state outside an imm `use_state` slot has
    /// changed.
    void invalidate();

    // ── Input ───────────────────────────────────────────────────────

    /// Forward a translated event to the document. Returns whether
    /// the UI "consumed" the event (e.g. a click hit a registered
    /// handler). If true, your game should suppress its own handling.
    bool dispatch(const Event& e);

    /// Current hovered element chain, deepest first. Native widgets use
    /// this to implement browser-like pointer capture against their own
    /// state while still letting AffineUI own hit-testing and CSS state.
    std::vector<Document::HoverInfo> hovered_info_chain() const;

    /// Capture pointer moves for a native event handler. While captured,
    /// MouseMove is offered to low-level handlers before DOM hover hit
    /// testing, so drag controls can update without thrashing unrelated
    /// :hover state. MouseDown/MouseUp still flow through the document.
    void capture_pointer();
    void release_pointer();
    bool pointer_captured() const;

    /// Cursor the OS should display under the last hovered position.
    /// Maps onto your windowing toolkit's cursor enum.
    ///   0 = default, 1 = pointer, 2 = text, 3 = crosshair, 4 = move,
    ///   5 = not-allowed, 6 = ew-resize, 7 = ns-resize,
    ///   8 = nwse-resize
    int hovered_cursor() const;

    // ── Handlers ────────────────────────────────────────────────────

    /// Register a click callback for elements matching a CSS-style
    /// selector. Phase 2D supports a minimal selector grammar:
    ///   "#id"   — element with matching id attribute
    ///   ".cls"  — element with matching class
    ///   "tag"   — element with matching tag name
    ///   "a,b"   — comma-separated list (any matches)
    /// Compound selectors (`div.card`) and combinators are Phase 3.
    void on_click(std::string_view selector, std::function<void()> cb);

    using EventHandler = std::function<bool(
        const Event&, const std::vector<Document::HoverInfo>&)>;

    /// Register a low-level native event handler. The handler receives
    /// the already-hit-tested hover chain, deepest first, after CSS
    /// hover/active state has been updated. This is the C++ analog of
    /// the small Decius JS behavior layer: controls can capture pointer
    /// drags and mutate native state without reparsing event targets.
    void on_event(EventHandler cb);

    // ── Configuration ──────────────────────────────────────────────

    /// Background color drawn behind the document. Default catppuccin
    /// Base (#1e1e2e).
    void  set_clear_color(Color c);
    Color clear_color() const;

    // ── Escape hatches (advanced) ──────────────────────────────────

    Document&       document();
    const Document& document() const;
    Renderer&       renderer();
    const Renderer& renderer() const;

    /// Current document content size after the last layout pass.
    Size content_size() const;

private:
    std::unique_ptr<detail::UiImpl> impl_;
};

}  // namespace affineui
