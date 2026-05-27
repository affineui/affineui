#pragma once

#include "decius_widgets.h"

#include <affineui/affineui.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace demo::decius {

struct ValueAccess {
    std::function<float(std::string_view)> get;
    std::function<void(std::string_view, float)> set;
    std::function<void()> rerender;
};

inline bool set_boolean_attr(affineui::Ui& ui,
                             std::string_view id,
                             std::string_view attr,
                             bool enabled) {
    return enabled ? ui.set_attr(id, attr, "true")
                   : ui.remove_attr(id, attr);
}

inline bool set_checked(affineui::Ui& ui,
                        std::string_view id,
                        bool checked) {
    return set_boolean_attr(ui, id, "aria-checked", checked);
}

inline bool set_pressed(affineui::Ui& ui,
                        std::string_view id,
                        bool pressed) {
    return set_boolean_attr(ui, id, "aria-pressed", pressed);
}

namespace detail {

inline const std::string* attr(const affineui::Document::HoverInfo& info,
                               std::string_view name) {
    for (const auto& [attr_name, attr_value] : info.attrs) {
        if (attr_name == name) return &attr_value;
    }
    return nullptr;
}

inline bool has_attr(const affineui::Document::HoverInfo& info,
                     std::string_view name) {
    return attr(info, name) != nullptr;
}

inline float attr_float(const affineui::Document::HoverInfo& info,
                        std::string_view name,
                        float fallback) {
    const auto* value = attr(info, name);
    if (!value || value->empty()) return fallback;
    try {
        return std::stof(*value);
    } catch (...) {
        return fallback;
    }
}

inline float value_from_x(const affineui::Rect& r, float x,
                          float min, float max) {
    const float t = r.w > 0
        ? clamp((x - static_cast<float>(r.x)) / static_cast<float>(r.w),
                0.0f, 1.0f)
        : 0.0f;
    return min + t * (max - min);
}

inline float value_from_y(const affineui::Rect& r, float y,
                          float min, float max) {
    const float t = r.h > 0
        ? clamp((y - static_cast<float>(r.y)) / static_cast<float>(r.h),
                0.0f, 1.0f)
        : 0.0f;
    // CSS `top` is a downward screen coordinate, while vertical faders use
    // the common control convention: minimum at the bottom, maximum at top.
    return min + (1.0f - t) * (max - min);
}

}  // namespace detail

inline void install_value_interactions(affineui::Ui& ui,
                                       ValueAccess access) {
    struct Drag {
        enum class Kind { None, Slider, Fader, Knob };
        Kind kind{Kind::None};
        std::string id;
        affineui::Rect bounds{};
        float min{0.0f};
        float max{1.0f};
        float start_value{0.0f};
        int start_y{0};
        bool bipolar{false};
    };

    auto drag = std::make_shared<Drag>();
    ui.on_event([drag, access = std::move(access), live_ui = &ui](
                    const affineui::Event& e,
                    const std::vector<affineui::Document::HoverInfo>& chain) {
        auto live_update = [&](float v) {
            if (!live_ui || drag->id.empty()) return false;
            if (drag->kind == Drag::Kind::Slider) {
                bool ok = true;
                ok = live_ui->set_attr(
                         child_id(drag->id, "__fill"), "style",
                         slider_fill_style(drag->min, drag->max, v,
                                           drag->bipolar)) && ok;
                ok = live_ui->set_attr(
                         child_id(drag->id, "__thumb"), "style",
                         slider_thumb_style(drag->min, drag->max, v)) && ok;
                return ok;
            }
            if (drag->kind == Drag::Kind::Fader) {
                return live_ui->set_attr(drag->id, "style", fader_style(v));
            }
            if (drag->kind == Drag::Kind::Knob) {
                bool ok = true;
                ok = live_ui->set_attr(
                         child_id(drag->id, "__indicator"), "style",
                         "--angle:" + num(knob_indicator_angle(
                             drag->min, drag->max, v)) + "deg") && ok;
                const auto arc = knob_arc_path(
                    drag->min, drag->max, v, drag->bipolar);
                if (arc.empty()) {
                    ok = live_ui->remove_attr(
                             child_id(drag->id, "__arc"), "d") && ok;
                } else {
                    ok = live_ui->set_attr(
                             child_id(drag->id, "__arc"), "d", arc) && ok;
                }
                ok = live_ui->set_text(
                         child_id(drag->id, "__value"), num(v)) && ok;
                return ok;
            }
            return false;
        };

        auto commit = [&](float value) {
            const float v = clamp(value, drag->min, drag->max);
            const float old = access.get ? access.get(drag->id) : v;
            if (std::abs(old - v) < 0.0005f) return;
            if (access.set) access.set(drag->id, v);
            if (!live_update(v) && access.rerender) {
                access.rerender();
            }
        };

        if (drag->kind != Drag::Kind::None) {
            if (e.type == affineui::EventType::MouseMove) {
                if (drag->kind == Drag::Kind::Slider) {
                    commit(detail::value_from_x(
                        drag->bounds, static_cast<float>(e.pos.x),
                        drag->min, drag->max));
                } else if (drag->kind == Drag::Kind::Fader) {
                    commit(detail::value_from_y(
                        drag->bounds, static_cast<float>(e.pos.y),
                        drag->min, drag->max));
                } else if (drag->kind == Drag::Kind::Knob) {
                    const float range = drag->max - drag->min;
                    commit(drag->start_value +
                           (static_cast<float>(drag->start_y - e.pos.y) / 150.0f) *
                               range);
                }
                return true;
            }
            if (e.type == affineui::EventType::MouseUp) {
                drag->kind = Drag::Kind::None;
                if (live_ui) live_ui->release_pointer();
                return true;
            }
            return e.type == affineui::EventType::MouseDown;
        }

        if (e.type != affineui::EventType::MouseDown ||
            e.button != affineui::MouseButton::Left) {
            return false;
        }

        for (const auto& info : chain) {
            if (info.elem_id.empty()) continue;
            Drag::Kind kind = Drag::Kind::None;
            if (detail::has_attr(info, "data-dcs-slider")) {
                kind = Drag::Kind::Slider;
            } else if (detail::has_attr(info, "data-dcs-fader")) {
                kind = Drag::Kind::Fader;
            } else if (detail::has_attr(info, "data-dcs-knob")) {
                kind = Drag::Kind::Knob;
            }
            if (kind == Drag::Kind::None) continue;

            drag->kind = kind;
            drag->id = info.elem_id;
            drag->bounds = info.bounds;
            drag->min = detail::attr_float(info, "data-min", 0.0f);
            drag->max = detail::attr_float(info, "data-max", 1.0f);
            drag->bipolar = detail::has_attr(info, "data-bipolar");
            drag->start_value = access.get
                ? access.get(drag->id)
                : detail::attr_float(info, "data-value", drag->min);
            drag->start_y = e.pos.y;
            if (live_ui) live_ui->capture_pointer();

            if (kind == Drag::Kind::Slider) {
                commit(detail::value_from_x(
                    info.bounds, static_cast<float>(e.pos.x),
                    drag->min, drag->max));
            } else if (kind == Drag::Kind::Fader) {
                commit(detail::value_from_y(
                    info.bounds, static_cast<float>(e.pos.y),
                    drag->min, drag->max));
            }
            return true;
        }
        return false;
    });
}

}  // namespace demo::decius
