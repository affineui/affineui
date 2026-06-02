#include "affineui/view.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <sstream>
#include <utility>

namespace affineui {

namespace {

std::uint64_t fnv1a_mix(std::uint64_t h, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) {
        h ^= (x >> (i * 8)) & 0xffu;
        h *= 0x100000001b3ull;
    }
    return h;
}

std::uint64_t fnv1a_bytes(std::uint64_t h, std::string_view text) {
    for (unsigned char c : text) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

StableId make_stable_id(StableId parent,
                        WidgetKind kind,
                        std::string_view key,
                        std::source_location here) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    h = fnv1a_mix(h, parent.value);
    h = fnv1a_mix(h, static_cast<std::uint64_t>(kind));
    if (!key.empty()) {
        h = fnv1a_bytes(h, key);
    } else {
        h = fnv1a_bytes(h, here.file_name());
        h = fnv1a_mix(h, here.line());
        h = fnv1a_mix(h, here.column());
    }
    return {h};
}

StableId make_duplicate_stable_id(StableId base, std::size_t index) {
    return {fnv1a_mix(base.value, index + 1)};
}

bool public_widget_key(std::string_view key) {
    return !key.empty() && !key.starts_with("__");
}

std::string remote_id(StableId id) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "aui-%016llx",
                  static_cast<unsigned long long>(id.value));
    return std::string{buf};
}

const char* patch_op_name(RemotePatchOp op) {
    switch (op) {
        case RemotePatchOp::CreateElement:   return "create_element";
        case RemotePatchOp::CreateText:      return "create_text";
        case RemotePatchOp::Remove:          return "remove";
        case RemotePatchOp::SetText:         return "set_text";
        case RemotePatchOp::SetAttribute:    return "set_attr";
        case RemotePatchOp::RemoveAttribute: return "remove_attr";
    }
    return "unknown";
}

std::string escape_html(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string escape_json(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

void set_attr(std::vector<WidgetAttribute>& attrs,
              std::string_view name,
              std::string_view value) {
    auto it = std::find_if(attrs.begin(), attrs.end(),
        [&](const WidgetAttribute& attr) { return attr.name == name; });
    if (it != attrs.end()) {
        it->value = std::string(value);
        return;
    }
    attrs.push_back({std::string(name), std::string(value)});
}

bool has_attr(const std::vector<WidgetAttribute>& attrs,
              std::string_view name) {
    return std::any_of(attrs.begin(), attrs.end(),
        [&](const WidgetAttribute& attr) { return attr.name == name; });
}

const WidgetAttribute* find_attr(const std::vector<WidgetAttribute>& attrs,
                                 std::string_view name) {
    auto it = std::find_if(attrs.begin(), attrs.end(),
        [&](const WidgetAttribute& attr) { return attr.name == name; });
    return it == attrs.end() ? nullptr : &*it;
}

bool has_any_attr(const std::vector<WidgetAttribute>& attrs,
                  std::initializer_list<std::string_view> names) {
    return std::any_of(names.begin(), names.end(),
        [&](std::string_view name) { return has_attr(attrs, name); });
}

std::string selector_value(std::string_view key, std::string_view value) {
    if (key == bootstrap::selector::size || key == decius::selector::size) {
        if (value == "med" || value == "medium") return "md";
    }
    return std::string(value);
}

void append_attrs_html(const std::vector<WidgetAttribute>& attrs,
                       std::string& out) {
    for (const auto& attr : attrs) {
        out += ' ';
        out += attr.name;
        if (!attr.value.empty()) {
            out += "=\"";
            out += escape_html(attr.value);
            out += '"';
        }
    }
}

char selector_attr_char(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-') {
        return c;
    }
    if (c == '_' || c == ' ') return '-';
    return '\0';
}

std::string normalized_selector_key(std::string_view name) {
    if (name.starts_with("data-aui-")) name.remove_prefix(9);
    else if (name.starts_with("data-dcs-")) name.remove_prefix(9);
    else if (name.starts_with("data-bs-")) name.remove_prefix(8);

    std::string out;
    out.reserve(name.size());
    bool previous_dash = false;
    for (char c : name) {
        const char mapped = selector_attr_char(c);
        if (mapped == '\0') return {};
        if (mapped == '-') {
            if (out.empty() || previous_dash) continue;
            previous_dash = true;
        } else {
            previous_dash = false;
        }
        out.push_back(mapped);
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

std::string selector_attr_name(std::string_view name) {
    const auto key = normalized_selector_key(name);
    if (key.empty()) return {};
    return "data-aui-" + key;
}

std::string dom_id_fragment(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool previous_dash = false;
    for (char c : text) {
        const char mapped = selector_attr_char(c);
        if (mapped == '\0') continue;
        if (mapped == '-') {
            if (out.empty() || previous_dash) continue;
            previous_dash = true;
        } else {
            previous_dash = false;
        }
        out.push_back(mapped);
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "color" : out;
}

std::string number(double value) {
    char buf[64]{};
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    if (ec == std::errc{}) return std::string(buf, ptr);
    std::ostringstream out;
    out << value;
    return out.str();
}

double normalized_value(double value, double min, double max) {
    if (max <= min) return 0.0;
    return std::clamp((value - min) / (max - min), 0.0, 1.0);
}

double parse_double_or(std::string_view text, double fallback) {
    double value = fallback;
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    return (ec == std::errc{} && ptr != first) ? value : fallback;
}

std::string percent(double fraction) {
    return number(std::clamp(fraction, 0.0, 1.0) * 100.0) + "%";
}

std::string px(double value) {
    return number(std::max(0.0, value)) + "px";
}

double virtual_item_size(const VirtualListOptions& options,
                         std::size_t index) {
    if (index < options.item_sizes.size()) {
        return std::max(0.0, options.item_sizes[index]);
    }
    return std::max(1.0, options.item_size);
}

double virtual_item_offset(const VirtualListOptions& options,
                           std::size_t end) {
    double out = 0.0;
    for (std::size_t i = 0; i < end && i < options.item_count; ++i) {
        out += virtual_item_size(options, i);
    }
    return out;
}

double knob_angle(double value, double min, double max) {
    return -135.0 + normalized_value(value, min, max) * 270.0;
}

std::pair<double, double> knob_ring_point(double deg) {
    constexpr double r = 10.5;
    constexpr double pi = 3.14159265358979323846;
    const double rad = deg * pi / 180.0;
    return {12.0 + r * std::cos(rad), 12.0 + r * std::sin(rad)};
}

std::string knob_arc_path(double min, double max, double value, bool bipolar) {
    const double p = normalized_value(value, min, max);
    const double sweep_degrees = bipolar ? (p - 0.5) * 270.0 : p * 270.0;
    if (std::abs(sweep_degrees) <= 0.5) return {};

    const double start_degrees = bipolar ? -90.0 : -225.0;
    const double end_degrees = start_degrees + sweep_degrees;
    const auto [x0, y0] = knob_ring_point(start_degrees);
    const auto [x1, y1] = knob_ring_point(end_degrees);
    const int large = std::abs(sweep_degrees) > 180.0 ? 1 : 0;
    const int sweep = end_degrees >= start_degrees ? 1 : 0;

    return "M " + number(x0) + " " + number(y0) +
           " A 10.5 10.5 0 " + std::to_string(large) + " " +
           std::to_string(sweep) + " " + number(x1) + " " + number(y1);
}

void emit_remove_tree(ViewSink* sink, const WidgetNode& node) {
    if (!sink) return;
    sink->remove(node);
}

enum class FrameworkElement {
    Panel,
    Card,
    CardTitle,
    Button,
    CheckboxGroup,
    CheckboxInput,
    CheckboxLabel,
    FieldGroup,
    FieldLabel,
    TextInput,
    TextArea,
    SelectInput,
    ButtonGroup,
    ButtonGroupButton,
    SliderGroup,
    SliderLabel,
    SliderInput,
    KnobGroup,
    KnobLabel,
    KnobInput,
};

struct ElementRecipe {
    std::string_view tag;
    std::string_view classes;
};

class ViewFramework {
public:
    virtual ~ViewFramework() = default;

    [[nodiscard]] virtual std::string_view stylesheet_href() const noexcept = 0;
    [[nodiscard]] virtual Color background_color() const noexcept = 0;
    [[nodiscard]] virtual ElementRecipe element(FrameworkElement element,
                                                bool primary = false) const noexcept = 0;
    virtual void adjust_document_attrs(
        std::vector<WidgetAttribute>& attrs) const = 0;
    virtual void adjust_widget_attrs(
        WidgetKind kind,
        std::vector<WidgetAttribute>& attrs) const = 0;
    virtual void apply_selector_attrs(
        std::string_view key,
        std::string_view value,
        std::vector<WidgetAttribute>& attrs) const = 0;
};

class PlainFramework final : public ViewFramework {
public:
    [[nodiscard]] std::string_view stylesheet_href() const noexcept override {
        return {};
    }

    [[nodiscard]] Color background_color() const noexcept override {
        return Color{30, 30, 46, 255};
    }

    [[nodiscard]] ElementRecipe element(FrameworkElement element,
                                        bool primary = false) const noexcept override {
        (void) primary;
        switch (element) {
            case FrameworkElement::Card:          return {"section", {}};
            case FrameworkElement::Button:        return {"button", {}};
            case FrameworkElement::CheckboxGroup: return {"label", {}};
            case FrameworkElement::CheckboxInput: return {"input", {}};
            case FrameworkElement::CheckboxLabel: return {"span", {}};
            case FrameworkElement::FieldGroup:    return {"label", {}};
            case FrameworkElement::FieldLabel:    return {"span", {}};
            case FrameworkElement::TextInput:     return {"input", {}};
            case FrameworkElement::TextArea:      return {"textarea", {}};
            case FrameworkElement::SelectInput:   return {"select", {}};
            case FrameworkElement::ButtonGroup:   return {"div", {}};
            case FrameworkElement::ButtonGroupButton:
                return {"button", {}};
            case FrameworkElement::SliderGroup:   return {"label", {}};
            case FrameworkElement::SliderLabel:   return {"span", {}};
            case FrameworkElement::SliderInput:   return {"input", {}};
            case FrameworkElement::KnobGroup:     return {"label", {}};
            case FrameworkElement::KnobLabel:     return {"span", {}};
            case FrameworkElement::KnobInput:     return {"input", {}};
            case FrameworkElement::Panel:
            case FrameworkElement::CardTitle:
                return {"div", {}};
        }
        return {"div", {}};
    }

    void adjust_document_attrs(
        std::vector<WidgetAttribute>& attrs) const override {
        (void) attrs;
    }

    void adjust_widget_attrs(
        WidgetKind kind,
        std::vector<WidgetAttribute>& attrs) const override {
        (void) kind;
        (void) attrs;
    }

    void apply_selector_attrs(
        std::string_view key,
        std::string_view value,
        std::vector<WidgetAttribute>& attrs) const override {
        set_attr(attrs, "data-aui-" + std::string(key), value);
    }
};

class BootstrapFramework final : public ViewFramework {
public:
    [[nodiscard]] std::string_view stylesheet_href() const noexcept override {
        return "frameworks/css/bootstrap-5.3.8.min.css";
    }

    [[nodiscard]] Color background_color() const noexcept override {
        return Color{0xFF, 0xFF, 0xFF, 0xFF};
    }

    [[nodiscard]] ElementRecipe element(FrameworkElement element,
                                        bool primary = false) const noexcept override {
        switch (element) {
            case FrameworkElement::Panel:
                return {"section", "card shadow-sm p-4 aui-bs-form"};
            case FrameworkElement::Card:          return {"section", "card shadow-sm"};
            case FrameworkElement::CardTitle:     return {"h3", "card-header h6 mb-0"};
            case FrameworkElement::Button:
                return {"button", primary ? "btn btn-primary" : "btn btn-outline-secondary"};
            case FrameworkElement::CheckboxGroup: return {"div", "aui-bs-field"};
            case FrameworkElement::CheckboxInput: return {"input", "form-check-input"};
            case FrameworkElement::CheckboxLabel: return {"label", "aui-bs-field__label form-label"};
            case FrameworkElement::FieldGroup:    return {"div", "aui-bs-field"};
            case FrameworkElement::FieldLabel:    return {"label", "aui-bs-field__label form-label"};
            case FrameworkElement::TextInput:     return {"input", "form-control"};
            case FrameworkElement::TextArea:      return {"textarea", "form-control"};
            case FrameworkElement::SelectInput:   return {"select", "form-select"};
            case FrameworkElement::ButtonGroup:   return {"div", "btn-group"};
            case FrameworkElement::ButtonGroupButton:
                return {"button", primary ? "btn btn-primary" : "btn btn-outline-primary"};
            case FrameworkElement::SliderGroup:   return {"div", "aui-bs-field"};
            case FrameworkElement::SliderLabel:   return {"label", "aui-bs-field__label form-label"};
            case FrameworkElement::SliderInput:   return {"input", "form-range"};
            case FrameworkElement::KnobGroup:     return {"div", "aui-bs-field"};
            case FrameworkElement::KnobLabel:     return {"label", "aui-bs-field__label form-label"};
            case FrameworkElement::KnobInput:     return {"input", "form-range"};
        }
        return {"div", {}};
    }

    void adjust_document_attrs(
        std::vector<WidgetAttribute>& attrs) const override {
        if (!has_attr(attrs, "data-aui-size")) {
            apply_selector_attrs(bootstrap::selector::size,
                                 bootstrap::size::md,
                                 attrs);
        }
    }

    void adjust_widget_attrs(
        WidgetKind kind,
        std::vector<WidgetAttribute>& attrs) const override {
        (void) kind;
        (void) attrs;
    }

    void apply_selector_attrs(
        std::string_view key,
        std::string_view value,
        std::vector<WidgetAttribute>& attrs) const override {
        const auto canonical = selector_value(key, value);
        set_attr(attrs, "data-aui-" + std::string(key), canonical);
        if (key == bootstrap::selector::theme) {
            set_attr(attrs, "data-bs-theme", canonical);
        }
    }
};

class DeciusFramework final : public ViewFramework {
public:
    [[nodiscard]] std::string_view stylesheet_href() const noexcept override {
        return "frameworks/css/decius-css-0.5.2.bundle.min.css";
    }

    [[nodiscard]] Color background_color() const noexcept override {
        return Color{0x1F, 0x22, 0x2A, 0xFF};
    }

    [[nodiscard]] ElementRecipe element(FrameworkElement element,
                                        bool primary = false) const noexcept override {
        switch (element) {
            case FrameworkElement::Panel:
                return {"section", "dcs-panel dcs-panel--bordered dcs-panel--raised dcs-form"};
            case FrameworkElement::Card:
                return {"section", "dcs-panel dcs-panel--bordered"};
            case FrameworkElement::CardTitle:
                return {"h3", "dcs-panel__header"};
            case FrameworkElement::Button:
                return {"button", primary ? "dcs-btn dcs-btn--primary" : "dcs-btn"};
            case FrameworkElement::CheckboxGroup: return {"div", "dcs-field"};
            case FrameworkElement::CheckboxInput: return {"input", "dcs-check__input"};
            case FrameworkElement::CheckboxLabel: return {"span", "dcs-field__label"};
            case FrameworkElement::FieldGroup:    return {"label", "dcs-field"};
            case FrameworkElement::FieldLabel:    return {"span", "dcs-field__label"};
            case FrameworkElement::TextInput:     return {"input", "dcs-input"};
            case FrameworkElement::TextArea:      return {"textarea", "dcs-textarea dcs-field__fill"};
            case FrameworkElement::SelectInput:   return {"select", "dcs-select"};
            case FrameworkElement::ButtonGroup:   return {"div", "dcs-btn-group"};
            case FrameworkElement::ButtonGroupButton:
                return {"button", "dcs-btn"};
            case FrameworkElement::SliderGroup:   return {"div", "dcs-field"};
            case FrameworkElement::SliderLabel:   return {"span", "dcs-field__label"};
            case FrameworkElement::SliderInput:   return {"div", "dcs-slider"};
            case FrameworkElement::KnobGroup:     return {"div", "dcs-field"};
            case FrameworkElement::KnobLabel:     return {"span", "dcs-field__label"};
            case FrameworkElement::KnobInput:     return {"div", "dcs-knob"};
        }
        return {"div", {}};
    }

    void adjust_document_attrs(
        std::vector<WidgetAttribute>& attrs) const override {
        set_attr(attrs, "class", "dcs");
        if (!has_attr(attrs, "data-aui-size")) {
            apply_selector_attrs(decius::selector::size, decius::size::md, attrs);
        }
        if (!has_any_attr(attrs, {"data-aui-style", "data-dcs-style"})) {
            apply_selector_attrs(decius::selector::style,
                                 decius::style::flat,
                                 attrs);
        }
        if (!has_any_attr(attrs, {"data-aui-density", "data-dcs-density"})) {
            apply_selector_attrs(decius::selector::density,
                                 decius::density::compact,
                                 attrs);
        }
    }

    void adjust_widget_attrs(
        WidgetKind kind,
        std::vector<WidgetAttribute>& attrs) const override {
        (void) kind;
        (void) attrs;
    }

    void apply_selector_attrs(
        std::string_view key,
        std::string_view value,
        std::vector<WidgetAttribute>& attrs) const override {
        const auto canonical = selector_value(key, value);
        set_attr(attrs, "data-aui-" + std::string(key), canonical);
        if (key == decius::selector::style ||
            key == decius::selector::density ||
            key == decius::selector::accent ||
            key == decius::selector::radius ||
            key == decius::selector::dark) {
            set_attr(attrs, "data-dcs-" + std::string(key), canonical);
        }
    }
};

const ViewFramework& framework_for(ViewTheme theme) {
    static const PlainFramework plain;
    static const BootstrapFramework bootstrap;
    static const DeciusFramework decius;

    switch (theme) {
        case ViewTheme::Bootstrap: return bootstrap;
        case ViewTheme::Decius:    return decius;
        case ViewTheme::Plain:     return plain;
    }
    return bootstrap;
}

std::string theme_link(ViewTheme theme) {
    const auto href = framework_for(theme).stylesheet_href();
    if (href.empty()) return {};
    std::string out = "<link rel=\"stylesheet\" href=\"";
    out += href;
    out += "\">";
    return out;
}

std::vector<WidgetAttribute> document_attrs(
    ViewTheme theme,
    const std::vector<WidgetAttribute>& explicit_attrs) {
    auto attrs = explicit_attrs;
    framework_for(theme).adjust_document_attrs(attrs);
    return attrs;
}

std::string body_attrs(ViewTheme theme,
                       const std::vector<WidgetAttribute>& explicit_attrs) {
    std::string out;
    append_attrs_html(document_attrs(theme, explicit_attrs), out);
    return out;
}

std::string command_widget_style() {
    std::string css;
    css.reserve(32768);
    css += R"CSS(
body{margin:0}.aui-root{min-height:100vh;padding:24px;box-sizing:border-box}
[data-aui-size=sm]{--aui-panel-pad:var(--dcs-s-3);--aui-panel-gap:var(--dcs-s-2)}
[data-aui-size=md]{--aui-panel-pad:var(--dcs-s-5);--aui-panel-gap:var(--dcs-s-3)}
[data-aui-size=lg]{--aui-panel-pad:var(--dcs-s-7);--aui-panel-gap:var(--dcs-s-5)}
.aui-bs-form,.aui-bs-props,.aui-bs-col{display:flex;flex-direction:column}
.aui-bs-form{gap:1rem}.aui-bs-props{gap:.5rem}.aui-bs-col{gap:1rem}
.aui-bs-row,.aui-bs-btn-row{display:flex;align-items:center;gap:.5rem}
.aui-bs-form>.aui-bs-field,.aui-bs-props>.aui-bs-field{display:flex;align-items:center;gap:.75rem;margin-bottom:0;min-width:0}
.aui-bs-form>.aui-bs-field{justify-content:center}
.aui-bs-form>.aui-bs-field>.aui-bs-field__label{flex:0 0 42%;max-width:220px;text-align:right;margin-bottom:0}
.aui-bs-props>.aui-bs-field>.aui-bs-field__label{flex:0 0 128px;text-align:left;margin-bottom:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.aui-bs-form>.aui-bs-field>.form-control,.aui-bs-form>.aui-bs-field>.form-select,.aui-bs-form>.aui-bs-field>.form-range,.aui-bs-form>.aui-bs-field>.aui-select{flex:0 1 280px;max-width:280px;min-width:0}
.aui-bs-props>.aui-bs-field>.form-control,.aui-bs-props>.aui-bs-field>.form-select,.aui-bs-props>.aui-bs-field>.form-range,.aui-bs-props>.aui-bs-field>.aui-select{flex:1 1 auto;min-width:0}
.aui-bs-field>input[type=number].form-control{cursor:ew-resize}
.aui-bs-form>.aui-bs-field>.form-control-color,.aui-bs-props>.aui-bs-field>.form-control-color{flex:0 0 3rem;min-width:3rem;width:3rem;height:2.25rem}
.aui-bs-field>.btn-group{display:inline-flex;flex:0 0 auto;width:auto}
.aui-bs-field>.btn-group>.btn{flex:0 0 auto;min-width:64px;white-space:nowrap}
.aui-bs-field>.aui-knob,.aui-bs-field>.aui-bs-check{flex:0 0 auto}
.aui-select{display:flex;flex-direction:column;min-width:0;width:100%;position:relative}
.aui-select>.form-select,.aui-select>.dcs-select{width:100%}
.aui-select__menu[hidden]{display:none}
.aui-select__menu:not([hidden]){display:flex!important;position:fixed!important;flex-direction:column;align-items:stretch;z-index:400;max-height:240px;overflow:auto}
.aui-select__menu .dropdown-item,.aui-select__menu .dcs-menu__item{border:0;background:transparent;text-align:left;font:inherit;width:100%;box-sizing:border-box}
.aui-select__menu .dcs-menu__item{margin-left:calc(var(--dcs-s-2)*-1);margin-right:calc(var(--dcs-s-2)*-1);width:calc(100% + var(--dcs-s-2)*2)}
.aui-bs-check{min-height:auto;margin-bottom:0;padding-left:1.5em}
.aui-bs-form>.btn{align-self:center}.aui-bs-props>.btn,.aui-bs-props>.btn-group{align-self:flex-start}.aui-bs-props>.aui-bs-btn-row>.btn{flex:1 1 0}
.aui-root>.card.aui-bs-form,.aui-root>.card.aui-bs-props,.aui-root>.card.aui-bs-col{gap:1rem}
.aui-root>.card>h1,.aui-root>.card>h2,.aui-root>.card>h3{margin-bottom:0}
.aui-root>.card>p{margin-bottom:0;color:var(--bs-secondary-color,#6c757d)}
.aui-knob{--aui-knob-size:64px;position:relative;display:inline-flex;align-items:flex-start;justify-content:center;width:var(--aui-knob-size);height:98px;padding-top:16px;box-sizing:border-box;cursor:ns-resize;user-select:none;color:inherit;touch-action:none}
.aui-knob__ring{position:absolute;left:0;top:16px;width:var(--aui-knob-size);height:var(--aui-knob-size);pointer-events:none}
.aui-knob__cap{position:absolute;left:14px;top:30px;width:36px;height:36px;border-radius:50%;background:linear-gradient(180deg,#f8f9fa,#dee2e6);border:1px solid rgba(0,0,0,.24);box-shadow:inset 0 1px 0 rgba(255,255,255,.85),0 1px 3px rgba(0,0,0,.2)}
.aui-knob__indicator{position:absolute;left:50%;top:48px;width:2px;height:24px;background:var(--bs-primary,#0d6efd);border-radius:1px;transform-origin:50% 100%;transform:translate(-50%,-100%) rotate(var(--angle,0deg))}
.aui-knob__value{position:absolute;left:0;right:0;top:0;text-align:center;font-size:11px;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;color:var(--bs-primary,#0d6efd)}
.aui-knob__label{position:absolute;left:-8px;right:-8px;bottom:0;text-align:center;font-size:12px;color:var(--bs-secondary-color,#6c757d);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.aui-knob__arc{stroke:var(--bs-primary,#0d6efd)}
.aui-root .aui-knob[data-aui-widget=knob]{display:flex;margin-top:16px;margin-bottom:18px}
.aui-root .dcs-card-list{gap:var(--dcs-s-2)}
.aui-root .dcs-card-list>.dcs-card{padding:var(--dcs-s-4);text-align:left}
.aui-root .dcs-field>.aui-select,.aui-root .dcs-field>.dcs-colorfield,.aui-root .dcs-field>.dcs-combo{flex:1 1 auto;min-width:0}
.aui-root .dcs-field>.dcs-btn-group{display:inline-flex;flex:0 0 auto;width:auto;min-width:0}
.aui-root .dcs-field>.dcs-btn-group>.dcs-btn{flex:0 0 auto;min-width:64px;white-space:nowrap}
.aui-root .dcs-combo>input.dcs-combo__value{background:transparent;border:0;outline:0;text-align:right;min-width:0;cursor:ew-resize}
.aui-root .dcs-colorfield{position:relative}
.aui-root .dcs-colorfield>.dcs-colorfield__chip{border:0;padding:0;background:var(--c,#3bb7ff);color:transparent;appearance:none}
.aui-root .aui-color-menu{gap:1px;align-items:stretch}
.aui-root .aui-color-menu .aui-color-option{display:flex;align-items:center;gap:8px;width:100%;box-sizing:border-box}
.aui-root .aui-color-option__swatch{display:inline-block;width:14px;height:14px;border-radius:2px;background:var(--c,#fff);box-shadow:inset 0 0 0 1px rgba(0,0,0,.35)}
.aui-virtual-list{position:relative;display:flex;flex-direction:column;min-height:0;overflow:auto}
.aui-virtual-list__spacer{flex:0 0 auto;min-height:0;pointer-events:none}
.aui-virtual-list__row{flex:0 0 auto;display:flex;min-width:0}
.aui-virtual-list__row>.list-group-item{width:100%;border-left-width:1px;border-right-width:1px}
.aui-virtual-list__row>.dcs-card,.aui-virtual-list__row>.dcs-list__item{display:flex;align-items:center;justify-content:flex-start;width:100%;min-width:100%;box-sizing:border-box;text-align:left}
.aui-scroll{overflow:auto;min-height:0}
.aui-tree-list{display:flex;flex-direction:column;align-items:stretch;width:100%;max-height:220px;overflow:auto}
.aui-tree-list .list-group-item,.aui-tree-list .dcs-tree__row,.aui-tree-list .aui-tree-row,.aui-scroll-tree .list-group-item,.aui-scroll-tree .dcs-tree__row,.aui-scroll-tree .aui-tree-row{display:flex;align-items:center;justify-content:flex-start;width:100%;min-width:100%;box-sizing:border-box;text-align:left}
.aui-tree-list .aui-tree-row::before{display:inline-flex;align-items:center;justify-content:center;flex:0 0 1.15em;width:1.15em;margin-left:-.15em;margin-right:.15em;opacity:.72}
.aui-tree-list .aui-tree-branch[aria-expanded=true]::before{content:"\25be"}
.aui-tree-list .aui-tree-branch:not([aria-expanded=true])::before{content:"\25b8"}
.aui-tree-list .aui-tree-leaf::before{content:""}
.aui-scroll-tree .aui-tree-row::before{display:inline-flex;align-items:center;justify-content:center;flex:0 0 1.15em;width:1.15em;margin-left:-.15em;margin-right:.15em;opacity:.72}
.aui-scroll-tree .aui-tree-branch[aria-expanded=true]::before{content:"\25be"}
.aui-scroll-tree .aui-tree-branch:not([aria-expanded=true])::before{content:"\25b8"}
.aui-scroll-tree .aui-tree-leaf::before{content:""}
.aui-root .dcs-field[data-aui-widget=knob]{height:auto;min-height:calc(var(--knob-size,56px) + 34px);align-items:center}
.aui-root .dcs-field[data-aui-widget=knob]>.dcs-knob{flex:0 0 auto;margin:18px 0 20px}
.aui-root .dcs-field[data-aui-widget=checkbox]>.dcs-check{flex:0 0 auto}
.aui-root>.dcs-panel{padding:var(--aui-panel-pad,var(--dcs-s-5));gap:var(--aui-panel-gap,var(--dcs-s-3))}
.aui-root>.dcs-panel>h1,.aui-root>.dcs-panel>h2,.aui-root>.dcs-panel>h3{margin:0}
.aui-root>.dcs-panel>p{margin:0;color:var(--dcs-text-dim)}
)CSS";
    css += R"CSS(
.aui-demo-grid{display:flex;flex-direction:row;flex-wrap:wrap;gap:1rem;align-items:flex-start;min-width:0}
.aui-demo-stack{display:flex;flex-direction:column;gap:1rem;min-width:0}
.aui-demo-grid>.aui-demo-section{flex:1 1 320px}
.aui-demo-stack>.aui-demo-section{flex:0 0 auto;width:100%}
.aui-demo-section{display:block;flex-grow:0;flex-shrink:0;flex-basis:auto;min-width:0;min-height:auto;overflow:visible}
.aui-root .aui-demo-section.dcs-panel{display:block;flex:0 0 auto;min-height:auto;overflow:visible}
.aui-demo-section>.card-header,.aui-demo-section>.dcs-panel__header{margin:0}
.aui-demo-section>.card-body,.aui-demo-section>.dcs-panel__body{flex-grow:0;flex-shrink:0;flex-basis:auto;height:auto;min-height:auto;max-height:none;min-width:0;overflow:visible}
.aui-root .aui-demo-section.dcs-panel>.dcs-panel__body{display:flex;flex-direction:column;flex:0 0 auto;height:auto;min-height:auto;max-height:none;overflow:visible}
.aui-root .aui-demo-section.dcs-subpanel{background:transparent;border-bottom:1px solid var(--dcs-line-soft)}
.aui-root .aui-demo-section.dcs-subpanel>.dcs-subpanel__header{background:transparent;border-bottom:1px solid var(--dcs-line-soft)}
.aui-root .aui-demo-section.dcs-subpanel>.dcs-subpanel__body{display:flex;flex-direction:column;gap:var(--dcs-s-4);height:auto;min-height:auto;overflow:visible;padding:var(--dcs-s-4) 0 var(--dcs-s-5)}
.aui-root .aui-demo-section.dcs-subpanel>.dcs-subpanel__body>.dcs-btn{align-self:flex-start}
.aui-root .aui-demo-section.dcs-subpanel>.dcs-subpanel__body.dcs-form>.dcs-btn{align-self:center}
.aui-root .aui-demo-section.dcs-subpanel>.dcs-subpanel__body.dcs-props>.dcs-btn{align-self:flex-start}
.aui-demo-section>.dcs-panel__body{gap:var(--dcs-s-4)}
.aui-demo-section>.card-body>p:first-child,.aui-demo-section>.dcs-panel__body>p:first-child{margin:0}
.aui-demo-section>.dcs-subpanel__body>p:first-child{margin:0}
.aui-root .dcs-props>.dcs-field[data-aui-widget=textarea]{height:auto;min-height:calc(var(--dcs-h-in)*3);align-items:flex-start}
.aui-root .dcs-props>.dcs-field[data-aui-widget=textarea]>.dcs-field__label{padding-top:3px}
.aui-root textarea.form-control,.aui-root textarea.dcs-textarea{text-align:left;resize:both;cursor:text}
.aui-root .dcs-props>.dcs-field[data-aui-widget=textarea]>.dcs-textarea{height:auto;min-height:calc(var(--dcs-h-in)*3);resize:both;text-align:left}
)CSS";
    css += R"CSS(
@font-face{font-family:decius-icons;src:url("frameworks/fonts/decius-icons.woff2") format("woff2");font-weight:400;font-style:normal;font-display:block}
.di,[class*=" di-"],[class^=di-]{font-family:decius-icons!important;font-style:normal;font-weight:400;font-variant:normal;text-transform:none;line-height:1;speak:never;display:inline-block;vertical-align:middle}
.di-check-circle:before{content:"\e01c"}.di-decius:before{content:"\e030"}.di-folder-open:before{content:"\e04f"}.di-gain:before{content:"\e051"}.di-gizmo:before{content:"\e053"}.di-grid:before{content:"\e059"}.di-keyframe:before{content:"\e066"}.di-layers:before{content:"\e06a"}.di-menu:before{content:"\e079"}.di-pencil:before{content:"\e090"}.di-rocket:before{content:"\e0a2"}
.aui-test-nav-icon{display:inline-flex;align-items:center;justify-content:center;margin:0;line-height:1;color:currentColor}
.aui-test-brand-icon{flex:0 0 auto;color:var(--bs-primary,var(--dcs-accent,#0d6efd));font-size:18px}
.aui-root>.aui-test-shell{margin:-24px;height:100vh;min-height:100vh}
.aui-test-shell{display:flex;flex-direction:column;height:100vh;min-height:0;background:inherit;color:inherit}
.aui-test-shell-inner{display:flex;flex:1 1 auto;min-height:0;flex-direction:column;overflow:hidden}
.aui-test-topbar{display:flex;align-items:stretch;flex-wrap:nowrap;gap:0;height:48px;min-height:48px;max-height:48px;padding:0 96px 0 16px;box-sizing:border-box;border-bottom:1px solid var(--bs-border-color,var(--dcs-line,rgba(128,128,128,.25)));background:var(--bs-body-bg,var(--dcs-surface-1,inherit));overflow:hidden;white-space:nowrap;flex:0 0 auto;min-width:0}
.aui-test-topbar.dcs-menubar{height:var(--dcs-h-lg);min-height:var(--dcs-h-lg);max-height:var(--dcs-h-lg);padding:0 96px 0 var(--dcs-s-3);gap:0}
.aui-test-brand{display:flex;align-items:center;gap:8px;flex:0 1 340px;max-width:36%;min-width:0;overflow:hidden;white-space:nowrap}
.aui-test-brand h1{margin:0;font-size:1rem;font-weight:700;line-height:1.2;white-space:nowrap}
.aui-test-brand p,.aui-test-subtitle p{margin:0}
.aui-test-subtitle{display:flex;align-items:center;min-width:0;overflow:hidden;color:var(--bs-secondary-color,var(--dcs-text-mute,#6c757d))}
.aui-test-subtitle p{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.aui-test-tweaks{display:flex;align-items:stretch;justify-content:flex-end;flex:1 1 auto;flex-wrap:nowrap;gap:0;min-width:0;margin-left:auto;height:100%;overflow:hidden}
.aui-test-control{display:flex;align-items:center;flex:0 1 auto;gap:7px;height:100%;min-width:0;padding:0 10px;border-left:1px solid var(--bs-border-color,var(--dcs-line,rgba(128,128,128,.25)));box-sizing:border-box;overflow:hidden}
.aui-test-control-label{margin:0;color:var(--bs-secondary-color,var(--dcs-text-mute,#6c757d));font-size:var(--dcs-fs-xs,.72rem);font-weight:600;letter-spacing:.06em;text-transform:uppercase;white-space:nowrap}
.aui-test-segment-group{display:inline-flex;align-items:center;flex:0 0 auto;width:auto;min-width:0;max-width:none}
.aui-test-segment-group.btn-group{display:inline-flex;height:30px;width:auto}
.aui-test-segment-group.dcs-btn-group{display:inline-flex;height:var(--dcs-h,24px);width:auto}
.aui-test-segment{flex:0 0 auto;width:auto;min-width:48px;white-space:nowrap}
.aui-test-segment-group.dcs-btn-group>.aui-test-segment{flex:0 0 auto;width:auto;min-width:56px;height:100%;padding-left:10px;padding-right:10px}
.aui-test-segment-group.btn-group>.aui-test-segment{flex:0 0 auto;width:auto;min-width:58px;height:100%;padding-left:10px;padding-right:10px}
.aui-test-keycolor{display:flex;align-items:center;gap:5px;height:100%;padding:0 2px 0 10px;border-left:1px solid var(--bs-border-color,var(--dcs-line,rgba(128,128,128,.25)));min-width:0;box-sizing:border-box}
.aui-keycolor-label{margin:0;color:var(--dcs-text-mute,#6c757d);font-size:var(--dcs-fs-xs,.72rem);font-weight:600;letter-spacing:.06em;text-transform:uppercase}
.aui-keycolor-swatch{--aui-swatch:#00b8d4;flex:0 0 18px;width:18px;height:18px;border-radius:50%;background:var(--aui-swatch);border:2px solid transparent;box-shadow:inset 0 1px 0 rgba(255,255,255,.35),inset 0 -1px 1px rgba(0,0,0,.35),0 1px 2px rgba(0,0,0,.45);cursor:pointer;box-sizing:border-box}
.aui-keycolor-swatch--cyan{--aui-swatch:#00b8d4}.aui-keycolor-swatch--green{--aui-swatch:#3dd68a}.aui-keycolor-swatch--orange{--aui-swatch:#ff8a3a}.aui-keycolor-swatch--violet{--aui-swatch:#8b6dff}
.aui-keycolor-swatch:hover{border-color:rgba(255,255,255,.35);box-shadow:inset 0 1px 0 rgba(255,255,255,.42),inset 0 -1px 1px rgba(0,0,0,.35),0 1px 2px rgba(0,0,0,.45)}
.aui-keycolor-swatch.is-active{border-color:var(--dcs-bg-app,#1f222a);box-shadow:0 0 0 2px var(--dcs-bg-app,#1f222a),0 0 0 4px var(--aui-swatch),inset 0 1px 0 rgba(255,255,255,.42),inset 0 -1px 1px rgba(0,0,0,.35),0 1px 2px rgba(0,0,0,.45)}
)CSS";
    css += R"CSS(
.aui-test-perf-reserve{flex:0 0 72px;min-width:0}
.aui-test-mobile-nav{display:none;padding:12px 16px;border-bottom:1px solid var(--bs-border-color,var(--dcs-line,rgba(128,128,128,.25)))}
.aui-test-body{display:flex;flex:1 1 auto;min-height:0;align-items:stretch;overflow:hidden}
.aui-test-nav{flex:0 0 256px;display:flex;flex-direction:column;gap:18px;min-height:0;max-height:100%;padding:18px 14px;box-sizing:border-box;border-right:1px solid var(--bs-border-color,var(--dcs-line,rgba(128,128,128,.25)));background:linear-gradient(180deg,rgba(128,128,128,.055),rgba(128,128,128,.025));overflow-x:hidden;overflow-y:auto;overscroll-behavior:contain}
.aui-test-nav-heading{flex:0 0 auto;margin:0 6px -4px;font-size:.78rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--bs-secondary-color,var(--dcs-text-dim,#6c757d))}
.aui-test-nav-group{display:flex;flex:0 0 auto;flex-direction:column;gap:4px;min-width:0}
.aui-test-nav-group-title{display:flex;align-items:center;flex:0 0 auto;gap:7px;min-height:18px;margin:0 6px 2px;font-size:.72rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--bs-secondary-color,var(--dcs-text-mute,#6c757d));white-space:nowrap;overflow:hidden}
.aui-test-nav-group-title>.aui-test-nav-icon{flex:0 0 14px;width:14px;font-size:13px;color:var(--dcs-accent,var(--bs-primary,#0d6efd));opacity:.82}
.aui-test-nav-label{margin:0;min-width:0;color:inherit;font:inherit;letter-spacing:inherit;text-transform:inherit;overflow:hidden;text-overflow:ellipsis}
.aui-test-nav-item{appearance:none;display:flex;align-items:center;flex:0 0 auto;gap:10px;width:100%;min-height:38px;padding:8px 10px;box-sizing:border-box;border:0;border-radius:0;background:transparent;color:var(--bs-body-color,var(--dcs-text,#222));font:inherit;font-weight:600;text-align:left;white-space:normal;cursor:pointer}
.aui-test-nav-item>.aui-test-nav-icon{flex:0 0 18px;width:18px;height:18px;background:transparent;color:var(--bs-secondary-color,var(--dcs-text-dim,#6c757d));font-size:15px}
.aui-test-nav-item:hover{background:rgba(128,128,128,.09)}
.aui-test-nav-item.is-active{background:var(--dcs-accent,var(--bs-primary,#0d6efd));color:var(--dcs-accent-text,#fff);box-shadow:none}
.aui-test-nav-item.is-active>.aui-test-nav-icon{background:transparent;color:var(--dcs-accent-text,#fff)}
.aui-test-nav-item.is-active:hover{background:var(--dcs-accent,var(--bs-primary,#0d6efd));color:var(--dcs-accent-text,#fff)}
.aui-test-nav-item.is-active:hover>.aui-test-nav-icon{background:transparent;color:var(--dcs-accent-text,#fff)}
.aui-test-nav-item.is-disabled{opacity:.52;cursor:default}
.aui-test-nav-item.is-disabled:hover{background:transparent}
.aui-test-nav-item.is-disabled::after{content:"soon";margin-left:auto;padding:1px 6px;border-radius:999px;background:rgba(128,128,128,.14);font-size:.64rem;font-weight:700;letter-spacing:.04em;text-transform:uppercase;color:var(--bs-secondary-color,var(--dcs-text-mute,#6c757d))}
.aui-test-content{flex:1 1 auto;display:flex;min-width:0;min-height:0;overflow:hidden;padding:0;box-sizing:border-box}
.aui-test-content>h1{margin-top:0}
.aui-test-page{display:flex;flex:1 1 auto;flex-direction:column;width:100%;min-width:0;min-height:0;overflow:hidden}
.aui-test-page>.aui-test-page-header{margin:0 0 .75rem;font-size:clamp(1.45rem,2vw,2.1rem);line-height:1.15;font-weight:700;letter-spacing:0}
.aui-test-page>.aui-test-page-body{display:flex;flex:1 1 auto;flex-direction:column;gap:1.25rem;min-height:0;min-width:0;overflow:auto;overscroll-behavior:contain;padding:28px 32px;box-sizing:border-box}
.aui-test-page>.dcs-panel__body{gap:var(--dcs-s-5)}
.aui-scroll-list,.aui-scroll-tree{width:100%;min-height:0;overflow:auto;border:1px solid var(--bs-border-color,var(--dcs-line,rgba(128,128,128,.25)));border-radius:4px;box-sizing:border-box;background:rgba(0,0,0,.03)}
.aui-scroll-tree{max-height:340px}
.photo-hybrid-shell{display:flex;flex-direction:column;min-height:560px;border:1px solid var(--dcs-line,rgba(128,128,128,.25));background:var(--dcs-bg-app,#1f222a);overflow:hidden}
.photo-hybrid-toolbar{display:flex;align-items:center;gap:6px;min-height:34px;padding:5px 8px;box-sizing:border-box;border-bottom:1px solid var(--dcs-line,rgba(128,128,128,.25));background:var(--dcs-surface-1,#242832)}
.photo-hybrid-body{display:flex;flex:1 1 auto;min-height:0}
.photo-hybrid-tools{display:flex;flex:0 0 42px;flex-direction:column;gap:5px;padding:6px;box-sizing:border-box;border-right:1px solid var(--dcs-line,rgba(128,128,128,.25));background:rgba(0,0,0,.08)}
.photo-hybrid-canvas{position:relative;display:flex;flex:1 1 auto;min-width:0;min-height:420px;background:#151820;overflow:hidden}
.photo-hybrid-ruler{position:absolute;background:#20242e;border-color:var(--dcs-line,rgba(128,128,128,.25));z-index:1}
.photo-hybrid-ruler--top{left:24px;right:0;top:0;height:24px;border-bottom:1px solid var(--dcs-line,rgba(128,128,128,.25))}
.photo-hybrid-ruler--left{left:0;top:24px;bottom:24px;width:24px;border-right:1px solid var(--dcs-line,rgba(128,128,128,.25))}
.photo-hybrid-artboard{position:absolute;left:72px;right:72px;top:58px;bottom:56px;background:#0e1118;border:1px solid rgba(255,255,255,.16);box-shadow:0 18px 50px rgba(0,0,0,.35)}
.photo-hybrid-photo{position:absolute;inset:28px;background:linear-gradient(135deg,#f08c3c 0%,#e26b9c 42%,#7654d9 100%);border:1px solid rgba(255,255,255,.18)}
.photo-hybrid-selection{position:absolute;left:24%;top:22%;width:46%;height:52%;border:1px dashed rgba(255,255,255,.72);box-shadow:0 0 0 1px rgba(0,0,0,.5)}
.photo-hybrid-tool-label{position:absolute;left:16px;bottom:12px;margin:0;color:#fff;background:rgba(0,0,0,.35);padding:3px 8px;border-radius:3px;font-size:12px}
.photo-hybrid-status{position:absolute;left:24px;right:0;bottom:0;height:24px;display:flex;align-items:center;gap:14px;padding:0 10px;box-sizing:border-box;border-top:1px solid var(--dcs-line,rgba(128,128,128,.25));background:#20242e;color:var(--dcs-text-dim,#aaa);font-size:12px}
.photo-hybrid-status p{margin:0}
.photo-hybrid-inspector{display:flex;flex:0 0 260px;flex-direction:column;gap:8px;padding:10px;box-sizing:border-box;border-left:1px solid var(--dcs-line,rgba(128,128,128,.25));background:var(--dcs-surface-1,#20242e);overflow:auto}
.photo-hybrid-inspector h2{margin:4px 0 2px;font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--dcs-text-mute,#888)}
@media (max-width:1120px){
  .aui-test-control-label{display:none}
  .aui-test-control{padding-left:7px;padding-right:7px}
  .aui-test-segment-group.dcs-btn-group>.aui-test-segment{min-width:44px;padding-left:8px;padding-right:8px}
  .aui-test-segment-group.btn-group>.aui-test-segment{min-width:46px;padding-left:8px;padding-right:8px}
}
@media (max-width:980px){
  .aui-test-control--top-density,.aui-test-control--top-accent{display:none}
}
@media (max-width:860px){
  .aui-test-control--top-style{display:none}
}
@media (max-width:760px){
  .aui-test-topbar{align-items:stretch;flex-direction:row;height:48px;min-height:48px;max-height:48px;padding:0 16px}
  .aui-test-topbar.dcs-menubar{height:var(--dcs-h-lg);min-height:var(--dcs-h-lg);max-height:var(--dcs-h-lg);padding:0 var(--dcs-s-3)}
  .aui-test-brand{min-width:0;flex:1 1 auto;overflow:hidden}
  .aui-test-brand h1{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .aui-test-subtitle,.aui-test-tweaks{display:none}
  .aui-test-nav{display:none}
  .aui-test-mobile-nav{display:block}
  .aui-test-body{flex-direction:column}
  .aui-test-page>.aui-test-page-body{padding:18px}
}
)CSS";
    return css;
}

std::string default_class(ViewTheme theme,
                          FrameworkElement element,
                          bool primary = false) {
    return std::string{framework_for(theme).element(element, primary).classes};
}

ElementRecipe default_element(ViewTheme theme,
                              FrameworkElement element,
                              bool primary = false) {
    return framework_for(theme).element(element, primary);
}

Color default_background_color(ViewTheme theme) {
    return framework_for(theme).background_color();
}

bool is_void_tag(std::string_view tag) {
    return tag == "input" || tag == "img" || tag == "br" || tag == "hr" ||
           tag == "meta" || tag == "link";
}

void append_node_html(const WidgetNode& node, std::string& out) {
    if (node.kind == WidgetKind::Root) {
        for (const auto& child : node.children) append_node_html(child, out);
        return;
    }
    if (node.kind == WidgetKind::Text) {
        out += escape_html(node.text);
        return;
    }
    if (node.kind == WidgetKind::RawHtml) {
        out += node.text;
        return;
    }

    out += '<';
    out += node.tag;
    for (const auto& attr : node.attrs) {
        out += ' ';
        out += attr.name;
        if (!attr.value.empty()) {
            out += "=\"";
            out += escape_html(attr.value);
            out += '"';
        }
    }
    if (is_void_tag(node.tag)) {
        out += '>';
        return;
    }

    out += '>';
    out += escape_html(node.text);
    for (const auto& child : node.children) append_node_html(child, out);
    out += "</";
    out += node.tag;
    out += '>';
}

const WidgetNode* find_remote_impl(const WidgetNode& node,
                                   std::string_view id) {
    if (node.remote_id == id) return &node;
    for (const auto& child : node.children) {
        if (auto* found = find_remote_impl(child, id)) return found;
    }
    return nullptr;
}

WidgetNode* find_id_impl(WidgetNode& node, StableId id) {
    if (node.id == id) return &node;
    for (auto& child : node.children) {
        if (auto* found = find_id_impl(child, id)) return found;
    }
    return nullptr;
}

const WidgetNode* find_id_impl(const WidgetNode& node, StableId id) {
    if (node.id == id) return &node;
    for (const auto& child : node.children) {
        if (auto* found = find_id_impl(child, id)) return found;
    }
    return nullptr;
}

WidgetNode* find_widget_impl(WidgetNode& node, std::string_view name) {
    if (node.widget_name == name) return &node;
    for (auto& child : node.children) {
        if (auto* found = find_widget_impl(child, name)) return found;
    }
    return nullptr;
}

}  // namespace

void RemotePatchQueue::clear() {
    patches_.clear();
}

void RemotePatchQueue::push(RemotePatch patch) {
    patches_.push_back(std::move(patch));
}

std::string RemotePatchQueue::to_json() const {
    std::string out{"["};
    for (std::size_t i = 0; i < patches_.size(); ++i) {
        const auto& p = patches_[i];
        if (i != 0) out += ',';
        out += "{\"op\":\"";
        out += patch_op_name(p.op);
        out += "\",\"id\":\"";
        out += escape_json(p.id);
        out += "\"";
        if (!p.parent_id.empty()) {
            out += ",\"parent\":\"";
            out += escape_json(p.parent_id);
            out += "\"";
        }
        if (!p.tag.empty()) {
            out += ",\"tag\":\"";
            out += escape_json(p.tag);
            out += "\"";
        }
        if (!p.name.empty()) {
            out += ",\"name\":\"";
            out += escape_json(p.name);
            out += "\"";
        }
        if (!p.value.empty()) {
            out += ",\"value\":\"";
            out += escape_json(p.value);
            out += "\"";
        }
        out += ",\"index\":";
        out += std::to_string(p.index);
        out += '}';
    }
    out += ']';
    return out;
}

void RemotePatchSink::create_element(const WidgetNode& node,
                                     const WidgetNode* parent,
                                     std::size_t index) {
    if (!queue_) return;
    queue_->push(RemotePatch{
        RemotePatchOp::CreateElement,
        node.remote_id,
        parent ? parent->remote_id : std::string{},
        node.tag,
        {},
        {},
        index,
    });
}

void RemotePatchSink::create_text(const WidgetNode& node,
                                  const WidgetNode* parent,
                                  std::size_t index) {
    if (!queue_) return;
    queue_->push(RemotePatch{
        RemotePatchOp::CreateText,
        node.remote_id,
        parent ? parent->remote_id : std::string{},
        {},
        {},
        {},
        index,
    });
}

void RemotePatchSink::remove(const WidgetNode& node) {
    if (!queue_ || node.remote_id.empty()) return;
    queue_->push(RemotePatch{
        RemotePatchOp::Remove,
        node.remote_id,
    });
}

void RemotePatchSink::set_text(const WidgetNode& node, std::string_view value) {
    if (!queue_) return;
    queue_->push(RemotePatch{
        RemotePatchOp::SetText,
        node.remote_id,
        {},
        {},
        {},
        std::string(value),
    });
}

void RemotePatchSink::set_attribute(const WidgetNode& node,
                                    std::string_view name,
                                    std::string_view value) {
    if (!queue_) return;
    queue_->push(RemotePatch{
        RemotePatchOp::SetAttribute,
        node.remote_id,
        {},
        {},
        std::string(name),
        std::string(value),
    });
}

void RemotePatchSink::remove_attribute(const WidgetNode& node,
                                       std::string_view name) {
    if (!queue_) return;
    queue_->push(RemotePatch{
        RemotePatchOp::RemoveAttribute,
        node.remote_id,
        {},
        {},
        std::string(name),
    });
}

WidgetRef::WidgetRef(View* owner,
                     StableId panel_id,
                     StableId id,
                     std::string_view name)
    : owner_(owner), panel_id_(panel_id), id_(id), name_(name) {}

WidgetRef::operator bool() const {
    return owner_ != nullptr && owner_->resolve_widget_ref(*this) != nullptr;
}

StableId WidgetRef::id() const {
    if (owner_) {
        [[maybe_unused]] auto* resolved = owner_->resolve_widget_ref(*this);
    }
    return id_;
}

const WidgetNode* WidgetRef::node() const {
    return owner_ ? owner_->resolve_widget_ref(*this) : nullptr;
}

std::string_view WidgetRef::name() const {
    if (const auto* n = node()) return n->widget_name;
    return name_;
}

WidgetRef& WidgetRef::named(std::string_view name) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->set_widget_name(*n, name);
            name_ = n->widget_name;
        }
    }
    return *this;
}

WidgetRef& WidgetRef::clear() {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) owner_->clear_children(*n);
    }
    return *this;
}

WidgetRef& WidgetRef::text(std::string_view value) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) owner_->set_text(*n, value);
    }
    return *this;
}

WidgetRef& WidgetRef::attr(std::string_view name, std::string_view value) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->set_attr(*n, name, value);
        }
    }
    return *this;
}

WidgetRef& WidgetRef::remove_attr(std::string_view name) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) owner_->remove_attr(*n, name);
    }
    return *this;
}

WidgetRef& WidgetRef::selector(std::string_view name, std::string_view value) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->set_selector(*n, name, value);
        }
    }
    return *this;
}

WidgetRef& WidgetRef::cls(std::string_view classes) {
    return attr("class", classes);
}

WidgetRef& WidgetRef::on_click(std::function<void()> cb) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->set_click_handler(*n, std::move(cb));
        }
    }
    return *this;
}

WidgetRef& WidgetRef::on_change(std::function<void(std::string_view)> cb) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->set_change_handler(*n, std::move(cb));
        }
    }
    return *this;
}

WidgetRef& WidgetRef::append(const std::function<void(View&)>& build) {
    if (owner_ && owner_->can_mutate_children("append")) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->build_children(*n, build, false);
        }
    }
    return *this;
}

WidgetRef& WidgetRef::replace(const std::function<void(View&)>& build) {
    if (owner_ && owner_->can_mutate_children("replace")) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->build_children(*n, build, true);
        }
    }
    return *this;
}

WidgetRef WidgetRef::find_widget(std::string_view name) const {
    if (!owner_) return {};
    const auto* root = owner_->resolve_widget_ref(*this);
    if (!root) return WidgetRef{owner_, id_, {}, name};
    auto* found = owner_->find_widget_node_under(root->id, name);
    return found ? owner_->ref_for_node(*found, root->id, name)
                 : WidgetRef{owner_, root->id, {}, name};
}

View::Scope::Scope(View* owner, WidgetNode* node) noexcept
    : owner_(owner), node_(node) {}

View::Scope::~Scope() {
    if (owner_) owner_->close_node();
}

View::Scope::Scope(Scope&& other) noexcept
    : owner_(other.owner_), node_(other.node_) {
    other.owner_ = nullptr;
    other.node_ = nullptr;
}

View::Scope& View::Scope::operator=(Scope&& other) noexcept {
    if (this == &other) return *this;
    if (owner_) owner_->close_node();
    owner_ = other.owner_;
    node_ = other.node_;
    other.owner_ = nullptr;
    other.node_ = nullptr;
    return *this;
}

WidgetRef View::Scope::ref() const {
    return (owner_ && node_) ? owner_->ref_for_node(*node_, node_->id) : WidgetRef{};
}

View::Scope& View::Scope::named(std::string_view name) {
    if (owner_ && node_) owner_->set_widget_name(*node_, name);
    return *this;
}

View::Scope& View::Scope::attr(std::string_view name, std::string_view value) {
    if (owner_ && node_) owner_->set_attr(*node_, name, value);
    return *this;
}

View::Scope& View::Scope::selector(std::string_view name,
                                   std::string_view value) {
    if (owner_ && node_) owner_->set_selector(*node_, name, value);
    return *this;
}

View::Scope& View::Scope::cls(std::string_view classes) {
    if (owner_ && node_) owner_->set_attr(*node_, "class", classes);
    return *this;
}

View::Scope& View::Scope::text(std::string_view value) {
    if (owner_ && node_) owner_->set_text(*node_, value);
    return *this;
}

WidgetRef View::Scope::find_widget(std::string_view name) const {
    if (!owner_ || !node_) return {};
    auto* found = owner_->find_widget_node_under(node_->id, name);
    return found ? owner_->ref_for_node(*found, node_->id, name)
                 : WidgetRef{owner_, node_->id, {}, name};
}

StableId current_panel_id(const std::vector<WidgetNode*>& stack) {
    if (stack.empty()) return {};
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if ((*it)->kind == WidgetKind::Card ||
            (*it)->kind == WidgetKind::Container ||
            (*it)->kind == WidgetKind::Root) {
            return (*it)->id;
        }
    }
    return stack.back()->id;
}

View::View(ViewTheme theme) : theme_(theme) {
    root_.id = {0xcbf29ce484222325ull};
    root_.kind = WidgetKind::Root;
    root_.tag = "#root";
    root_.remote_id = "aui-root";
}

void View::clear() {
    root_.children.clear();
    root_.cursor = 0;
    stack_.clear();
    widget_names_.clear();
    click_handlers_.clear();
    change_handlers_.clear();
}

View& View::selector(std::string_view name, std::string_view value) {
    const auto key = normalized_selector_key(name);
    if (key.empty() || selector_attr_name(key).empty()) {
        diagnostics_.push_back("Invalid selector name: " + std::string(name));
        return *this;
    }
    framework_for(theme_).apply_selector_attrs(key, value, document_attrs_);
    return *this;
}

void View::begin(ViewSink* sink) {
    sink_ = sink;
    reconciling_ = true;
    root_.cursor = 0;
    stack_.clear();
    stack_.push_back(&root_);
}

void View::begin(RemotePatchQueue* remote_patches) {
    remote_patch_sink_.reset(remote_patches);
    begin(static_cast<ViewSink*>(&remote_patch_sink_));
}

void View::end() {
    while (stack_.size() > 1) close_node();
    if (!stack_.empty()) {
        auto* root = stack_.back();
        for (std::size_t i = root->cursor; i < root->children.size(); ++i) {
            unregister_tree(root->children[i]);
            emit_remove_tree(sink_, root->children[i]);
        }
        root->children.erase(root->children.begin() +
                             static_cast<std::ptrdiff_t>(root->cursor),
                             root->children.end());
    }
    stack_.clear();
    sink_ = nullptr;
    reconciling_ = false;
    remote_patch_sink_.reset(nullptr);
}

Color View::background_color() const noexcept {
    return default_background_color(theme_);
}

View::Scope View::container(std::string_view classes,
                            std::string_view key,
                            std::source_location here) {
    auto& node = open_node(WidgetKind::Container, "div", classes, key, here, true);
    return Scope{this, &node};
}

View::Scope View::element(std::string_view tag,
                          std::string_view classes,
                          std::string_view key,
                          std::source_location here) {
    if (tag.empty()) {
        diagnostics_.push_back("View::element requires a non-empty tag");
        tag = "div";
    }
    auto& node = open_node(WidgetKind::Container, tag, classes, key, here, true);
    return Scope{this, &node};
}

View::Scope View::panel(std::string_view key,
                        std::source_location here) {
    const auto recipe = default_element(theme_, FrameworkElement::Panel);
    auto& node = open_node(WidgetKind::Panel, recipe.tag, recipe.classes,
                           key, here, true);
    return Scope{this, &node};
}

View::Scope View::card(std::string_view title,
                       std::string_view classes,
                       std::string_view key,
                       std::source_location here) {
    std::string cls = default_class(theme_, FrameworkElement::Card);
    if (!classes.empty()) {
        if (!cls.empty()) cls += ' ';
        cls += classes;
    }
    const auto recipe = default_element(theme_, FrameworkElement::Card);
    auto& node = open_node(WidgetKind::Card, recipe.tag, cls, key, here, true);
    if (!title.empty()) {
        heading(3, title, default_class(theme_, FrameworkElement::CardTitle),
                "__title", here);
    }
    return Scope{this, &node};
}

WidgetRef View::heading(int level,
                        std::string_view value,
                        std::string_view classes,
                        std::string_view key,
                        std::source_location here) {
    if (level < 1) level = 1;
    if (level > 6) level = 6;
    const std::string tag = "h" + std::to_string(level);
    auto& node = open_node(WidgetKind::Heading, tag, classes, key, here, false);
    set_text(node, value);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::paragraph(std::string_view value,
                          std::string_view classes,
                          std::string_view key,
                          std::source_location here) {
    auto& node = open_node(WidgetKind::Container, "p", classes, key, here, false);
    set_text(node, value);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::text(std::string_view value,
                     std::string_view key,
                     std::source_location here) {
    auto& node = open_node(WidgetKind::Text, "#text", {}, key, here, false);
    set_text(node, value);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::html(std::string_view markup,
                     std::string_view key,
                     std::source_location here) {
    auto& node = open_node(WidgetKind::RawHtml, "#html", {}, key, here, false);
    set_text(node, markup);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::button(std::string_view label,
                       bool primary,
                       std::string_view key,
                       std::source_location here) {
    const auto recipe = default_element(theme_, FrameworkElement::Button, primary);
    auto& node = open_node(WidgetKind::Button, recipe.tag, recipe.classes,
                           key, here, false);
    set_attr(node, "type", "button");
    set_text(node, label);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::checkbox(std::string_view label,
                         bool checked,
                         std::string_view key,
                         std::source_location here) {
    const auto group_recipe = default_element(theme_, FrameworkElement::CheckboxGroup);
    auto& group = open_node(WidgetKind::Checkbox, group_recipe.tag,
                            group_recipe.classes, key, here, true);
    set_attr(group, "data-aui-widget", "checkbox");
    if (checked) set_attr(group, "aria-checked", "true");
    else remove_attr(group, "aria-checked");

    if (theme_ == ViewTheme::Decius) {
        const auto label_recipe = default_element(theme_, FrameworkElement::CheckboxLabel);
        auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                     label_recipe.classes, "__label", here, false);
        set_text(label_node, label);

        auto& check = open_node(WidgetKind::Checkbox, "div",
                                "dcs-check", "__check", here, true);
        set_attr(check, "role", "checkbox");
        if (checked) set_attr(check, "aria-checked", "true");
        else remove_attr(check, "aria-checked");

        auto& box = open_node(WidgetKind::Container, "div",
                              "dcs-check__box", "__box", here, true);
        (void) box;
        auto& icon = open_node(WidgetKind::Container, "i",
                               "di di-check", "__icon", here, false);
        (void) icon;
        close_node();

        const auto input_recipe = default_element(theme_, FrameworkElement::CheckboxInput);
        auto& input = open_node(WidgetKind::Checkbox, input_recipe.tag,
                                input_recipe.classes, "__input", here, false);
        set_attr(input, "type", "checkbox");
        set_attr(input, "style", "display:none");
        if (checked) set_attr(input, "checked", "checked");
        else remove_attr(input, "checked");

        close_node();
        close_node();
        return ref_for_node(group, current_panel_id(stack_));
    }

    if (theme_ == ViewTheme::Bootstrap) {
        const auto label_recipe = default_element(theme_, FrameworkElement::CheckboxLabel);
        auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                     label_recipe.classes, "__label", here, false);
        set_text(label_node, label);

        auto& check = open_node(WidgetKind::Container, "label",
                                "form-check aui-bs-check", "__check", here, true);
        (void) check;
        const auto input_recipe = default_element(theme_, FrameworkElement::CheckboxInput);
        auto& input = open_node(WidgetKind::Checkbox, input_recipe.tag,
                                input_recipe.classes,
                                "__input", here, false);
        set_attr(input, "type", "checkbox");
        if (checked) set_attr(input, "checked", "checked");
        else remove_attr(input, "checked");
        close_node();
        close_node();
        return ref_for_node(group, current_panel_id(stack_));
    }

    const auto input_recipe = default_element(theme_, FrameworkElement::CheckboxInput);
    auto& input = open_node(WidgetKind::Checkbox, input_recipe.tag,
                            input_recipe.classes,
                            "__input", here, false);
    set_attr(input, "type", "checkbox");
    if (checked) set_attr(input, "checked", "checked");
    else remove_attr(input, "checked");

    const auto label_recipe = default_element(theme_, FrameworkElement::CheckboxLabel);
    auto& span = open_node(WidgetKind::Container, label_recipe.tag,
                           label_recipe.classes,
                           "__label", here, false);
    set_text(span, label);
    close_node();
    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::input(std::string_view label,
                      std::string_view value,
                      std::string_view type,
                      std::string_view key,
                      std::source_location here) {
    const std::string_view input_type = type.empty() ? "text" : type;
    const auto group_recipe = default_element(theme_, FrameworkElement::FieldGroup);
    auto& group = open_node(WidgetKind::TextInput, group_recipe.tag,
                            group_recipe.classes, key, here, true);
    set_attr(group, "data-aui-widget", "input");
    set_attr(group, "data-aui-type", input_type);

    const auto label_recipe = default_element(theme_, FrameworkElement::FieldLabel);
    auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                 label_recipe.classes, "__label", here, false);
    set_text(label_node, label);

    if (theme_ == ViewTheme::Decius && input_type == "number") {
        const double numeric_value = parse_double_or(value, 0.0);
        auto& combo = open_node(WidgetKind::Container, "div", "dcs-combo",
                                "__combo", here, true);
        set_attr(combo, "role", "spinbutton");
        set_attr(combo, "aria-valuenow", number(numeric_value));
        set_attr(combo, "data-dcs-combo", "");
        set_attr(combo, "data-value", number(numeric_value));
        set_attr(combo, "data-step", "0.01");
        set_attr(combo, "style", "--fill:" + percent(0.5));

        open_node(WidgetKind::Container, "div", "dcs-combo__fill", "__fill",
                  here, false);

        const auto input_recipe = default_element(theme_, FrameworkElement::TextInput);
        (void) input_recipe;
        auto& input_node = open_node(WidgetKind::TextInput, "input",
                                     "dcs-combo__value", "__input", here,
                                     false);
        set_attr(input_node, "type", "number");
        set_attr(input_node, "value", value);
        set_attr(input_node, "data-fill-min", number(numeric_value - 1.0));
        set_attr(input_node, "data-fill-max", number(numeric_value + 1.0));
        if (!key.empty()) set_attr(input_node, "data-aui-name", key);
        close_node();
        close_node();
        return ref_for_node(group, current_panel_id(stack_));
    }

    if (theme_ == ViewTheme::Decius && input_type == "color") {
        auto& color = open_node(WidgetKind::Container, "div", "dcs-colorfield",
                                "__colorfield", here, true);
        const std::string field_id =
            "aui-colorfield-" + dom_id_fragment(key.empty() ? color.remote_id
                                                            : std::string(key));
        const std::string menu_id = field_id + "-menu";
        set_attr(group, "aria-expanded", "false");
        set_attr(group, "data-dcs-toggle", "menu");
        set_attr(group, "data-dcs-target", "#" + menu_id);
        set_attr(color, "id", field_id);
        if (!key.empty()) set_attr(color, "data-aui-name", key);
        set_attr(color, "role", "button");
        set_attr(color, "aria-expanded", "false");
        set_attr(color, "data-dcs-toggle", "menu");
        set_attr(color, "data-dcs-target", "#" + menu_id);
        set_attr(color, "data-value", value);

        auto& chip = open_node(WidgetKind::Container, "div",
                               "dcs-colorfield__chip", "__chip", here,
                               false);
        set_attr(chip, "style", "--c:" + std::string(value) +
                                ";background:" + std::string(value));

        auto& hex = open_node(WidgetKind::Container, "span",
                              "dcs-colorfield__hex", "__hex", here, false);
        set_text(hex, value);

        auto& caret = open_node(WidgetKind::Container, "span",
                                "dcs-colorfield__caret di di-chevron-down",
                                "__caret", here, false);
        (void) caret;

        close_node();

        auto& menu = open_node(WidgetKind::Container, "div",
                               "dcs-menu aui-color-menu", "__menu", here,
                               true);
        set_attr(menu, "id", menu_id);
        set_attr(menu, "hidden", "");
        set_attr(menu, "data-aui-colorfield", field_id);

        std::vector<std::string> palette{
            std::string(value), "#3bb7ff", "#33aaff", "#3dd68a",
            "#ff8a3a", "#8b6dff", "#cf6b3a", "#3a3d45",
        };
        std::sort(palette.begin(), palette.end());
        palette.erase(std::unique(palette.begin(), palette.end()),
                      palette.end());
        for (const auto& swatch : palette) {
            auto& option = open_node(
                WidgetKind::Button, "button", "dcs-menu__item aui-color-option",
                std::string{"__color_"} + dom_id_fragment(swatch), here, true);
            set_attr(option, "type", "button");
            set_attr(option, "role", "menuitem");
            set_attr(option, "value", swatch);
            set_attr(option, "data-dcs-value", swatch);
            if (swatch == value) set_attr(option, "aria-selected", "true");

            auto& swatch_node = open_node(
                WidgetKind::Container, "span", "aui-color-option__swatch",
                "__swatch", here, false);
            set_attr(swatch_node, "style", "--c:" + swatch +
                                      ";background:" + swatch);

            auto& swatch_label = open_node(WidgetKind::Container, "span",
                                           "dcs-menu__label-text", "__label",
                                           here, false);
            set_text(swatch_label, swatch);
            close_node();
        }
        close_node();
        close_node();
        return ref_for_node(group, current_panel_id(stack_));
    }

    const auto input_recipe = default_element(theme_, FrameworkElement::TextInput);
    std::string input_classes{input_recipe.classes};
    if (theme_ == ViewTheme::Bootstrap && input_type == "color") {
        input_classes += " form-control-color";
    } else if (theme_ == ViewTheme::Decius && input_type == "number") {
        input_classes += " dcs-input--num";
    }
    auto& input_node = open_node(WidgetKind::TextInput, input_recipe.tag,
                                 input_classes, "__input", here, false);
    set_attr(input_node, "type", input_type);
    set_attr(input_node, "value", value);
    if (input_type == "number") set_attr(input_node, "style", "cursor:ew-resize");
    if (!key.empty()) set_attr(input_node, "data-aui-name", key);
    close_node();
    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::password(std::string_view label,
                         std::string_view value,
                         std::string_view key,
                         std::source_location here) {
    return input(label, value, "password", key, here);
}

WidgetRef View::textarea(std::string_view label,
                         std::string_view value,
                         int rows,
                         std::string_view key,
                         std::source_location here) {
    const auto group_recipe = default_element(theme_, FrameworkElement::FieldGroup);
    auto& group = open_node(WidgetKind::TextArea, group_recipe.tag,
                            group_recipe.classes, key, here, true);
    set_attr(group, "data-aui-widget", "textarea");

    const auto label_recipe = default_element(theme_, FrameworkElement::FieldLabel);
    auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                 label_recipe.classes, "__label", here, false);
    set_text(label_node, label);

    const auto text_recipe = default_element(theme_, FrameworkElement::TextArea);
    auto& text_node = open_node(WidgetKind::TextArea, text_recipe.tag,
                                text_recipe.classes, "__input", here, false);
    set_attr(text_node, "rows", std::to_string(std::max(rows, 1)));
    set_attr(text_node, "style", "resize:both;text-align:left");
    if (!key.empty()) set_attr(text_node, "data-aui-name", key);
    set_text(text_node, value);
    close_node();
    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::dropdown(std::string_view label,
                         const std::vector<std::string>& options,
                         std::string_view selected,
                         std::string_view key,
                         std::source_location here) {
    const auto group_recipe = default_element(theme_, FrameworkElement::FieldGroup);
    auto& group = open_node(WidgetKind::Dropdown, group_recipe.tag,
                            group_recipe.classes, key, here, true);
    set_attr(group, "data-aui-widget", "dropdown");
    set_attr(group, "data-value", selected);

    const auto label_recipe = default_element(theme_, FrameworkElement::FieldLabel);
    auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                 label_recipe.classes, "__label", here, false);
    set_text(label_node, label);

    auto& select_shell = open_node(WidgetKind::Container, "div",
                                   "aui-select", "__select-shell", here,
                                   true);
    (void) select_shell;

    const auto select_recipe = default_element(theme_, FrameworkElement::SelectInput);
    auto& select_node = open_node(WidgetKind::Dropdown, select_recipe.tag,
                                  select_recipe.classes, "__select", here, true);
    set_attr(select_node, "value", selected);
    if (!key.empty()) set_attr(select_node, "data-aui-name", key);
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto option_key = "__option-" + std::to_string(i);
        auto& option = open_node(WidgetKind::Container, "option", {},
                                 option_key, here, false);
        set_attr(option, "value", options[i]);
        if (options[i] == selected) set_attr(option, "selected", "selected");
        else remove_attr(option, "selected");
        set_text(option, options[i]);
    }
    close_node();

    std::string menu_classes{"aui-select__menu"};
    std::string item_classes{"aui-select__item"};
    if (theme_ == ViewTheme::Bootstrap) {
        menu_classes = "dropdown-menu aui-select__menu";
        item_classes = "dropdown-item";
    } else if (theme_ == ViewTheme::Decius) {
        menu_classes = "dcs-menu aui-select__menu";
        item_classes = "dcs-menu__item";
    }
    auto& menu = open_node(WidgetKind::Container, "div", menu_classes,
                           "__menu", here, true);
    set_attr(menu, "hidden", "");
    set_attr(menu, "role", "listbox");
    for (std::size_t i = 0; i < options.size(); ++i) {
        const bool active = options[i] == selected;
        std::string option_classes{item_classes};
        if (active) {
            if (theme_ == ViewTheme::Bootstrap) option_classes += " active";
            else if (theme_ == ViewTheme::Decius) option_classes += " dcs-menu__item--active";
        }
        const auto option_key = "__menu-option-" + std::to_string(i);
        auto& option = open_node(WidgetKind::Button, "button", option_classes,
                                 option_key, here, false);
        set_attr(option, "type", "button");
        set_attr(option, "role", "option");
        set_attr(option, "value", options[i]);
        if (active) set_attr(option, "aria-selected", "true");
        else remove_attr(option, "aria-selected");
        set_text(option, options[i]);
    }
    close_node();
    close_node();
    close_node();
    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::button_group(std::string_view label,
                             const std::vector<std::string>& options,
                             std::string_view selected,
                             std::string_view key,
                             std::source_location here) {
    const char* field_classes =
        theme_ == ViewTheme::Decius ? "dcs-field" :
        theme_ == ViewTheme::Bootstrap ? "aui-bs-field" : "";
    auto& field = open_node(WidgetKind::ButtonGroup, "div", field_classes,
                            key, here, true);
    set_attr(field, "data-aui-widget", "button-group");
    set_attr(field, "data-value", selected);

    const auto label_recipe = default_element(theme_, FrameworkElement::FieldLabel);
    auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                 label_recipe.classes, "__label", here, false);
    set_text(label_node, label);

    const auto group_recipe = default_element(theme_, FrameworkElement::ButtonGroup);
    auto& group = open_node(WidgetKind::ButtonGroup, group_recipe.tag,
                            group_recipe.classes, "__group", here, true);
    (void) group;
    for (std::size_t i = 0; i < options.size(); ++i) {
        const bool active = options[i] == selected;
        const auto button_recipe =
            default_element(theme_, FrameworkElement::ButtonGroupButton, active);
        const auto option_key = "__option-" + std::to_string(i);
        auto& button = open_node(WidgetKind::Button, button_recipe.tag,
                                 button_recipe.classes, option_key, here, false);
        set_attr(button, "type", "button");
        set_attr(button, "value", options[i]);
        if (active) set_attr(button, "aria-pressed", "true");
        else remove_attr(button, "aria-pressed");
        set_text(button, options[i]);
    }
    close_node();
    close_node();
    return ref_for_node(field, current_panel_id(stack_));
}

WidgetRef View::virtual_list(
        std::string_view key,
        const VirtualListOptions& options,
        const std::function<void(View&, std::size_t)>& build_item,
        std::string_view classes,
        std::source_location here) {
    std::string list_classes{"aui-virtual-list"};
    if (!classes.empty()) {
        list_classes += ' ';
        list_classes += classes;
    }

    auto& list = open_node(WidgetKind::VirtualList, "div", list_classes,
                           key, here, true);
    set_attr(list, "data-aui-widget", "virtual-list");
    set_attr(list, "role", "list");
    set_attr(list, "data-item-count", std::to_string(options.item_count));
    set_attr(list, "data-first-item", std::to_string(options.first_item));
    set_attr(list, "data-visible-items", std::to_string(options.visible_items));
    set_attr(list, "data-overscan", std::to_string(options.overscan));
    set_attr(list, "data-item-size", number(std::max(1.0, options.item_size)));

    const std::size_t count = options.item_count;
    const std::size_t first = std::min(options.first_item, count);
    const std::size_t visible = std::min(options.visible_items, count - first);
    const std::size_t start = first > options.overscan
        ? first - options.overscan
        : 0;
    const std::size_t end = std::min(
        count,
        first + visible + options.overscan);
    const double viewport_height =
        virtual_item_offset(options, first + visible) -
        virtual_item_offset(options, first);
    if (viewport_height > 0.0) {
        set_attr(list, "style", "height:" + px(viewport_height));
    } else {
        remove_attr(list, "style");
    }

    auto& before = open_node(WidgetKind::Container, "div",
                             "aui-virtual-list__spacer",
                             "__before", here, false);
    set_attr(before, "style", "height:" + px(virtual_item_offset(options, start)));

    for (std::size_t i = start; i < end; ++i) {
        const std::string row_key = "__row-" + std::to_string(i);
        auto& row = open_node(WidgetKind::Container, "div",
                              "aui-virtual-list__row", row_key, here, true);
        set_attr(row, "role", "listitem");
        set_attr(row, "data-index", std::to_string(i));
        set_attr(row, "style",
                 "min-height:" + px(virtual_item_size(options, i)));

        const auto stack_size = stack_.size();
        if (build_item) build_item(*this, i);
        while (stack_.size() > stack_size) close_node();
        close_node();
    }

    auto& after = open_node(WidgetKind::Container, "div",
                            "aui-virtual-list__spacer",
                            "__after", here, false);
    const double total_height = virtual_item_offset(options, count);
    set_attr(after, "style",
             "height:" + px(std::max(0.0, total_height -
                                      virtual_item_offset(options, end))));

    close_node();
    return ref_for_node(list, current_panel_id(stack_));
}

WidgetRef View::slider(std::string_view label,
                       double value,
                       double min,
                       double max,
                       std::string_view key,
                       std::source_location here) {
    const auto group_recipe = default_element(theme_, FrameworkElement::SliderGroup);
    auto& group = open_node(WidgetKind::Slider, group_recipe.tag,
                            group_recipe.classes,
                            key, here, true);
    set_attr(group, "data-aui-widget", "slider");
    set_attr(group, "role", "slider");
    set_attr(group, "aria-valuemin", number(min));
    set_attr(group, "aria-valuemax", number(max));
    set_attr(group, "aria-valuenow", number(value));

    const auto label_recipe = default_element(theme_, FrameworkElement::SliderLabel);
    auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                 label_recipe.classes, "__label", here, false);
    set_text(label_node, label);

    const auto input_recipe = default_element(theme_, FrameworkElement::SliderInput);
    auto& input = open_node(WidgetKind::Slider, input_recipe.tag,
                            input_recipe.classes,
                            "__input", here, theme_ == ViewTheme::Decius);
    set_attr(input, "min", number(min));
    set_attr(input, "max", number(max));
    set_attr(input, "value", number(value));
    if (theme_ == ViewTheme::Decius) {
        const double pos = normalized_value(value, min, max);
        set_attr(input, "data-dcs-slider", "");
        set_attr(input, "data-min", number(min));
        set_attr(input, "data-max", number(max));
        set_attr(input, "data-value", number(value));

        auto& track = open_node(WidgetKind::Container, "div",
                                "dcs-slider__track", "__track", here, true);
        (void) track;
        auto& fill = open_node(WidgetKind::Container, "div",
                               "dcs-slider__fill", "__fill", here, false);
        set_attr(fill, "style", "width:" + percent(pos));
        auto& thumb = open_node(WidgetKind::Container, "div",
                                "dcs-slider__thumb", "__thumb", here, false);
        set_attr(thumb, "style", "left:" + percent(pos));
        close_node();
        close_node();
    } else {
        set_attr(input, "type", "range");
    }
    close_node();
    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::knob(std::string_view label,
                     double value,
                     double min,
                     double max,
                     bool bipolar,
                     std::string_view key,
                     std::source_location here) {
    if (max <= min) max = min + 1.0;
    const double clamped = std::clamp(value, min, max);

    const auto group_recipe = default_element(theme_, FrameworkElement::KnobGroup);
    auto& group = open_node(WidgetKind::Knob, group_recipe.tag,
                            group_recipe.classes, key, here,
                            true);
    set_attr(group, "data-aui-widget", "knob");

    if (theme_ != ViewTheme::Decius) {
        WidgetNode* control = &group;
        if (theme_ == ViewTheme::Bootstrap) {
            const auto label_recipe =
                default_element(theme_, FrameworkElement::KnobLabel);
            auto& label_node = open_node(WidgetKind::Container,
                                         label_recipe.tag,
                                         label_recipe.classes,
                                         "__label", here, false);
            set_text(label_node, label);
            control = &open_node(WidgetKind::Knob, "div", "aui-knob",
                                 "__knob", here, true);
            if (!key.empty()) set_attr(*control, "data-aui-name", key);
        } else {
            set_attr(*control, "class", "aui-knob");
        }
        set_attr(*control, "data-aui-widget", "knob");
        set_attr(*control, "role", "slider");
        set_attr(*control, "aria-valuemin", number(min));
        set_attr(*control, "aria-valuemax", number(max));
        set_attr(*control, "aria-valuenow", number(clamped));
        set_attr(*control, "data-aui-knob", "");
        set_attr(*control, "data-min", number(min));
        set_attr(*control, "data-max", number(max));
        set_attr(*control, "data-value", number(clamped));
        set_attr(*control, "value", number(clamped));
        if (bipolar) set_attr(*control, "data-bipolar", "");
        else remove_attr(*control, "data-bipolar");

        const auto [bg_x0, bg_y0] = knob_ring_point(-225.0);
        const auto [bg_x1, bg_y1] = knob_ring_point(45.0);

        auto& svg = open_node(WidgetKind::Container, "svg", "aui-knob__ring",
                              "__ring", here, true);
        set_attr(svg, "viewBox", "0 0 24 24");

        auto& bg = open_node(WidgetKind::Container, "path", {}, "__ring-bg",
                             here, false);
        set_attr(bg, "d", "M " + number(bg_x0) + " " + number(bg_y0) +
                         " A 10.5 10.5 0 1 1 " + number(bg_x1) + " " +
                         number(bg_y1));
        set_attr(bg, "fill", "none");
        set_attr(bg, "stroke", "rgba(108,117,125,.35)");
        set_attr(bg, "stroke-width", "1.5");
        set_attr(bg, "stroke-linecap", "round");

        auto& arc = open_node(WidgetKind::Container, "path", "aui-knob__arc",
                              "__arc", here, false);
        const auto path = knob_arc_path(min, max, clamped, bipolar);
        if (!path.empty()) set_attr(arc, "d", path);
        else remove_attr(arc, "d");
        set_attr(arc, "fill", "none");
        set_attr(arc, "stroke", "#0d6efd");
        set_attr(arc, "stroke-width", "1.75");
        set_attr(arc, "stroke-linecap", "round");
        close_node();

        auto& cap = open_node(WidgetKind::Container, "div",
                              "aui-knob__cap", "__cap", here, false);
        (void) cap;
        auto& indicator = open_node(WidgetKind::Container, "div",
                                    "aui-knob__indicator", "__indicator",
                                    here, false);
        set_attr(indicator, "style",
                 "--angle:" + number(knob_angle(clamped, min, max)) + "deg");
        auto& value_node = open_node(WidgetKind::Container, "div",
                                     "aui-knob__value", "__value", here, false);
        set_text(value_node, number(clamped));
        auto& label_node = open_node(WidgetKind::Container, "div",
                                     "aui-knob__label", "__label", here, false);
        set_text(label_node, label);
        close_node();
        if (theme_ == ViewTheme::Bootstrap) {
            close_node();
        }
        return ref_for_node(group, current_panel_id(stack_));
    }

    const auto label_recipe = default_element(theme_, FrameworkElement::KnobLabel);
    auto& field_label = open_node(WidgetKind::Container, label_recipe.tag,
                                  label_recipe.classes, "__label", here, false);
    set_text(field_label, label);

    const auto input_recipe = default_element(theme_, FrameworkElement::KnobInput);
    auto& knob = open_node(WidgetKind::Knob, input_recipe.tag,
                           input_recipe.classes, "__knob", here, true);
    set_attr(knob, "data-dcs-knob", "");
    set_attr(knob, "role", "slider");
    set_attr(knob, "aria-valuemin", number(min));
    set_attr(knob, "aria-valuemax", number(max));
    set_attr(knob, "aria-valuenow", number(clamped));
    set_attr(knob, "data-min", number(min));
    set_attr(knob, "data-max", number(max));
    set_attr(knob, "data-value", number(clamped));
    set_attr(knob, "value", number(clamped));
    if (!key.empty()) set_attr(knob, "data-aui-name", key);
    if (bipolar) set_attr(knob, "data-bipolar", "");
    else remove_attr(knob, "data-bipolar");

    const auto [bg_x0, bg_y0] = knob_ring_point(-225.0);
    const auto [bg_x1, bg_y1] = knob_ring_point(45.0);

    auto& svg = open_node(WidgetKind::Container, "svg", "dcs-knob__ring",
                          "__ring", here, true);
    set_attr(svg, "viewBox", "0 0 24 24");

    auto& bg = open_node(WidgetKind::Container, "path", {}, "__ring-bg",
                         here, false);
    set_attr(bg, "d", "M " + number(bg_x0) + " " + number(bg_y0) +
                     " A 10.5 10.5 0 1 1 " + number(bg_x1) + " " +
                     number(bg_y1));
    set_attr(bg, "fill", "none");
    set_attr(bg, "stroke", "rgba(255,255,255,.08)");
    set_attr(bg, "stroke-width", "1.5");
    set_attr(bg, "stroke-linecap", "round");

    auto& arc = open_node(WidgetKind::Container, "path", "dcs-knob__arc",
                          "__arc", here, false);
    const auto path = knob_arc_path(min, max, clamped, bipolar);
    if (!path.empty()) set_attr(arc, "d", path);
    else remove_attr(arc, "d");
    set_attr(arc, "fill", "none");
    set_attr(arc, "stroke", "var(--dcs-accent)");
    set_attr(arc, "stroke-width", "1.75");
    set_attr(arc, "stroke-linecap", "round");
    close_node();

    auto& cap = open_node(WidgetKind::Container, "div",
                          "dcs-knob__cap", "__cap", here, false);
    (void) cap;

    auto& indicator = open_node(WidgetKind::Container, "div",
                                "dcs-knob__indicator", "__indicator",
                                here, false);
    set_attr(indicator, "style",
             "--angle:" + number(knob_angle(clamped, min, max)) + "deg");

    auto& value_node = open_node(WidgetKind::Container, "div",
                                 "dcs-knob__value", "__value", here, false);
    set_text(value_node, number(clamped));

    close_node();
    close_node();

    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::container_ref(std::string_view classes,
                              std::string_view key,
                              std::source_location here) {
    auto& node = open_node(WidgetKind::Container, "div", classes, key, here, false);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::element_ref(std::string_view tag,
                            std::string_view classes,
                            std::string_view key,
                            std::source_location here) {
    if (tag.empty()) {
        diagnostics_.push_back("View::element_ref requires a non-empty tag");
        tag = "div";
    }
    auto& node = open_node(WidgetKind::Container, tag, classes, key, here, false);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::panel_ref(std::string_view key, std::source_location here) {
    const auto recipe = default_element(theme_, FrameworkElement::Panel);
    auto& node = open_node(WidgetKind::Panel, recipe.tag, recipe.classes,
                           key, here, false);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::find_widget(std::string_view name) {
    for (auto it = widget_names_.rbegin(); it != widget_names_.rend(); ++it) {
        if (it->first != name) continue;
        if (auto* node = find_id(it->second)) {
            return ref_for_node(*node, root_.id, name);
        }
    }
    auto* found = find_widget_node_under(root_.id, name);
    return found ? ref_for_node(*found, root_.id, name)
                 : WidgetRef{this, root_.id, {}, name};
}

std::vector<WidgetClickBinding> View::click_bindings() const {
    std::vector<WidgetClickBinding> out;
    out.reserve(click_handlers_.size());
    for (const auto& [id, handler] : click_handlers_) {
        const auto* node = find_id(id);
        if (!node || node->widget_name.empty() || !handler) continue;
        out.push_back({node->widget_name, handler});
    }
    return out;
}

std::vector<WidgetChangeBinding> View::change_bindings() const {
    std::vector<WidgetChangeBinding> out;
    out.reserve(change_handlers_.size());
    for (const auto& [id, handler] : change_handlers_) {
        const auto* node = find_id(id);
        if (!node || node->widget_name.empty() || !handler) continue;
        out.push_back({node->widget_name, handler});
    }
    return out;
}

const WidgetNode* View::find_remote(std::string_view id) const {
    return find_remote_impl(root_, id);
}

std::string View::to_html_fragment() const {
    std::string out;
    for (const auto& child : root_.children) append_node_html(child, out);
    return out;
}

std::string View::to_html_document() const {
    std::string out;
    out += "<!doctype html><html><head><meta charset=\"utf-8\">";
    out += theme_link(theme_);
    out += "<style>";
    out += command_widget_style();
    out += "</style>";
    out += "</head><body";
    out += body_attrs(theme_, document_attrs_);
    out += "><main id=\"aui-root\" class=\"aui-root\">";
    out += to_html_fragment();
    out += "</main></body></html>";
    return out;
}

WidgetNode& View::open_node(WidgetKind kind,
                            std::string_view tag,
                            std::string_view classes,
                            std::string_view key,
                            std::source_location here,
                            bool push_scope) {
    if (stack_.empty()) begin(static_cast<ViewSink*>(nullptr));
    auto* parent = stack_.back();
    const std::size_t index = parent->cursor++;
    StableId id = make_stable_id(parent->id, kind, key, here);
    if (!key.empty()) {
        for (std::size_t i = 0; i < index && i < parent->children.size(); ++i) {
            const auto& sibling = parent->children[i];
            if (sibling.id == id || sibling.widget_name == key) {
                id = make_duplicate_stable_id(id, index);
                break;
            }
        }
    }

    WidgetNode* node = nullptr;
    bool created = false;
    if (index < parent->children.size() &&
        parent->children[index].id == id &&
        parent->children[index].kind == kind &&
        parent->children[index].tag == tag) {
        node = &parent->children[index];
    } else {
        for (std::size_t i = index; i < parent->children.size(); ++i) {
            unregister_tree(parent->children[i]);
            emit_remove_tree(sink_, parent->children[i]);
        }
        parent->children.erase(parent->children.begin() +
                               static_cast<std::ptrdiff_t>(index),
                               parent->children.end());
        parent->children.push_back({});
        node = &parent->children.back();
        node->id = id;
        node->kind = kind;
        node->remote_id = remote_id(id);
        created = true;
    }

    node->tag = std::string(tag);
    if (public_widget_key(key) && (created || node->widget_name.empty())) {
        set_widget_name(*node, key);
    } else if (!public_widget_key(key) && !node->widget_name.empty()) {
        set_widget_name(*node, {});
    }
    node->cursor = 0;

    if (created && sink_) {
        if (kind == WidgetKind::Text) {
            sink_->create_text(*node, parent == &root_ ? nullptr : parent, index);
        } else if (kind == WidgetKind::RawHtml) {
            diagnostics_.push_back(
                "Raw HTML nodes are not represented in remote patch streams");
        } else {
            sink_->create_element(*node, parent == &root_ ? nullptr : parent, index);
        }
    }
    if (!classes.empty()) set_attr(*node, "class", classes);
    else remove_attr(*node, "class");

    auto adjusted_attrs = node->attrs;
    framework_for(theme_).adjust_widget_attrs(kind, adjusted_attrs);
    for (const auto& attr : adjusted_attrs) {
        const auto* existing = find_attr(node->attrs, attr.name);
        if (existing == nullptr || existing->value != attr.value) {
            set_attr(*node, attr.name, attr.value);
        }
    }

    if (push_scope) {
        stack_.push_back(node);
    } else if (!node->children.empty()) {
        clear_children(*node);
    }
    return *node;
}

void View::close_node() {
    if (stack_.size() <= 1) return;
    auto* node = stack_.back();
    for (std::size_t i = node->cursor; i < node->children.size(); ++i) {
        unregister_tree(node->children[i]);
        emit_remove_tree(sink_, node->children[i]);
    }
    node->children.erase(node->children.begin() +
                         static_cast<std::ptrdiff_t>(node->cursor),
                         node->children.end());
    stack_.pop_back();
}

void View::set_attr(WidgetNode& node,
                    std::string_view name,
                    std::string_view value) {
    auto it = std::find_if(node.attrs.begin(), node.attrs.end(),
        [&](const WidgetAttribute& attr) { return attr.name == name; });
    if (it != node.attrs.end()) {
        if (it->value == value) return;
        it->value = std::string(value);
    } else {
        node.attrs.push_back({std::string(name), std::string(value)});
    }
    if (auto* sink = current_sink()) sink->set_attribute(node, name, value);
}

void View::set_selector(WidgetNode& node,
                        std::string_view name,
                        std::string_view value) {
    const auto key = normalized_selector_key(name);
    if (key.empty() || selector_attr_name(key).empty()) {
        diagnostics_.push_back("Invalid selector name: " + std::string(name));
        return;
    }

    auto adjusted_attrs = node.attrs;
    framework_for(theme_).apply_selector_attrs(key, value, adjusted_attrs);
    for (const auto& attr : adjusted_attrs) {
        const auto* existing = find_attr(node.attrs, attr.name);
        if (existing == nullptr || existing->value != attr.value) {
            set_attr(node, attr.name, attr.value);
        }
    }
}

void View::remove_attr(WidgetNode& node, std::string_view name) {
    auto it = std::find_if(node.attrs.begin(), node.attrs.end(),
        [&](const WidgetAttribute& attr) { return attr.name == name; });
    if (it == node.attrs.end()) return;
    node.attrs.erase(it);
    if (auto* sink = current_sink()) sink->remove_attribute(node, name);
}

void View::set_text(WidgetNode& node, std::string_view value) {
    if (node.text == value) return;
    node.text = std::string(value);
    if (node.kind == WidgetKind::RawHtml) return;
    if (auto* sink = current_sink()) sink->set_text(node, value);
}

void View::set_click_handler(WidgetNode& node, std::function<void()> cb) {
    auto it = std::find_if(click_handlers_.begin(), click_handlers_.end(),
        [&](const auto& entry) { return entry.first == node.id; });
    if (it != click_handlers_.end()) {
        it->second = std::move(cb);
    } else {
        click_handlers_.push_back({node.id, std::move(cb)});
    }
}

void View::set_change_handler(WidgetNode& node,
                              std::function<void(std::string_view)> cb) {
    auto it = std::find_if(change_handlers_.begin(), change_handlers_.end(),
        [&](const auto& entry) { return entry.first == node.id; });
    if (it != change_handlers_.end()) {
        it->second = std::move(cb);
    } else {
        change_handlers_.push_back({node.id, std::move(cb)});
    }
}

void View::clear_children(WidgetNode& node) {
    for (const auto& child : node.children) {
        unregister_tree(child);
        emit_remove_tree(current_sink(), child);
    }
    node.children.clear();
    node.cursor = 0;
}

void View::set_widget_name(WidgetNode& node, std::string_view name) {
    const std::string old_name = node.widget_name;
    if (!old_name.empty()) {
        widget_names_.erase(
            std::remove_if(widget_names_.begin(), widget_names_.end(),
                [&](const auto& entry) {
                    return entry.first == old_name && entry.second == node.id;
                }),
            widget_names_.end());
    }

    node.widget_name = std::string(name);
    if (!node.widget_name.empty()) {
        auto it = std::find_if(widget_names_.begin(), widget_names_.end(),
            [&](const auto& entry) {
                return entry.first == node.widget_name;
            });
        if (it != widget_names_.end()) {
            if (it->second != node.id || find_id(it->second) != &node) {
                diagnostics_.push_back("Duplicate widget id: " + node.widget_name);
            }
            it->second = node.id;
        } else {
            widget_names_.push_back({node.widget_name, node.id});
        }
        set_attr(node, "data-aui-name", node.widget_name);
    } else {
        remove_attr(node, "data-aui-name");
    }
}

void View::unregister_tree(const WidgetNode& node) {
    click_handlers_.erase(
        std::remove_if(click_handlers_.begin(), click_handlers_.end(),
            [&](const auto& entry) {
                return entry.first == node.id;
            }),
        click_handlers_.end());
    change_handlers_.erase(
        std::remove_if(change_handlers_.begin(), change_handlers_.end(),
            [&](const auto& entry) {
                return entry.first == node.id;
            }),
        change_handlers_.end());
    if (!node.widget_name.empty()) {
        widget_names_.erase(
            std::remove_if(widget_names_.begin(), widget_names_.end(),
                [&](const auto& entry) {
                    return entry.first == node.widget_name && entry.second == node.id;
                }),
            widget_names_.end());
    }
    for (const auto& child : node.children) unregister_tree(child);
}

bool View::can_mutate_children(std::string_view operation) {
    if (!reconciling_) return true;
    std::string message{"Illegal WidgetRef::"};
    message += operation;
    message += " during view generation";
    diagnostics_.push_back(std::move(message));
    return false;
}

void View::build_children(WidgetNode& node,
                          const std::function<void(View&)>& build,
                          bool replace) {
    if (!build) return;
    if (replace) clear_children(node);

    const auto old_stack = std::move(stack_);
    const bool old_reconciling = reconciling_;
    ViewSink* old_sink = sink_;

    reconciling_ = false;
    sink_ = nullptr;
    stack_.clear();
    node.cursor = node.children.size();
    stack_.push_back(&root_);
    stack_.push_back(&node);

    build(*this);

    while (stack_.size() > 2) close_node();
    if (stack_.size() == 2) stack_.pop_back();
    if (stack_.size() == 1) stack_.pop_back();

    stack_ = old_stack;
    reconciling_ = old_reconciling;
    sink_ = old_sink;
}

WidgetNode* View::find_id(StableId id) {
    return find_id_impl(root_, id);
}

const WidgetNode* View::find_id(StableId id) const {
    return find_id_impl(root_, id);
}

WidgetRef View::ref_for_node(const WidgetNode& node,
                             StableId panel_id,
                             std::string_view name) {
    const std::string_view ref_name = !name.empty() ? name : node.widget_name;
    if (ref_name.empty()) return {};
    return WidgetRef{this, panel_id, node.id, ref_name};
}

WidgetNode* View::find_widget_node_under(StableId root_id, std::string_view name) {
    auto* root = find_id(root_id);
    if (!root) return nullptr;
    auto* found = find_widget_impl(*root, name);
    return found ? found : nullptr;
}

WidgetNode* View::resolve_widget_ref(const WidgetRef& ref) {
    auto* node = find_id(ref.id_);
    if (node != nullptr) return node;
    if (ref.name_.empty()) return nullptr;

    if (auto* found = find_widget_node_under(ref.panel_id_, ref.name_)) {
        ref.id_ = found->id;
        return found;
    }
    if (auto* found = find_widget_node_under(root_.id, ref.name_)) {
        ref.id_ = found->id;
        return found;
    }
    return nullptr;
}

ViewSink* View::current_sink() const noexcept {
    return reconciling_ ? sink_ : mutation_sink_;
}

}  // namespace affineui
