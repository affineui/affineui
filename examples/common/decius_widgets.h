#pragma once

#include "demo_assets.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace demo::decius {

inline float clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

inline std::string num(float v, int places = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(places) << v;
    auto s = out.str();
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

inline float pct(float min, float max, float value) {
    if (std::abs(max - min) < 0.00001f) return 0.0f;
    return clamp((value - min) / (max - min), 0.0f, 1.0f) * 100.0f;
}

inline std::string child_id(std::string_view id, std::string_view suffix) {
    if (id.empty()) return {};
    return std::string(id) + std::string(suffix);
}

inline std::string slider_fill_style(float min, float max, float value,
                                     bool bipolar) {
    const float p = pct(min, max, value);
    std::ostringstream fill;
    if (bipolar) {
        const float start = std::min(50.0f, p);
        const float width = std::abs(p - 50.0f);
        fill << "left:" << num(start) << "%;right:auto;width:" << num(width) << "%";
    } else {
        fill << "width:" << num(p) << "%";
    }
    return fill.str();
}

inline std::string slider_thumb_style(float min, float max, float value) {
    return "left:" + num(pct(min, max, value)) + "%";
}

inline std::string fader_style(float pos) {
    const float p = (1.0f - clamp(pos, 0.0f, 1.0f)) * 100.0f;
    return "--pos:" + num(p) + "%";
}

inline std::string icon(std::string_view name) {
    return "<i class=\"di di-" + std::string(name) + "\"></i>";
}

inline std::string button(std::string_view label,
                          std::string_view icon_name = {},
                          std::string_view classes = {},
                          std::string_view id = {},
                          bool pressed = false) {
    std::ostringstream h;
    h << "<button";
    if (!id.empty()) h << " id=\"" << id << "\"";
    h << " class=\"dcs-btn";
    if (!classes.empty()) h << ' ' << classes;
    h << "\"";
    if (pressed) h << " aria-pressed=\"true\"";
    h << ">";
    if (!icon_name.empty()) h << icon(icon_name) << ' ';
    h << demo::html_escape(label) << "</button>";
    return h.str();
}

inline std::string icon_button(std::string_view icon_name,
                               std::string_view id = {},
                               bool pressed = false,
                               std::string_view extra_classes = {}) {
    std::ostringstream h;
    h << "<button";
    if (!id.empty()) h << " id=\"" << id << "\"";
    h << " class=\"dcs-btn dcs-btn--icon";
    if (!extra_classes.empty()) h << ' ' << extra_classes;
    h << "\"";
    if (pressed) h << " aria-pressed=\"true\"";
    h << ">" << icon(icon_name) << "</button>";
    return h.str();
}

inline std::string check(std::string_view label, bool checked,
                         bool radio = false, std::string_view id = {}) {
    std::ostringstream h;
    h << "<div";
    if (!id.empty()) h << " id=\"" << id << "\"";
    h << " class=\"dcs-check";
    if (radio) h << " dcs-radio";
    h << "\"";
    if (checked) h << " aria-checked=\"true\"";
    h << "><div class=\"dcs-check__box\">";
    if (!radio) h << icon("check");
    h << "</div><span>" << demo::html_escape(label) << "</span></div>";
    return h.str();
}

inline std::string toggle(std::string_view label, bool checked,
                          std::string_view id = {}) {
    std::ostringstream h;
    h << "<div class=\"dcs-row\"><div";
    if (!id.empty()) h << " id=\"" << id << "\"";
    h << " class=\"dcs-switch\"";
    if (checked) h << " aria-checked=\"true\"";
    h << "></div><span>" << demo::html_escape(label) << "</span></div>";
    return h.str();
}

inline std::string slider(float min, float max, float value,
                          bool bipolar = false,
                          bool ticks = false,
                          std::string_view id = {}) {
    std::ostringstream h;
    h << "<div";
    if (!id.empty()) h << " id=\"" << id << "\"";
    h << " data-dcs-slider data-min=\"" << num(min)
      << "\" data-max=\"" << num(max)
      << "\" data-value=\"" << num(value) << "\"";
    if (bipolar) h << " data-bipolar";
    h << " class=\"dcs-slider";
    if (bipolar) h << " dcs-slider--bipolar";
    h << "\"><div class=\"dcs-slider__track\">";
    if (ticks) {
        h << "<div class=\"dcs-slider__tick\" style=\"left:25%\"></div>"
          << "<div class=\"dcs-slider__tick\" style=\"left:50%\"></div>"
          << "<div class=\"dcs-slider__tick\" style=\"left:75%\"></div>";
    }
    h << "<div";
    if (!id.empty()) h << " id=\"" << child_id(id, "__fill") << "\"";
    h << " class=\"dcs-slider__fill\" style=\""
      << slider_fill_style(min, max, value, bipolar) << "\"></div>"
      << "<div";
    if (!id.empty()) h << " id=\"" << child_id(id, "__thumb") << "\"";
    h << " class=\"dcs-slider__thumb\" style=\""
      << slider_thumb_style(min, max, value) << "\"></div>"
      << "</div></div>";
    return h.str();
}

inline std::string fader(float pos, bool ticks = true,
                         std::string_view id = {}) {
    std::ostringstream h;
    h << "<div";
    if (!id.empty()) h << " id=\"" << id << "\"";
    h << " data-dcs-fader data-min=\"0\" data-max=\"1\" data-value=\""
      << num(pos) << "\" class=\"dcs-fader\" style=\"" << fader_style(pos) << "\">"
      << "<div class=\"dcs-fader__track\"></div>";
    if (ticks) {
        h << "<div class=\"dcs-fader__tick\" style=\"top:25%\"></div>"
          << "<div class=\"dcs-fader__tick\" style=\"top:50%\"></div>"
          << "<div class=\"dcs-fader__tick\" style=\"top:75%\"></div>";
    }
    h << "<div";
    if (!id.empty()) h << " id=\"" << child_id(id, "__thumb") << "\"";
    h << " class=\"dcs-fader__thumb\"></div></div>";
    return h.str();
}

inline float knob_indicator_angle(float min, float max, float value) {
    const float p = pct(min, max, value) / 100.0f;
    return -135.0f + p * 270.0f;
}

inline std::pair<float, float> knob_ring_point(float deg) {
    constexpr float r = 10.5f;
    const float rad = deg * 3.1415926535f / 180.0f;
    return {12.0f + r * std::cos(rad), 12.0f + r * std::sin(rad)};
}

inline std::string knob_arc_path(float min, float max, float value,
                                 bool bipolar) {
    const float p = pct(min, max, value) / 100.0f;
    const float arc_sweep = bipolar ? (p - 0.5f) * 270.0f : p * 270.0f;
    if (std::abs(arc_sweep) <= 0.5f) return {};
    const float arc_start = bipolar ? -90.0f : -225.0f;
    const float arc_end = arc_start + arc_sweep;
    const auto [x0, y0] = knob_ring_point(arc_start);
    const auto [x1, y1] = knob_ring_point(arc_end);
    const int large = std::abs(arc_sweep) > 180.0f ? 1 : 0;
    const int sweep = arc_end >= arc_start ? 1 : 0;
    std::ostringstream d;
    d << "M " << num(x0) << ' ' << num(y0)
      << " A 10.5 10.5 0 " << large << ' ' << sweep << ' '
      << num(x1) << ' ' << num(y1);
    return d.str();
}

inline std::string knob(float min, float max, float value,
                        std::string_view label,
                        bool bipolar = false,
                        int size = 0,
                        std::string_view id = {}) {
    const float p = pct(min, max, value) / 100.0f;
    const float indicator_angle = knob_indicator_angle(min, max, value);
    const float arc_sweep = bipolar ? (p - 0.5f) * 270.0f : p * 270.0f;
    const float arc_start = bipolar ? -90.0f : -225.0f;
    const float arc_end = arc_start + arc_sweep;
    constexpr float r = 10.5f;
    const auto [bg_x0, bg_y0] = knob_ring_point(-225.0f);
    const auto [bg_x1, bg_y1] = knob_ring_point(45.0f);
    const auto [x0, y0] = knob_ring_point(arc_start);
    const auto [x1, y1] = knob_ring_point(arc_end);
    const int large = std::abs(arc_sweep) > 180.0f ? 1 : 0;
    const int sweep = arc_end >= arc_start ? 1 : 0;

    std::ostringstream h;
    h << "<div";
    if (!id.empty()) h << " id=\"" << id << "\"";
    h << " data-dcs-knob data-min=\"" << num(min)
      << "\" data-max=\"" << num(max)
      << "\" data-value=\"" << num(value) << "\"";
    if (bipolar) h << " data-bipolar";
    h << " class=\"dcs-knob\"";
    if (size > 0) h << " style=\"--knob-size:" << size << "px\"";
    h << ">"
      << "<svg class=\"dcs-knob__ring\" viewBox=\"0 0 24 24\">"
      << "<path d=\"M " << num(bg_x0) << ' ' << num(bg_y0)
      << " A " << r << ' ' << r << " 0 1 1 " << num(bg_x1) << ' ' << num(bg_y1)
      << "\" fill=\"none\" stroke=\"rgba(255,255,255,.08)\" stroke-width=\"1.5\" stroke-linecap=\"round\"></path>";
    if (std::abs(arc_sweep) > 0.5f) {
        h << "<path";
        if (!id.empty()) h << " id=\"" << child_id(id, "__arc") << "\"";
        h << " class=\"dcs-knob__arc\" d=\"M " << num(x0) << ' ' << num(y0)
          << " A " << r << ' ' << r << " 0 " << large << ' ' << sweep << ' '
          << num(x1) << ' ' << num(y1)
          << "\" fill=\"none\" stroke=\"var(--dcs-accent)\" stroke-width=\"1.75\" stroke-linecap=\"round\"></path>";
    } else {
        h << "<path";
        if (!id.empty()) h << " id=\"" << child_id(id, "__arc") << "\"";
        h << " class=\"dcs-knob__arc\" fill=\"none\" stroke=\"var(--dcs-accent)\" stroke-width=\"1.75\" stroke-linecap=\"round\"></path>";
    }
    h << "</svg><div class=\"dcs-knob__cap\"></div>"
      << "<div";
    if (!id.empty()) h << " id=\"" << child_id(id, "__indicator") << "\"";
    h << " class=\"dcs-knob__indicator\" style=\"--angle:" << num(indicator_angle) << "deg\"></div>"
      << "<div class=\"dcs-knob__label\">" << demo::html_escape(label) << "</div>"
      << "<div";
    if (!id.empty()) h << " id=\"" << child_id(id, "__value") << "\"";
    h << " class=\"dcs-knob__value\">" << num(value) << "</div></div>";
    return h.str();
}

inline std::string hardware_screws() {
    return "<span class=\"dcs-hw__screw dcs-hw__screw--tl\"></span>"
           "<span class=\"dcs-hw__screw dcs-hw__screw--tr\"></span>"
           "<span class=\"dcs-hw__screw dcs-hw__screw--bl\"></span>"
           "<span class=\"dcs-hw__screw dcs-hw__screw--br\"></span>";
}

}  // namespace demo::decius
