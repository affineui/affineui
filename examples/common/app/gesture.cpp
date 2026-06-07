#include "app/gesture.h"

#include <cstdlib>

namespace app {

bool GestureRouter::on_pointer(const affineui::Event& ev) {
    if (!tool_) return false;

    switch (ev.type) {
        case affineui::EventType::MouseDown: {
            if (ev.button != affineui::MouseButton::Left) return false;
            pressed_  = true;
            dragging_ = false;
            drag_ = Drag{ev.pos, ev.pos, {0, 0}, ev.ctrl, ev.shift, ev.alt};
            return true;
        }
        case affineui::EventType::MouseMove: {
            if (!pressed_) return false;
            drag_.current = ev.pos;
            drag_.delta   = {ev.pos.x - drag_.start.x, ev.pos.y - drag_.start.y};
            if (!dragging_) {
                if (std::abs(drag_.delta.x) >= threshold_ ||
                    std::abs(drag_.delta.y) >= threshold_) {
                    dragging_ = true;
                    tool_->on_drag_begin(ctx_, drag_);
                }
            }
            if (dragging_) tool_->on_drag_update(ctx_, drag_);
            return dragging_;
        }
        case affineui::EventType::MouseUp: {
            if (ev.button != affineui::MouseButton::Left || !pressed_) return false;
            pressed_ = false;
            if (dragging_) {
                drag_.current = ev.pos;
                drag_.delta   = {ev.pos.x - drag_.start.x,
                                 ev.pos.y - drag_.start.y};
                tool_->on_drag_end(ctx_, drag_);
                dragging_ = false;
            } else {
                tool_->on_click(ctx_, ev.pos);
            }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace app
