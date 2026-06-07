#pragma once

// ── DCC template: gestures + tools ──────────────────────────────────────────
//
// EXAMPLE / TEMPLATE CODE. Turns raw pointer events into higher-level gestures
// (press / click / drag with a movement threshold) and routes them to the
// active Tool. This is the minimal spine: enough to tell a click from a drag
// and to give tools begin/update/end of a drag. Extend with more gesture kinds
// (double-click, wheel, multi-button) as an app needs.

#include "affineui/types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace app {

class Context;

/// A drag in progress, in widget-local coordinates.
struct Drag {
    affineui::Point start{};
    affineui::Point current{};
    affineui::Point delta{};   // current - start
    bool ctrl{false};
    bool shift{false};
    bool alt{false};
};

/// A tool handles gestures within the work area (select, move, brush, …). The
/// base no-ops, so a tool overrides only what it cares about. Tools are owned
/// by the app and are typically long-lived.
class Tool {
public:
    virtual ~Tool() = default;
    [[nodiscard]] virtual std::string_view id() const = 0;

    virtual void on_click(Context&, affineui::Point) {}
    virtual void on_drag_begin(Context&, const Drag&) {}
    virtual void on_drag_update(Context&, const Drag&) {}
    virtual void on_drag_end(Context&, const Drag&) {}
};

/// Feeds pointer events to the active tool, classifying press→move→release into
/// click vs drag using a pixel threshold.
class GestureRouter {
public:
    explicit GestureRouter(Context& ctx) : ctx_(ctx) {}

    void set_tool(std::shared_ptr<Tool> tool) { tool_ = std::move(tool); }
    [[nodiscard]] std::string_view tool_id() const {
        return tool_ ? tool_->id() : std::string_view{};
    }

    /// Feed a translated pointer event. Returns true if a gesture consumed it.
    bool on_pointer(const affineui::Event& ev);

    void set_drag_threshold(int px) { threshold_ = px; }

private:
    Context&              ctx_;
    std::shared_ptr<Tool> tool_;
    bool                  pressed_{false};
    bool                  dragging_{false};
    int                   threshold_{4};
    Drag                  drag_{};
};

}  // namespace app
