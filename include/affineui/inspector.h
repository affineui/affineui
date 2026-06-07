#pragma once

// ── Reflected property inspector ────────────────────────────────────────────
//
// Bridges the object reflection layer (object.h) to the View component layer:
// it turns a reflected object's properties into editor fields, choosing the
// widget from each property's attributes (attr::Color, attr::Slider, …) and its
// value type. This is the generic "inspector over arbitrary objects" — the
// AffineUI answer to Qt's property grid — and it works on ANY Reflectable type
// without that type knowing anything about the UI.
//
//   v.container("dcs-props", "props");
//   affineui::inspect(v, mesh, [&](std::string_view prop, const auto& value) {
//       run_set_property_command(mesh, prop, value);   // app owns the edit/undo
//   });
//
// The widget-choice policy lives here (a sensible default); an app that wants a
// different mapping can iterate property_count()/property_at() itself and call
// the View components directly.

#include <charconv>
#include <functional>
#include <string>
#include <string_view>
#include <variant>

#include "affineui/object.h"
#include "affineui/view.h"

namespace affineui {

namespace detail {
inline double parse_double_or(std::string_view s, double fallback) {
    double out = fallback;
    const auto* first = s.data();
    const auto [ptr, ec] = std::from_chars(first, first + s.size(), out);
    return ec == std::errc{} && ptr != first ? out : fallback;
}
inline bool parse_bool(std::string_view s) {
    return s == "true" || s == "1" || s == "on" || s == "yes";
}
inline std::string number_text(double value) {
    std::string s = std::to_string(value);
    // Trim trailing zeros / a dangling decimal point for a tidy field value.
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}
}  // namespace detail

/// Build one editor field for a reflected property `prop` whose current value
/// is `current`. The widget is chosen from the property's attributes first
/// (attr::Color → colour field, attr::Slider(+Range) → slider) then its value
/// type (bool → checkbox, double → number, string → text). `on_change` receives
/// the new value already converted back to the property's type. Returns the
/// field ref.
inline WidgetRef property_field(
    View& v, const PropertyInfo& prop, const PropertyValue& current,
    std::function<void(const PropertyValue&)> on_change,
    std::string_view key = {}) {
    const std::string k = key.empty() ? prop.name : std::string(key);
    const std::string label(prop.display_label());

    const auto as_string = [&] {
        return std::holds_alternative<std::string>(current)
                   ? std::get<std::string>(current)
                   : std::string{};
    };
    const auto as_double = [&] {
        return std::holds_alternative<double>(current)
                   ? std::get<double>(current)
                   : 0.0;
    };

    // attr::Color → the Decius colour field.
    if (prop.has<attr::Color>()) {
        auto ref = v.colorfield(label, as_string(), k);
        ref.on_change([cb = std::move(on_change)](std::string_view val) {
            cb(PropertyValue{std::string(val)});
        });
        return ref;
    }
    // attr::Slider (with optional attr::Range) → a slider.
    if (prop.has<attr::Slider>()) {
        double mn = 0.0, mx = 1.0;
        if (const auto* r = prop.attribute<attr::Range>()) { mn = r->min; mx = r->max; }
        auto ref = v.slider(label, as_double(), mn, mx, k);
        ref.on_change([cb = std::move(on_change)](std::string_view val) {
            cb(PropertyValue{detail::parse_double_or(val, 0.0)});
        });
        return ref;
    }
    // Otherwise dispatch on the value type.
    if (std::holds_alternative<bool>(current)) {
        auto ref = v.checkbox(label, std::get<bool>(current), k);
        ref.on_change([cb = std::move(on_change)](std::string_view val) {
            cb(PropertyValue{detail::parse_bool(val)});
        });
        return ref;
    }
    if (std::holds_alternative<double>(current)) {
        auto ref = v.input(label, detail::number_text(as_double()), "number", k);
        ref.on_change([cb = std::move(on_change)](std::string_view val) {
            cb(PropertyValue{detail::parse_double_or(val, 0.0)});
        });
        return ref;
    }
    // string (text)
    auto ref = v.input(label, as_string(), "text", k);
    ref.on_change([cb = std::move(on_change)](std::string_view val) {
        cb(PropertyValue{std::string(val)});
    });
    return ref;
}

/// Build editor fields for every property of a reflectable object `obj` (in
/// declared order) into the current container. `on_edit(prop, value)` is called
/// with the property name and new value when a field changes — the app routes
/// that to its command/undo system and writes it back via set_property.
template <Reflectable T>
void inspect(View& v, const T& obj,
             std::function<void(std::string_view, const PropertyValue&)> on_edit) {
    const ObjectClass& cls = get_class(obj);
    for (std::size_t i = 0; i < cls.property_count(); ++i) {
        const PropertyInfo prop = cls.property_at(i);
        if (!prop.valid() || !prop.get) continue;
        const PropertyValue current = prop.get(&obj);
        const bool read_only = prop.has<attr::ReadOnly>() || !prop.set;
        auto field = property_field(
            v, prop, current,
            [on_edit, name = prop.name](const PropertyValue& value) {
                if (on_edit) on_edit(name, value);
            });
        if (read_only) field.attr("aria-disabled", "true");
    }
}

}  // namespace affineui
