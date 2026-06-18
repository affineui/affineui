#pragma once

// Scripted UI automation — drive a Document with named-element input and
// validate the results, no human tester required.
//
// A UiScript wraps a Document and replays an input script against it:
// clicks, cursor moves, drags and key presses target elements by name
// ("#id", "[attr=value]", or a View widget key), every dispatched event is
// followed by a relayout, and an optional step hook lets the script emulate
// the host app's reload loop (the game editor rebuilds its view after every
// layout change — scripts that don't model that miss the bugs that only
// happen across reloads). Trace lines (the dock trace) are captured for
// assertions, and validate_dock_layout() checks the structural invariants
// every dock arrangement must satisfy.
//
//   affineui::UiScript ui(doc, 640, 360, &painter);
//   ui.set_step_hook([&](const DispatchResult& r) {
//       if (r.layout_changed) rebuild();   // emulate the app
//   });
//   ui.drag("[data-dcs-target=#Console-body]", "pane-Hierarchy",
//           UiScript::Anchor::Bottom);
//   CHECK(ui.log_contains("edge-dock panel=Console"));
//   CHECK(UiScript::validate_dock_layout(doc.dock_layout(),
//                                        {"Console", "Hierarchy"}).empty());
//
// Screenshot diffing rides on the conformance harness (conformance/run.py);
// scripts expose deterministic checkpoints via the trace log.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "affineui/document.h"
#include "affineui/types.h"

namespace affineui {

namespace detail {
// Installed by UiScript to record dock-trace lines (document.cpp bridge).
void set_dock_trace_capture(std::function<void(const std::string&)> fn);
}  // namespace detail

class UiScript {
public:
    /// Where inside a target rect a pointer op lands.
    enum class Anchor {
        Center,
        Left,    ///< mid-left edge band (dock-edge drops)
        Right,
        Top,
        Bottom,
        TopLeft  ///< near the top-left corner (e.g. a tab's label area)
    };

    /// `measurer` is the painter used for relayout after each event (tests
    /// pass their RecordingPainter; nullptr falls back to estimates).
    UiScript(Document& doc, int viewport_w, int viewport_h,
             Painter* measurer = nullptr);
    ~UiScript();

    UiScript(const UiScript&) = delete;
    UiScript& operator=(const UiScript&) = delete;

    /// Called after every dispatched event with its result — emulate the host
    /// app here (rebuild the view on layout_changed, etc.).
    void set_step_hook(std::function<void(const DispatchResult&)> hook);

    /// Resolve a target to a point. Returns {-1,-1} when the element is
    /// missing or not laid out (assert on it in the script).
    [[nodiscard]] Point point_of(std::string_view target,
                                 Anchor anchor = Anchor::Center);

    // ── pointer ──────────────────────────────────────────────────────────
    bool move_to(Point p);
    bool move_to(std::string_view target, Anchor anchor = Anchor::Center);
    bool mouse_down(MouseButton button = MouseButton::Left);
    bool mouse_up(MouseButton button = MouseButton::Left);
    /// Press + release on the target (selects/buttons/tabs).
    bool click(std::string_view target, Anchor anchor = Anchor::Center);
    /// Press on `from`, glide in `steps` intermediate moves, release on `to`.
    /// This is the docking gesture: the intermediate moves cross the drag
    /// threshold and exercise the live preview path.
    bool drag(std::string_view from, std::string_view to,
              Anchor to_anchor = Anchor::Center, int steps = 6);
    bool drag_to(Point to, int steps = 6);

    // ── keyboard ─────────────────────────────────────────────────────────
    bool key(Key k, bool shift = false, bool ctrl = false, bool alt = false);
    bool type_text(std::string_view text);

    // ── validation ───────────────────────────────────────────────────────
    /// Trace lines captured since construction / the last clear_log().
    [[nodiscard]] const std::vector<std::string>& log() const { return log_; }
    [[nodiscard]] bool log_contains(std::string_view needle) const;
    void clear_log() { log_.clear(); }

    /// Structural invariants every dock arrangement must satisfy. Returns
    /// human-readable violations (empty = valid):
    ///   • every expected panel appears exactly once (tree or floats)
    ///   • no empty panes or single-child/empty splits in the tree
    ///   • every leaf's active tab is one of its tabs
    ///   • floats have positive sizes
    [[nodiscard]] static std::vector<std::string> validate_dock_layout(
        const Document::DockLayout& layout,
        const std::vector<std::string>& expected_panels);

private:
    bool dispatch(const Event& ev);

    Document&  doc_;
    int        w_;
    int        h_;
    Painter*   measurer_;
    Point      cursor_{0, 0};
    std::function<void(const DispatchResult&)> hook_;
    std::vector<std::string> log_;
};

}  // namespace affineui
