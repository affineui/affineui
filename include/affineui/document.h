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
class ViewSink;  // view.h (View reconciliation sink; view.h includes us)

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
class ImmRuntime;
}

/// A parsed HTML document with its associated CSS, layout, and event state.
/// Owned by an App, but can also be used headless for layout / testing.
class Document {
public:
    struct WidgetChange {
        std::string name;
        std::string value;
        /// True while a continuous gesture (drag-scrub of a combo,
        /// slider, fader, knob, colour picker) is still in flight; the
        /// gesture's end emits one final change with live == false.
        /// Discrete controls (checkbox, menu, typed commit) only emit
        /// committed (live == false) changes.
        bool        live{false};
    };

    /// A runtime override of a dockable panel's placement, produced by
    /// drag-to-dock / tearoff interactions and read back by the app so the next
    /// resolve_dock re-seeds from it (the same "interaction → persist → re-seed"
    /// rail as dock_pane_sizes, but for *structure* rather than just size).
    /// `side` uses the View's Dock order: 0=Left,1=Right,2=Top,3=Bottom,4=Tab.
    struct DockPlacement {
        bool        present{false};   ///< false = no override for this panel
        bool        floating{false};  ///< true = torn off into a floating panel
        std::string parent;           ///< docked: target pane id ("" = document)
        int         side{0};          ///< docked: Dock side
        int         size{0};          ///< docked: px flex-basis (0 = default)
        int         x{0}, y{0}, w{0}, h{0};  ///< floating: rect in float-host px
    };

    /// The CURRENT dock arrangement, read live from the DOM (the dock structure
    /// IS the DOM — decius.js semantics). The View's document_view() replays
    /// this on re-emit so drag-to-dock / tearoff surgery survives view reloads;
    /// apps can serialize it for workspace persistence. `flex` strings are the
    /// inline `flex:` value of each child in its parent split ("1 1 240px",
    /// "0 0 320px", "1", or empty for default).
    struct DockLayout {
        struct Node {
            bool                     split{false};
            bool                     vertical{false};  ///< split orientation
            std::string              flex;             ///< slot flex in parent
            std::vector<Node>        children;         ///< split children
            // Leaf (a dockpane):
            std::string              kind;     ///< "panels" / "documents"
            std::vector<std::string> tabs;     ///< panel ids, in tab order
            std::string              active;   ///< active panel id ("" = first)
            // Dock-graph placement, round-tripped so a replay re-emits the
            // same data-aui-dock-parent/side the live pane carried (without
            // this, one replay resets every pane's graph to document:left).
            std::string              dock_parent;   ///< "" = the document
            int                      dock_side{-1}; ///< Dock enum value; -1 = none
        };
        struct Float {
            int  x{0}, y{0}, w{0}, h{0};  ///< rect in workspace-root px
            // x/y measure inward from the RIGHT/BOTTOM edge when set — a
            // corner-anchored seed positions with right:/bottom: CSS, and the
            // harvest keeps that form so the replay needs no layout pass.
            bool from_right{false};
            bool from_bottom{false};
            bool title_only{false};       ///< single-tab title-bar mode
            Node pane;                    ///< the floating dockpane (leaf)
        };
        bool               present{false};  ///< false = no dock in the DOM
        Node               root;            ///< the workspace split tree
        std::vector<Float> floats;
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

    /// As above, but `base_url` is the stylesheet's own location (e.g. the
    /// directory a framework bundle was loaded from). url()s inside the sheet —
    /// @font-face src, background images — resolve relative to it, exactly as
    /// for a <link href=...>ed sheet. Without it, the sheet's relative url()s
    /// have no base and will not resolve.
    void set_user_stylesheet(std::string_view css, std::string_view base_url);

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

    /// The current fixed pixel size of every dock pane that has one, keyed by
    /// the pane id (the dockpanel key). Reads the live flex-basis, so it
    /// reflects splitter drags — the app persists this to restore the layout.
    /// The flexible center/document pane (no fixed basis) is omitted.
    [[nodiscard]] std::vector<std::pair<std::string, int>> dock_pane_sizes() const;

    /// Runtime dock-placement override for a panel (empty `present=false` if the
    /// panel has not been moved/torn-off this session). Set by drag-to-dock and
    /// tearoff interactions; the app wires this into the View's dock-placement
    /// provider so a rebuild re-seeds the moved/floating layout.
    [[nodiscard]] DockPlacement dock_override(std::string_view panel_id) const;

    /// All accumulated dock-placement overrides, for the app to persist.
    [[nodiscard]] std::vector<std::pair<std::string, DockPlacement>>
    dock_overrides() const;

    /// Forget every runtime dock override and remembered active tab (a
    /// "Reset workspace" action). The app should then rebuild WITHOUT wiring
    /// the dock-layout/placement providers for that one pass, so the declared
    /// seed layout wins over the live DOM arrangement.
    void reset_dock_state();

    /// The current dock arrangement, read live from the DOM (see DockLayout).
    /// Wire into View::set_dock_layout_provider so view rebuilds re-emit the
    /// user's arrangement instead of the declared seed.
    [[nodiscard]] DockLayout dock_layout() const;

    /// True once (consumes the flag) after dock SURGERY — tearoff,
    /// drag-to-dock, tab move — restructured the DOM outside a view batch.
    /// A retained View must not reconcile incrementally over that (the
    /// surgery's wrapper elements would survive as duplicate chrome):
    /// App::rebuild_view re-bootstraps when this fires.
    [[nodiscard]] bool take_dock_structure_changed();

    /// Border-box rect of the first element matching `target`, in document
    /// coordinates (valid after layout). Target forms:
    ///   "#id"            — by id attribute
    ///   "[name=value]"   — by attribute equality (e.g. "[data-dcs-target=#x]")
    ///   "name"           — by data-aui-name (the View widget key)
    /// Returns w<=0 when not found / not laid out. Drives the automation
    /// scripting layer (affineui/automation.h) and programmatic scroll/anchor
    /// use cases.
    [[nodiscard]] Rect find_element_rect(std::string_view target) const;

    /// Active tab for a dock leaf, keyed by the leaf/pane id. Empty means the
    /// primary panel is active. Updated by dock-tab clicks so View can rebuild
    /// inactive tab bodies as empty placeholders and create content on show.
    [[nodiscard]] std::string dock_active_tab(std::string_view pane_id) const;

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

    /// Programmatically set the VALUE a named widget displays (the
    /// element whose data-aui-name is `name`): a dcs-combo / slider /
    /// fader / knob updates its input text, fill and value attributes
    /// through the same path an interactive scrub uses, other controls
    /// get their value attribute set. Emits NO widget-change event —
    /// this is the write-back half of a data binding (e.g. an inspector
    /// tracking a 3D gizmo drag), not user input. Returns true if a
    /// widget matched and changed.
    bool set_widget_value(std::string_view name, std::string_view value);

    // ── View reconciliation (App fast path) ─────────────────────────
    /// Begin a batched View-reconcile mutation pass and return the
    /// document-backed ViewSink. Attribute/text mutations flow through
    /// the same live-mutation classification as set_attribute_by_id
    /// (paint-only where possible); structural mutations are applied
    /// with lexbor's eager style attach suppressed and settled by ONE
    /// restyle + box recollect in end_view_mutations(). Pair every
    /// begin with an end. Returns null when no document is loaded.
    ViewSink* begin_view_mutations();
    void end_view_mutations();

    // ── Custom paint (canvas) ────────────────────────────────────────
    /// App-drawn regions inside the retained document — the HTML
    /// <canvas> idea. An element opts in with `data-aui-paint="name"`;
    /// after its background/borders (and any inline-svg children) paint,
    /// the handler registered under `name` is invoked with the active
    /// Painter and the element's border-box rect in document space. The
    /// handler draws immediate-mode vector content (fill_path /
    /// stroke_path / text / …) — floats end to end, no DOM, no strings —
    /// which the retained renderer records, hashes, and composites like
    /// any other paint. Per-frame animated geometry (patch cables,
    /// meters, scopes) belongs here; retained SVG stays for static
    /// declarative art.
    ///
    /// The handler must not mutate the Document. An empty fn removes the
    /// registration.
    using CustomPaintFn = std::function<void(Painter&, const Rect&)>;
    void set_custom_paint(std::string_view name, CustomPaintFn fn);

    /// Mark every `data-aui-paint="name"` element's rect dirty so the
    /// next frame repaints it (geometry-only change: no restyle, no
    /// layout, no reconcile). Returns true if any element matched.
    bool request_custom_repaint(std::string_view name);

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
    struct TransientState {
        struct Layer {
            std::string id;
            std::string style;
            std::string base_style;
            std::string target_selector;
            bool        popover{false};
            bool        menu{false};
        };
        std::vector<Layer> open_layers;
    };

    [[nodiscard]] TransientState capture_transient_state() const;
    void restore_transient_state(const TransientState& state);

    std::unique_ptr<detail::DocumentImpl> impl_;
    friend class App;
    friend class detail::ImmRuntime;
    // Devtools read pump (tools.h): services queued dom/css/resource
    // read commands at the frame boundary; read-only-never-relayout.
    friend void tools_pump(Document& doc);
};

}  // namespace affineui
