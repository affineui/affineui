#pragma once

#include "affineui/types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace affineui {

class App;
class Painter;

/// Optional C++ behavior scripts that can be attached to a Document.
/// These are intentionally separate from the renderer: raw HTML/CSS
/// documents can remain paint-only, while command-tree/framework demos can
/// opt into browser-like control behavior.
enum class DocumentScript {
    /// Stock behavior for native form controls and AffineUI framework
    /// widgets emitted as HTML/CSS, such as Decius sliders/faders/knobs.
    UiControls,
};

namespace detail {
struct DocumentImpl;
}

/// A parsed HTML document with its associated CSS, layout, and event state.
/// Owned by an App, but can also be used headless for layout / testing.
class Document {
public:
    struct WidgetChange {
        std::string name;
        std::string value;
    };

    Document();
    ~Document();

    Document(const Document&)            = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;

    /// Parse and replace the document body. Previous DOM is discarded.
    void set_html(std::string_view html);

    /// Inject (or replace) the user stylesheet. Applied on top of the
    /// document's own <style> blocks and <link> imports.
    void set_user_stylesheet(std::string_view css);

    /// Reapply stylesheets without re-parsing the DOM. Cheap; intended
    /// for hot-reload workflows.
    void reload_stylesheets();

    /// Trigger a layout pass against the given viewport width. Called
    /// automatically on resize and after `set_html`; exposed for tests.
    ///
    /// `measurer` is consulted for per-font text metrics (used to size
    /// text-bearing leaves precisely). Pass nullptr for tests or
    /// headless cases — layout falls back to a conservative `font_size`
    /// estimate that may produce vertically-asymmetric padding.
    /// `viewport_height` extends content_size so the body background
    /// fills the visible viewport even when natural page content is
    /// shorter than the window. Pass 0 to disable the floor.
    void layout(int viewport_width, int viewport_height = 0,
                Painter* measurer = nullptr);

    /// Paint the current document into `painter`. `painter` is whatever
    /// the embedder supplies — typically an internal NanoVG-backed one.
    void draw(Painter& painter);

    /// Route an OS / app event through litehtml. Returns whether a
    /// redraw and/or imm-view re-evaluation is needed.
    DispatchResult dispatch(const Event& ev);

    /// Drain named widget activations produced by attached behavior scripts.
    /// Names are stable `data-aui-name` values when present, otherwise `id`.
    std::vector<std::string> take_activated_widgets();

    /// Drain named value changes produced by attached behavior scripts.
    /// Values are serialized strings so language bindings and remote
    /// transports can forward them without ABI-specific variant machinery.
    std::vector<WidgetChange> take_widget_changes();

    /// Attach/detach optional C++ behavior scripts. This is the native
    /// equivalent of including a page script: without it, Document remains a
    /// renderer plus CSS pseudo-state engine; with it, stock widgets mutate
    /// themselves in response to pointer input.
    void attach_script(DocumentScript script);
    void detach_script(DocumentScript script);
    void clear_scripts();

    /// Embedder-supplied resource loader for `<img src=...>`,
    /// `<link rel=stylesheet href=...>`, etc.
    void set_resource_loader(ResourceLoader loader);

    /// Clipboard bridge used by editor keyboard commands. Hosts that do
    /// not provide hooks still get deterministic in-document copy/paste
    /// through a small fallback clipboard, which keeps tests and embedded
    /// render-only use working without platform dependencies.
    using ClipboardGet = std::function<std::string()>;
    using ClipboardSet = std::function<void(std::string_view)>;
    void set_clipboard(ClipboardGet get, ClipboardSet set);

    /// Current document size after the last layout pass.
    Size content_size() const;

    /// Mutate a live DOM attribute on the element with `id`.
    /// Returns true only when the attribute value actually changed. A
    /// successful mutation marks the affected subtree dirty while preserving
    /// the parsed DOM and stylesheet state.
    bool set_attribute_by_id(std::string_view elem_id,
                             std::string_view name,
                             std::string_view value);

    /// Remove a live DOM attribute on the element with `id`.
    bool remove_attribute_by_id(std::string_view elem_id,
                                std::string_view name);

    /// Replace textContent for a leaf element with `id`.
    bool set_text_by_id(std::string_view elem_id, std::string_view text);

    /// Drain dirty document rectangles accumulated by live mutations.
    std::vector<Rect> take_dirty_rects();

    /// Drain the document-wide paint dirty bit. Most mutations report
    /// precise dirty rects; this covers full-document/style invalidations
    /// where the retained renderer must rebuild its display list.
    bool take_paint_dirty();

    /// True while CSS animations in the document need another frame.
    bool has_active_animations() const;

    /// Weak handles for command-tree/native DOM reconciliation. Handles do
    /// not extend lifetime and become invalid when the document is replaced or
    /// when Lexbor removes/destroys the referenced node.
    [[nodiscard]] DomHandle weak_handle_for_id(std::string_view elem_id);
    [[nodiscard]] bool weak_handle_valid(DomHandle handle) const;

    /// Set the document animation clock to a deterministic elapsed time.
    /// Intended for conformance/test harnesses that compare exact animation
    /// phases against a browser.
    void set_animation_time_for_testing(double seconds);

    /// Identity of the currently-hovered element, for click routing.
    /// `valid` is false when nothing is hovered. `tag` / `elem_id` /
    /// `classes` are populated from the element's HTML attributes —
    /// enough for the minimal selector grammar in Ui::on_click.
    struct HoverInfo {
        bool                     valid{false};
        std::string              tag;
        std::string              elem_id;
        std::vector<std::string> classes;
        std::vector<std::pair<std::string, std::string>> attrs;
        Rect                     bounds{};
    };
    HoverInfo hovered_info() const;

    /// Hover identity chain for event routing, ordered from the
    /// deepest hovered element to the document root. This lets Ui
    /// implement DOM-like click bubbling while keeping the simple
    /// selector grammar on Ui::on_click.
    std::vector<HoverInfo> hovered_info_chain() const;
    void hovered_info_chain(std::vector<HoverInfo>& out) const;

    // ── Immediate-mode view (Phase 2D — "clear and rebuild") ────────

    /// Install an imm-mode view function. The function will be called
    /// when the document is dirty (state mutation or explicit
    /// invalidate_imm); it should produce the desired UI via the
    /// affineui::imm API.
    void set_imm_view(std::function<void()> view_fn);

    /// True when the imm view should be re-run before the next paint.
    /// Set by state mutations + the explicit `imm::invalidate()` call.
    bool imm_dirty() const;

    /// Mark the imm view dirty. Next tick_imm() will re-run the view
    /// function.
    void invalidate_imm();

    /// If dirty, clear the document body, run the view function, then
    /// re-cascade + re-collect blocks so paint sees the new DOM.
    /// No-op when there is no view function or nothing is dirty.
    void tick_imm();

    /// Invoke an imm-mode click handler matching `elem_id` (the form
    /// `aui-imm-{hash}` auto-assigned during view-fn rebuilds), if one
    /// is registered. Returns true on hit. Ui::dispatch consults this
    /// after its own selector-based handlers when MouseUp lands on
    /// an element with such an id.
    bool invoke_imm_click(std::string_view elem_id);

    /// Cursor enum that the OS should display under the current
    /// hovered element. Returned as `int` to avoid exporting the
    /// internal `detail::ComputedStyle::Cursor` enum through the
    /// public header. Map onto your platform's cursor API on the
    /// host side (App does this for sokol_app).
    ///   0 = default, 1 = pointer, 2 = text, 3 = crosshair, 4 = move,
    ///   5 = not-allowed, 6 = ew-resize, 7 = ns-resize,
    ///   8 = nwse-resize
    int hovered_cursor() const;

private:
    std::unique_ptr<detail::DocumentImpl> impl_;
    friend class App;
};

}  // namespace affineui
