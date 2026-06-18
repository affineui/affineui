#include "affineui/automation.h"

#include <algorithm>
#include <set>

namespace affineui {

UiScript::UiScript(Document& doc, int viewport_w, int viewport_h,
                   Painter* measurer)
    : doc_(doc), w_(viewport_w), h_(viewport_h), measurer_(measurer) {
    detail::set_dock_trace_capture(
        [this](const std::string& line) { log_.push_back(line); });
}

UiScript::~UiScript() { detail::set_dock_trace_capture(nullptr); }

void UiScript::set_step_hook(std::function<void(const DispatchResult&)> hook) {
    hook_ = std::move(hook);
}

Point UiScript::point_of(std::string_view target, Anchor anchor) {
    doc_.layout(w_, h_, measurer_);
    const Rect r = doc_.find_element_rect(target);
    if (r.w <= 0 || r.h <= 0) return {-1, -1};
    // Edge anchors land INSIDE the rect's edge band (a few px in), so dock
    // edge-zone math (outer 22%) sees them without falling off the element.
    const int inset_x = std::max(2, r.w / 10);
    const int inset_y = std::max(2, r.h / 10);
    switch (anchor) {
        case Anchor::Center:  return {r.x + r.w / 2, r.y + r.h / 2};
        case Anchor::Left:    return {r.x + inset_x, r.y + r.h / 2};
        case Anchor::Right:   return {r.x + r.w - inset_x, r.y + r.h / 2};
        case Anchor::Top:     return {r.x + r.w / 2, r.y + inset_y};
        case Anchor::Bottom:  return {r.x + r.w / 2, r.y + r.h - inset_y};
        case Anchor::TopLeft: return {r.x + inset_x, r.y + inset_y};
    }
    return {r.x + r.w / 2, r.y + r.h / 2};
}

bool UiScript::dispatch(const Event& ev) {
    const auto result = doc_.dispatch(ev);
    if (hook_) hook_(result);
    doc_.layout(w_, h_, measurer_);
    return true;
}

bool UiScript::move_to(Point p) {
    cursor_ = p;
    Event e{};
    e.type = EventType::MouseMove;
    e.pos = p;
    return dispatch(e);
}

bool UiScript::move_to(std::string_view target, Anchor anchor) {
    const Point p = point_of(target, anchor);
    if (p.x < 0) return false;
    return move_to(p);
}

bool UiScript::mouse_down(MouseButton button) {
    Event e{};
    e.type = EventType::MouseDown;
    e.button = button;
    e.pos = cursor_;
    return dispatch(e);
}

bool UiScript::mouse_up(MouseButton button) {
    Event e{};
    e.type = EventType::MouseUp;
    e.button = button;
    e.pos = cursor_;
    return dispatch(e);
}

bool UiScript::click(std::string_view target, Anchor anchor) {
    if (!move_to(target, anchor)) return false;
    return mouse_down() && mouse_up();
}

bool UiScript::drag_to(Point to, int steps) {
    if (!mouse_down()) return false;
    const Point from = cursor_;
    const int n = std::max(1, steps);
    for (int i = 1; i <= n; ++i) {
        const Point p{from.x + (to.x - from.x) * i / n,
                      from.y + (to.y - from.y) * i / n};
        if (!move_to(p)) return false;
    }
    cursor_ = to;
    return mouse_up();
}

bool UiScript::drag(std::string_view from, std::string_view to,
                    Anchor to_anchor, int steps) {
    if (!move_to(from, Anchor::Center)) return false;
    // Resolve the destination AFTER the press: a press can reload the view
    // (selection), moving everything.
    if (!mouse_down()) return false;
    const Point dest = point_of(to, to_anchor);
    if (dest.x < 0) {
        mouse_up();
        return false;
    }
    const Point start = cursor_;
    const int n = std::max(1, steps);
    for (int i = 1; i <= n; ++i) {
        const Point p{start.x + (dest.x - start.x) * i / n,
                      start.y + (dest.y - start.y) * i / n};
        if (!move_to(p)) return false;
    }
    return mouse_up();
}

bool UiScript::key(Key k, bool shift, bool ctrl, bool alt) {
    Event e{};
    e.type = EventType::KeyDown;
    e.key = k;
    e.shift = shift;
    e.ctrl = ctrl;
    e.alt = alt;
    return dispatch(e);
}

bool UiScript::type_text(std::string_view text) {
    for (const char c : text) {
        Event e{};
        e.type = EventType::TextInput;
        e.text.assign(1, c);
        if (!dispatch(e)) return false;
    }
    return true;
}

bool UiScript::log_contains(std::string_view needle) const {
    return std::any_of(log_.begin(), log_.end(), [&](const std::string& line) {
        return line.find(needle) != std::string::npos;
    });
}

std::vector<std::string> UiScript::validate_dock_layout(
    const Document::DockLayout& layout,
    const std::vector<std::string>& expected_panels) {
    std::vector<std::string> issues;
    if (!layout.present) {
        issues.push_back("no dock layout present");
        return issues;
    }
    std::vector<std::string> seen;
    std::function<void(const Document::DockLayout::Node&, bool)> walk =
        [&](const Document::DockLayout::Node& n, bool in_float) {
            if (n.split) {
                if (n.children.empty()) {
                    issues.push_back("empty split in the dock tree");
                }
                if (!in_float && n.children.size() == 1) {
                    issues.push_back("single-child split (uncollapsed wrapper)");
                }
                for (const auto& c : n.children) walk(c, in_float);
                return;
            }
            if (n.tabs.empty()) {
                issues.push_back("pane with no tabs");
                return;
            }
            for (const auto& id : n.tabs) seen.push_back(id);
            if (!n.active.empty() &&
                std::find(n.tabs.begin(), n.tabs.end(), n.active) ==
                    n.tabs.end()) {
                issues.push_back("active tab '" + n.active +
                                 "' is not one of the pane's tabs");
            }
        };
    walk(layout.root, false);
    for (const auto& f : layout.floats) {
        if (f.w <= 0 || f.h <= 0) {
            issues.push_back("float with a non-positive size");
        }
        walk(f.pane, true);
    }
    for (const auto& id : expected_panels) {
        const auto n = std::count(seen.begin(), seen.end(), id);
        if (n != 1) {
            issues.push_back("panel '" + id + "' appears " +
                             std::to_string(n) + " times (expected 1)");
        }
    }
    return issues;
}

}  // namespace affineui
