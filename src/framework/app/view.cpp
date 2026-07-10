#include "affineui/view.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    // snprintf, not std::to_chars: Apple's libc++ gates the
    // floating-point to_chars overload to the macOS 13.3 SDK, and the
    // wheel deployment target (11.0) doesn't have it.
    char buf[64]{};
    const int n = std::snprintf(buf, sizeof(buf), "%g", value);
    if (n > 0 && n < static_cast<int>(sizeof(buf))) return std::string(buf, n);
    std::ostringstream out;
    out << value;
    return out.str();
}

double normalized_value(double value, double min, double max) {
    if (max <= min) return 0.0;
    return std::clamp((value - min) / (max - min), 0.0, 1.0);
}

double parse_double_or(std::string_view text, double fallback) {
    // strtod, not std::from_chars: Apple's libc++ leaves the
    // floating-point from_chars overload `= delete`'d.
    if (text.empty()) return fallback;
    std::string tmp(text);
    char* end = nullptr;
    errno = 0;
    double value = std::strtod(tmp.c_str(), &end);
    return (end != tmp.c_str() && errno != ERANGE) ? value : fallback;
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

// (Knob ring/arc/indicator geometry moved to the engine's UA knob
// chrome painter in document.cpp — the DOM no longer carries the SVG,
// so the builder-side path helpers that produced it are gone.)

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
    // General app-shell / data components (themed structural widgets).
    Toolbar,
    ToolbarSeparator,
    IconButton,
    Menubar,
    MenubarItem,
    Menu,
    MenuItem,
    DockHost,
    DockPanel,
    DockPanelTab,
    DockPanelBody,
    Splitter,
    Tree,
    TreeRow,
    List,
    ListItem,
    Table,
    Foldout,
    FoldoutHeader,
    FoldoutBody,
    Statusbar,
    StatusbarItem,
    Notification,
};

struct ElementRecipe {
    std::string_view tag;
    std::string_view classes;
};

// The "personality module": translates semantic widgets (FrameworkElement)
// into concrete DOM/CSS for one CSS framework, and owns that framework's
// version (which both names its stylesheet bundle and lets it branch markup
// across framework versions). Cross-framework components go through this;
// components clearly specific to one framework may emit their classes directly
// instead of adding recipes to every personality.
class ViewFramework {
public:
    virtual ~ViewFramework() = default;

    /// Default stylesheet href (for the personality's default version).
    [[nodiscard]] virtual std::string_view stylesheet_href() const noexcept = 0;
    /// The framework version this personality ships with / targets by default.
    [[nodiscard]] virtual std::string_view default_version() const noexcept = 0;
    /// A short framework id stamped on the document root (e.g. "decius",
    /// "bootstrap", "") so the interaction layer can tell personalities apart.
    [[nodiscard]] virtual std::string_view framework_id() const noexcept = 0;
    /// Format the stylesheet bundle href for a specific version. Empty when the
    /// personality has no external stylesheet (Plain).
    [[nodiscard]] virtual std::string bundle_href(
        std::string_view version) const = 0;
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
    [[nodiscard]] std::string_view default_version() const noexcept override {
        return {};
    }
    [[nodiscard]] std::string_view framework_id() const noexcept override {
        return {};
    }
    [[nodiscard]] std::string bundle_href(std::string_view) const override {
        return {};  // Plain has no external stylesheet.
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
    [[nodiscard]] std::string_view default_version() const noexcept override {
        return bootstrap::default_version;
    }
    [[nodiscard]] std::string_view framework_id() const noexcept override {
        return "bootstrap";
    }
    [[nodiscard]] std::string bundle_href(std::string_view version) const override {
        return "frameworks/css/bootstrap-" + std::string(version) + ".min.css";
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
            case FrameworkElement::Toolbar:          return {"div", "btn-toolbar gap-2 align-items-center"};
            case FrameworkElement::ToolbarSeparator: return {"div", "vr"};
            case FrameworkElement::IconButton:       return {"button", "btn btn-light btn-sm"};
            case FrameworkElement::Menubar:          return {"nav", "navbar navbar-expand"};
            case FrameworkElement::MenubarItem:      return {"button", "btn btn-link nav-link"};
            case FrameworkElement::Menu:             return {"div", "dropdown-menu"};
            case FrameworkElement::MenuItem:         return {"button", "dropdown-item"};
            case FrameworkElement::DockHost:         return {"div", "d-flex flex-column"};
            case FrameworkElement::DockPanel:        return {"section", "card"};
            case FrameworkElement::DockPanelTab:     return {"button", "nav-link active"};
            case FrameworkElement::DockPanelBody:    return {"div", "card-body"};
            case FrameworkElement::Splitter:         return {"div", "aui-bs-splitter"};
            case FrameworkElement::Tree:             return {"div", "list-group"};
            case FrameworkElement::TreeRow:          return {"button", "list-group-item list-group-item-action"};
            case FrameworkElement::List:             return {"div", "list-group"};
            case FrameworkElement::ListItem:         return {"button", "list-group-item list-group-item-action"};
            case FrameworkElement::Table:            return {"table", "table"};
            case FrameworkElement::Foldout:          return {"div", "accordion-item"};
            case FrameworkElement::FoldoutHeader:    return {"button", "accordion-button"};
            case FrameworkElement::FoldoutBody:      return {"div", "accordion-body"};
            case FrameworkElement::Statusbar:        return {"div", "d-flex align-items-center gap-2 small text-body-secondary"};
            case FrameworkElement::StatusbarItem:    return {"span", ""};
            case FrameworkElement::Notification:     return {"div", "toast show"};
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
        return "frameworks/css/decius-css-0.6.2.bundle.min.css";
    }
    [[nodiscard]] std::string_view default_version() const noexcept override {
        return decius::default_version;
    }
    [[nodiscard]] std::string_view framework_id() const noexcept override {
        return "decius";
    }
    [[nodiscard]] std::string bundle_href(std::string_view version) const override {
        return "frameworks/css/decius-css-" + std::string(version) +
               ".bundle.min.css";
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
            case FrameworkElement::Toolbar:          return {"div", "dcs-toolbar"};
            case FrameworkElement::ToolbarSeparator: return {"span", "dcs-toolbar__sep"};
            case FrameworkElement::IconButton:
                return {"button", "dcs-btn dcs-btn--icon dcs-btn--ghost"};
            case FrameworkElement::Menubar:          return {"nav", "dcs-menubar"};
            case FrameworkElement::MenubarItem:      return {"button", "dcs-menubar__item"};
            case FrameworkElement::Menu:             return {"div", "dcs-menu"};
            case FrameworkElement::MenuItem:         return {"button", "dcs-menu__item"};
            case FrameworkElement::DockHost:         return {"div", "dcs-dock dcs-dock--v"};
            case FrameworkElement::DockPanel:        return {"section", "dcs-dockpane"};
            case FrameworkElement::DockPanelTab:     return {"button", "dcs-dockpane__tab"};
            case FrameworkElement::DockPanelBody:    return {"div", "dcs-dockpane__body"};
            case FrameworkElement::Splitter:         return {"div", "dcs-splitter"};
            case FrameworkElement::Tree:             return {"div", "dcs-tree"};
            case FrameworkElement::TreeRow:          return {"button", "dcs-tree__row"};
            case FrameworkElement::List:             return {"div", "dcs-list"};
            case FrameworkElement::ListItem:         return {"button", "dcs-list__item"};
            case FrameworkElement::Table:            return {"table", "dcs-table"};
            case FrameworkElement::Foldout:          return {"div", "dcs-foldout"};
            case FrameworkElement::FoldoutHeader:    return {"button", "dcs-foldout__header"};
            case FrameworkElement::FoldoutBody:      return {"div", "dcs-foldout__body"};
            case FrameworkElement::Statusbar:        return {"div", "dcs-statusbar"};
            case FrameworkElement::StatusbarItem:    return {"span", "dcs-statusbar__item"};
            case FrameworkElement::Notification:     return {"div", "dcs-toast"};
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

}  // namespace

namespace decius {
std::string bundle_href(std::string_view version) {
    return "frameworks/css/decius-css-" + std::string(version) +
           ".bundle.min.css";
}
}  // namespace decius

FrameworkVersion FrameworkVersion::parse(std::string_view text) noexcept {
    FrameworkVersion out;
    int* fields[] = {&out.major, &out.minor, &out.patch};
    int idx = 0;
    std::size_t i = 0;
    while (i < text.size() && idx < 3) {
        if (text[i] < '0' || text[i] > '9') {
            // Separators advance to the next field; anything else ends parsing.
            if (text[i] == '.') { ++idx; ++i; continue; }
            break;
        }
        int value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            value = value * 10 + (text[i] - '0');
            ++i;
        }
        *fields[idx] = value;
        if (i < text.size() && text[i] == '.') { ++idx; ++i; }
        else break;
    }
    return out;
}

namespace {

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

// Resolve the framework version to use: the View's explicit override if set,
// else the personality's default.
std::string resolved_version(ViewTheme theme, std::string_view override_version) {
    if (!override_version.empty()) return std::string(override_version);
    return std::string(framework_for(theme).default_version());
}

std::string theme_link(ViewTheme theme, std::string_view version) {
    const auto& framework = framework_for(theme);
    const std::string ver = resolved_version(theme, version);
    const std::string href =
        ver.empty() ? std::string(framework.stylesheet_href())
                    : framework.bundle_href(ver);
    if (href.empty()) return {};
    return "<link rel=\"stylesheet\" href=\"" + href + "\">";
}

std::vector<WidgetAttribute> document_attrs(
    ViewTheme theme,
    std::string_view version,
    const std::vector<WidgetAttribute>& explicit_attrs) {
    auto attrs = explicit_attrs;
    const auto& framework = framework_for(theme);
    framework.adjust_document_attrs(attrs);
    // Stamp the framework id + resolved version on the root so the interaction
    // layer (and version-branching personalities) can read them off the DOM.
    const auto fid = framework.framework_id();
    if (!fid.empty()) {
        set_attr(attrs, "data-aui-framework", std::string(fid));
        const std::string ver = resolved_version(theme, version);
        if (!ver.empty()) {
            set_attr(attrs, "data-aui-framework-version", ver);
        }
    }
    return attrs;
}

std::string body_attrs(ViewTheme theme,
                       std::string_view version,
                       const std::vector<WidgetAttribute>& explicit_attrs) {
    std::string out;
    append_attrs_html(document_attrs(theme, version, explicit_attrs), out);
    return out;
}

std::string command_widget_style() {
    std::string css;
    css.reserve(32768);
    css += R"CSS(
body{margin:0}
.aui-root{min-height:100vh;padding:24px;box-sizing:border-box;display:flex;flex-direction:column;align-items:stretch;gap:var(--dcs-s-3,16px)}
.aui-root.aui-root--shell{padding:0;gap:0}
.aui-root>.dcs-btn,.aui-root>.btn,.aui-root>.dcs-btn-group,.aui-root>.btn-group{align-self:flex-start}
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
.aui-select__menu.dcs-menu{max-width:none}
.aui-select__menu .dropdown-item,.aui-select__menu .dcs-menu__item{border:0;background:transparent;text-align:left;font:inherit;box-sizing:border-box;max-width:none}
.aui-select__menu .dropdown-item{width:100%}
.aui-select__menu .dcs-menu__item{align-self:stretch;width:auto}
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
.aui-root .dcs-field>.aui-select,.aui-root .dcs-field>.dcs-colorfield,.aui-root .dcs-field>.dcs-combo,.aui-root .dcs-field>.dcs-vec{flex:1 1 auto;min-width:0}
.aui-root .dcs-field>.dcs-btn-group{display:inline-flex;flex:0 0 auto;width:auto;min-width:0}
.aui-root .dcs-field>.dcs-btn-group>.dcs-btn{flex:0 0 auto;min-width:64px;white-space:nowrap}
.aui-root .dcs-combo>input.dcs-combo__value{background:transparent;border:0;outline:0;text-align:right;min-width:0;cursor:ew-resize}
.aui-root .dcs-vec{--dcs-xform-minwidth:72px;display:flex;gap:var(--dcs-s-1);column-gap:var(--dcs-s-1);row-gap:var(--dcs-s-1);min-width:0}
.aui-root .dcs-vec>*{flex:1 1 0;min-width:var(--dcs-xform-minwidth);min-height:var(--dcs-h-in)}
.aui-root .dcs-vec>.dcs-combo{min-width:var(--dcs-xform-minwidth)}
.aui-root .dcs-vec>.dcs-combo .dcs-combo__fill{display:none}
.aui-root .dcs-vec--stacked{flex-direction:column}
.aui-root .dcs-vec--stacked>*{min-width:0;width:100%}
.aui-root .dcs-field.dcs-field--vec{align-items:flex-start;height:auto;min-height:var(--dcs-h-in)}
.aui-root .dcs-field.dcs-field--vec>.dcs-field__label{padding-top:3px}
.aui-root .dcs-props>.dcs-field.dcs-field--vec-stacked{height:auto;min-height:var(--aui-vec-stack-min,var(--dcs-h-in))}
.aui-root .dcs-colorfield{position:relative}
.aui-root .dcs-colorfield>.dcs-colorfield__chip{border:0;padding:0;background:var(--c,#3bb7ff);color:transparent;appearance:none}
.aui-root .dcs-colorfield__picker{width:100%;min-width:188px;box-sizing:border-box;align-self:stretch;display:flex;flex-grow:0;flex-shrink:0;flex-direction:column;gap:8px}
.aui-root .dcs-colorfield__picker .dcs-color-square{width:100%;box-sizing:border-box;align-self:stretch;height:134px;flex-grow:0;flex-shrink:0}
.aui-root .dcs-colorfield__picker .dcs-hue-bar{width:100%;box-sizing:border-box;align-self:stretch;height:12px;flex-grow:0;flex-shrink:0}
.aui-root .dcs-colorfield__picker-row{display:flex;align-items:center;gap:6px;flex-grow:0;flex-shrink:0}
.aui-root .dcs-colorfield__picker-chip{width:22px;height:22px;flex:0 0 auto;border-radius:var(--dcs-r-1);background:var(--c,#4d9fff);box-shadow:inset 0 0 0 1px rgba(0,0,0,.35)}
.aui-root .dcs-colorfield__picker-input{flex:1 1 auto;min-width:0;height:var(--dcs-h-in)}
.aui-root .dcs-colorfield__picker-eyedropper{flex:0 0 auto;width:var(--dcs-h-in);height:var(--dcs-h-in);padding:0}
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

std::string framework_bundle_href(ViewTheme theme, std::string_view version) {
    const auto& framework = framework_for(theme);
    const std::string ver = resolved_version(theme, version);
    return ver.empty() ? std::string(framework.stylesheet_href())
                       : framework.bundle_href(ver);
}

std::string_view framework_default_version(ViewTheme theme) {
    return framework_for(theme).default_version();
}

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

std::string_view WidgetRef::attr_value(std::string_view name,
                                       std::string_view fallback) const {
    if (const auto* n = node()) {
        if (const auto* a = find_attr(n->attrs, name)) return a->value;
    }
    return fallback;
}

std::string_view WidgetRef::text_value() const {
    if (const auto* n = node()) return n->text;
    return {};
}

bool WidgetRef::has_attr(std::string_view name) const {
    if (const auto* n = node()) return find_attr(n->attrs, name) != nullptr;
    return false;
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

WidgetRef& WidgetRef::add_class(std::string_view token) {
    if (token.empty() || !owner_) return *this;
    auto* n = owner_->resolve_widget_ref(*this);
    if (!n) return *this;
    std::string_view current;
    if (const auto* a = find_attr(n->attrs, "class")) current = a->value;
    // Idempotent: skip if the token is already a whole class in the list.
    std::size_t pos = 0;
    while (pos < current.size()) {
        while (pos < current.size() && current[pos] == ' ') ++pos;
        const std::size_t start = pos;
        while (pos < current.size() && current[pos] != ' ') ++pos;
        if (current.substr(start, pos - start) == token) return *this;
    }
    std::string next(current);
    if (!next.empty()) next += ' ';
    next += token;
    owner_->set_attr(*n, "class", next);
    return *this;
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

WidgetRef& WidgetRef::on_commit(std::function<void(std::string_view)> cb) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->set_commit_handler(*n, std::move(cb));
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

View::Scope::Scope(View* owner, WidgetNode* node, std::size_t unwind_to) noexcept
    : owner_(owner), node_(node), unwind_to_(unwind_to) {}

View::Scope::~Scope() {
    if (owner_) owner_->close_to(unwind_to_);
}

View::Scope::Scope(Scope&& other) noexcept
    : owner_(other.owner_), node_(other.node_), unwind_to_(other.unwind_to_) {
    other.owner_ = nullptr;
    other.node_ = nullptr;
}

View::Scope& View::Scope::operator=(Scope&& other) noexcept {
    if (this == &other) return *this;
    if (owner_) owner_->close_to(unwind_to_);
    owner_ = other.owner_;
    node_ = other.node_;
    unwind_to_ = other.unwind_to_;
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
    root_app_shell_ = false;
    stack_.clear();
    widget_names_.clear();
    click_handlers_.clear();
    change_handlers_.clear();
    commit_handlers_.clear();
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
    root_app_shell_ = false;  // re-detected by this build's root declarations
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
    // Emit the coalesced attribute diffs: removed subtrees are already gone,
    // so only surviving nodes flush, and only their NET change vs the start
    // of this pass reaches the sink (WIDGET_RECONCILIATION.md §5.2).
    if (attr_coalesce_dirty_) {
        flush_attr_diffs(root_);
        attr_coalesce_dirty_ = false;
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
    return scope_here(node);
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
    return scope_here(node);
}

View::Scope View::panel(std::string_view key,
                        std::source_location here) {
    const auto recipe = default_element(theme_, FrameworkElement::Panel);
    auto& node = open_node(WidgetKind::Panel, recipe.tag, recipe.classes,
                           key, here, true);
    return scope_here(node);
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
    return scope_here(node);
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

WidgetRef View::toggle(std::string_view label, bool on, std::string_view key,
                       std::source_location here) {
    // Off-Decius personalities have no switch primitive; a checkbox is the
    // idiomatic on/off control there.
    if (theme_ != ViewTheme::Decius) {
        return checkbox(label, on, key, here);
    }

    const auto group_recipe = default_element(theme_, FrameworkElement::CheckboxGroup);
    auto& group = open_node(WidgetKind::Checkbox, group_recipe.tag,
                            group_recipe.classes, key, here, true);
    set_attr(group, "data-aui-widget", "toggle");
    if (on) set_attr(group, "aria-checked", "true");
    else remove_attr(group, "aria-checked");

    const auto label_recipe = default_element(theme_, FrameworkElement::CheckboxLabel);
    auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                 label_recipe.classes, "__label", here, false);
    set_text(label_node, label);

    // The switch track; the knob is the CSS ::after. The core interaction layer
    // treats .dcs-switch like a checkbox (it toggles aria-checked on press).
    auto& sw = open_node(WidgetKind::Checkbox, "div", "dcs-switch", "__switch",
                         here, true);
    set_attr(sw, "role", "switch");
    if (on) set_attr(sw, "aria-checked", "true");
    else remove_attr(sw, "aria-checked");

    auto& input = open_node(WidgetKind::Checkbox, "input", {}, "__input", here,
                            false);
    set_attr(input, "type", "checkbox");
    set_attr(input, "style", "display:none");
    if (on) set_attr(input, "checked", "checked");
    else remove_attr(input, "checked");

    close_node();  // switch
    close_node();  // group
    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::input(std::string_view label,
                      std::string_view value,
                      std::string_view type,
                      std::string_view key,
                      std::source_location here) {
    const std::string_view input_type = type.empty() ? "text" : type;
    if (theme_ == ViewTheme::Decius && input_type == "color") {
        return colorfield(label, value, key, here);
    }

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
    // No inline styles: the framework owns resize affordance per context
    // (decius: `resize:both` in forms, `resize:vertical` in props).
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
        menu_classes = "dcs-menu dcs-menu--select aui-select__menu";
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

        // Ring/arc/indicator are UA-painted from the data-* attrs above
        // (document.cpp knob chrome); only the styled cap and labels
        // live in the DOM.
        auto& cap = open_node(WidgetKind::Container, "div",
                              "aui-knob__cap", "__cap", here, false);
        (void) cap;
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

    // Ring, value arc, and indicator are UA-painted by the engine from
    // the data-* attrs above (document.cpp knob chrome — same tier as
    // checkbox/radio/switch). The DOM carries only the styled cap and
    // the value label: a knob move is ONE attribute write, with no SVG
    // path strings to build, diff, or reparse.
    auto& cap = open_node(WidgetKind::Container, "div",
                          "dcs-knob__cap", "__cap", here, false);
    (void) cap;

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

WidgetRef View::canvas(std::string_view paint_name,
                       std::string_view classes,
                       std::string_view key,
                       std::source_location here) {
    auto& node = open_node(WidgetKind::Container, "div", classes,
                           key.empty() ? paint_name : key, here, false);
    auto ref = ref_for_node(node, current_panel_id(stack_));
    if (!paint_name.empty()) ref.attr("data-aui-paint", paint_name);
    return ref;
}

WidgetRef View::panel_ref(std::string_view key, std::source_location here) {
    const auto recipe = default_element(theme_, FrameworkElement::Panel);
    auto& node = open_node(WidgetKind::Panel, recipe.tag, recipe.classes,
                           key, here, false);
    return ref_for_node(node, current_panel_id(stack_));
}

// ── App-shell / structural component builders ───────────────────────────────

View::Scope View::toolbar(std::string_view key, std::source_location here) {
    const auto r = default_element(theme_, FrameworkElement::Toolbar);
    auto& node = open_node(WidgetKind::Container, r.tag, r.classes, key, here, true);
    if (stack_.size() == 2) root_app_shell_ = true;  // root-level toolbar = app shell
    return scope_here(node);
}

View::Scope View::floating_toolbar(const FloatingToolbarOptions& opts,
                                   std::string_view key,
                                   std::source_location here) {
    std::string cls = "dcs-toolbar";
    cls += opts.vertical ? " dcs-toolbar--v" : " dcs-toolbar--h";
    if (opts.small) cls += " dcs-toolbar--sm";
    cls += " dcs-toolbar--floating";
    auto& node = open_node(WidgetKind::Container, "div", cls, key, here, true);
    if (!opts.position.empty()) set_attr(node, "style", opts.position);
    // Mark the container draggable (decius data-dcs-drag); the grip below is the
    // grab handle, and drag-bounds (if given) constrains movement.
    set_attr(node, "data-dcs-drag", "");
    if (!opts.drag_bounds.empty()) {
        set_attr(node, "data-dcs-drag-bounds", opts.drag_bounds);
    }
    // Grip drag handle (a horizontal grip for a vertical rail, and vice versa).
    auto& grip = open_node(WidgetKind::Container, "span",
                           opts.vertical ? "dcs-grip dcs-grip--h"
                                         : "dcs-grip dcs-grip--v",
                           "__grip", here, false);
    set_attr(grip, "data-dcs-drag-handle", "");
    return scope_here(node);
}

WidgetRef View::toolbar_separator(std::string_view key,
                                  std::source_location here) {
    const auto r = default_element(theme_, FrameworkElement::ToolbarSeparator);
    auto& node = open_node(WidgetKind::Container, r.tag, r.classes, key, here, false);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::icon_button(std::string_view icon,
                            std::string_view key,
                            std::source_location here) {
    const auto r = default_element(theme_, FrameworkElement::IconButton);
    auto& node = open_node(WidgetKind::Button, r.tag, r.classes, key, here, true);
    set_attr(node, "type", "button");
    // Icon glyph child (Decius icon font: <i class="di di-NAME">).
    auto& glyph = open_node(WidgetKind::Container, "i",
                            "di di-" + std::string(icon), "__icon", here, false);
    (void) glyph;
    close_node();
    return ref_for_node(node, current_panel_id(stack_));
}

View::Scope View::menu_bar(std::string_view key, std::source_location here) {
    const auto r = default_element(theme_, FrameworkElement::Menubar);
    auto& node = open_node(WidgetKind::Container, r.tag, r.classes, key, here, true);
    if (stack_.size() == 2) root_app_shell_ = true;
    return scope_here(node);
}

WidgetRef View::menu_button(std::string_view label,
                            std::string_view menu_id,
                            std::string_view key,
                            std::source_location here) {
    const auto r = default_element(theme_, FrameworkElement::MenubarItem);
    auto& node = open_node(WidgetKind::Button, r.tag, r.classes, key, here, false);
    set_attr(node, "type", "button");
    set_attr(node, "data-dcs-toggle", "menu");
    set_attr(node, "data-dcs-target", "#" + std::string(menu_id));
    set_text(node, label);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::menu_button(std::string_view label,
                            const std::function<void(View&)>& build,
                            std::string_view key, std::source_location here) {
    // The trigger button.
    const auto r = default_element(theme_, FrameworkElement::MenubarItem);
    auto& node = open_node(WidgetKind::Button, r.tag, r.classes, key, here, false);
    set_attr(node, "type", "button");
    const std::string menu_id =
        "aui-menu-" + dom_id_fragment(key.empty() ? node.remote_id
                                                  : std::string(key));
    set_attr(node, "data-dcs-toggle", "menu");
    set_attr(node, "data-dcs-target", "#" + menu_id);
    set_text(node, label);
    auto ref = ref_for_node(node, current_panel_id(stack_));
    // The dropdown menu, emitted as the trigger's sibling (positioned at the
    // trigger when opened). Linked by the generated id.
    menu(menu_id, build, here);
    return ref;
}

WidgetRef View::menu_brand(std::string_view title, std::string_view icon,
                           std::string_view key, std::source_location here) {
    const bool decius = theme_ == ViewTheme::Decius;
    auto& node = open_node(WidgetKind::Container, decius ? "div" : "span",
                           decius ? "dcs-menubar__brand" : "navbar-brand", key,
                           here, true);
    if (!icon.empty()) {
        open_node(WidgetKind::Container, "i", "di di-" + std::string(icon),
                  "__icon", here, false);
    }
    auto& label = open_node(WidgetKind::Container, "span", {}, "__title", here,
                            false);
    set_text(label, title);
    close_node();  // brand
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::menu_spacer(std::string_view key, std::source_location here) {
    const bool decius = theme_ == ViewTheme::Decius;
    auto& node = open_node(WidgetKind::Container, "div",
                           decius ? "dcs-menubar__spacer" : "ms-auto", key,
                           here, false);
    if (!decius) set_attr(node, "style", "flex:1");
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::menu_meta(std::string_view text, std::string_view key,
                          std::source_location here) {
    const bool decius = theme_ == ViewTheme::Decius;
    auto& node = open_node(WidgetKind::Container, "div",
                           decius ? "dcs-menubar__meta"
                                  : "navbar-text text-body-secondary",
                           key, here, false);
    set_text(node, text);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::menu(std::string_view id,
                     const std::function<void(View&)>& build,
                     std::source_location here) {
    auto& node = open_node(WidgetKind::Container, "div", "dcs-menu", id, here,
                           true);
    set_attr(node, "id", std::string(id));
    set_attr(node, "hidden", "");
    if (build) build(*this);
    close_node();
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::menu_item(std::string_view label, std::string_view icon,
                          std::string_view shortcut, std::string_view key,
                          std::source_location here) {
    auto& node = open_node(WidgetKind::Button, "div", "dcs-menu__item", key,
                           here, true);
    // Desktop-menu convention: every row reserves the 16px icon gutter
    // (`.dcs-menu__icon`), glyph or not, so labels stay column-aligned when
    // some rows carry icons/check marks and others don't.
    {
        auto& ic = open_node(WidgetKind::Container, "span", "dcs-menu__icon",
                             "__icon", here, true);
        (void) ic;
        if (!icon.empty()) {
            open_node(WidgetKind::Container, "i",
                      "di di-" + std::string(icon), "__icon-glyph", here,
                      false);
        }
        close_node();  // icon
    }
    auto& lbl = open_node(WidgetKind::Container, "span", "dcs-menu__label-text",
                          "__label", here, false);
    set_text(lbl, label);
    if (!shortcut.empty()) {
        auto& sc = open_node(WidgetKind::Container, "span", "dcs-menu__shortcut",
                             "__shortcut", here, false);
        set_text(sc, shortcut);
    }
    close_node();  // item
    return ref_for_node(node, current_panel_id(stack_));
}

View::Scope View::menu_item_custom(std::string_view key,
                                   std::source_location here) {
    // A menu row whose CONTENT the caller composes (swatch chips, custom
    // layouts, ...). Same kind + class as menu_item(), so the engine's menu
    // activation (hover highlight, click → select → close) and WidgetRef
    // on_click behave identically.
    auto& node = open_node(WidgetKind::Button, "div", "dcs-menu__item", key,
                           here, true);
    return scope_here(node);
}

WidgetRef View::menu_separator(std::string_view key, std::source_location here) {
    auto& node = open_node(WidgetKind::Container, "div", "dcs-menu__sep", key,
                           here, false);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::submenu(std::string_view label,
                        const std::function<void(View&)>& build,
                        std::string_view icon, std::string_view key,
                        std::source_location here) {
    auto& node = open_node(WidgetKind::Container, "div",
                           "dcs-menu__item dcs-menu__item--has-sub", key, here,
                           true);
    if (!icon.empty()) {
        auto& ic = open_node(WidgetKind::Container, "span", "dcs-menu__icon",
                             "__icon", here, true);
        (void) ic;
        open_node(WidgetKind::Container, "i", "di di-" + std::string(icon),
                  "__icon-glyph", here, false);
        close_node();  // icon
    }
    auto& lbl = open_node(WidgetKind::Container, "span", "dcs-menu__label-text",
                          "__label", here, false);
    set_text(lbl, label);
    // Caret marking the row as a submenu opener.
    {
        auto& caret = open_node(WidgetKind::Container, "span",
                                "dcs-menu__caret", "__caret", here, true);
        (void) caret;
        open_node(WidgetKind::Container, "i", "di di-chevron-right",
                  "__caret-glyph", here, false);
        close_node();  // caret
    }
    // Nested submenu — revealed on hover (pure-CSS via --has-sub > __sub).
    // Reference markup (decius site docs) is `class="dcs-menu dcs-menu__sub"`:
    // dcs-menu supplies the panel chrome (background/border/shadow/min-width),
    // dcs-menu__sub the cascade positioning.
    {
        auto& sub = open_node(WidgetKind::Container, "div",
                              "dcs-menu dcs-menu__sub", "__sub", here, true);
        (void) sub;
        if (build) build(*this);
        close_node();  // sub
    }
    close_node();  // item
    return ref_for_node(node, current_panel_id(stack_));
}

View::Scope View::dock_panel(std::string_view title,
                             std::string_view tabpanel_id,
                             std::string_view classes,
                             std::string_view key,
                             std::source_location here) {
    const std::size_t depth_before = stack_.size();  // unwind here on close
    std::string pane_classes{default_class(theme_, FrameworkElement::DockPanel)};
    if (!classes.empty()) { pane_classes += ' '; pane_classes += classes; }
    const auto pane_recipe = default_element(theme_, FrameworkElement::DockPanel);
    auto& pane = open_node(WidgetKind::Container, pane_recipe.tag, pane_classes,
                           key, here, true);
    (void) pane;  // unwound via depth_before; ref only needed for child opens

    // Tab bar with a single selected tab targeting the body.
    {
        auto& tabbar = open_node(WidgetKind::Container, "div",
                                 "dcs-dockpane__tabbar", "__tabbar", here, true);
        (void) tabbar;
        auto& tabs = open_node(WidgetKind::Container, "div",
                               "dcs-dockpane__tabs", "__tabs", here, true);
        (void) tabs;
        const auto tab_recipe = default_element(theme_, FrameworkElement::DockPanelTab);
        auto& tab = open_node(WidgetKind::Button, tab_recipe.tag,
                              tab_recipe.classes, "__tab", here, false);
        set_attr(tab, "type", "button");
        set_attr(tab, "aria-selected", "true");
        set_attr(tab, "data-dcs-target", "#" + std::string(tabpanel_id));
        set_text(tab, title);
        close_node();  // tabs
        close_node();  // tabbar
    }

    // Body (the build scope target). Carries the id the tab points at.
    const auto body_recipe = default_element(theme_, FrameworkElement::DockPanelBody);
    auto& body = open_node(WidgetKind::Container, body_recipe.tag,
                           body_recipe.classes, tabpanel_id, here, true);
    set_attr(body, "id", std::string(tabpanel_id));
    // The returned Scope owns BOTH the pane and the body: the caller fills the
    // body, and when the scope unwinds it closes body then pane back to the
    // depth before the pane was opened. (A single close would leak the pane.)
    return Scope{this, &body, depth_before};
}

// ── Declarative docking engine ──────────────────────────────────────────────

// Recorded dockable declarations gathered during a document_view build, before
// the layout is resolved + emitted.
struct View::DockRecorder {
    struct Spec {
        std::string                id;
        std::string                title;
        std::string                icon;     // di glyph for the tab (empty = none)
        std::string                parent;   // empty = the document/center
        Dock                       side{Dock::Left};
        DockState                  state{DockState::Docked};
        std::optional<int>         size;
        std::optional<DockCorner>          anchor;      // floating anchor corner
        std::optional<std::pair<int, int>> offset;      // floating pos (px)
        std::optional<std::pair<int, int>> float_size;  // floating size (px)
        std::function<void(View&)> content;
        std::function<void(View&)> toolbar;   // tab toolbar (empty = none)
    };
    std::function<void(View&)> document_content;
    std::function<void(View&)> document_toolbar;  // document tab toolbar
    std::string                document_title{"Document"};
    std::string                document_icon;
    std::vector<Spec>          panels;

    [[nodiscard]] const Spec* find(std::string_view id) const {
        for (const auto& p : panels)
            if (p.id == id) return &p;
        return nullptr;
    }
};

// The resolved layout: a tree of splits (dcs-dock) and leaves (dcs-dockpane).
struct View::DockNode {
    bool                  split{false};
    bool                  vertical{false};   // split orientation (column)
    std::vector<DockNode> children;          // split children (splitter between)
    // Slot flex as a full inline value ("1 1 240px"); when set it wins over
    // `size` below. Carried by dock-layout REPLAY so a user-arranged split
    // keeps its exact proportions across rebuilds.
    std::string           flex;
    // Leaf:
    std::string                       id;
    std::string                       title;
    std::string                       icon;
    bool                              is_document{false};
    std::string                       active_tab;
    std::optional<int>                size;     // px flex-basis
    std::optional<std::pair<int, int>> float_size;  // default tearoff size
    std::function<void(View&)>        content;
    std::function<void(View&)>        toolbar;  // tab toolbar (empty = none)
    std::vector<const DockRecorder::Spec*> tabs;  // Dock::Tab co-panels
    std::string                       placement_parent;
    Dock                              placement_side{Dock::Left};
};

namespace {
// A leaf's default flex-basis (px) when none was given, by which edge it sits
// on. Center/document gets flex:1 (basis 0 here meaning "grow").
int default_dock_size(Dock side) {
    switch (side) {
        case Dock::Left:
        case Dock::Right:  return 280;
        case Dock::Top:
        case Dock::Bottom: return 160;
        case Dock::Tab:    return 0;
    }
    return 0;
}

// A panel's EFFECTIVE placement: the runtime override (drag-to-dock / tearoff,
// supplied via the placement provider) wins over the declared DockLocation —
// the structural analogue of the saved-size-wins rule. Returns plain fields so
// it can live at file scope (the Spec nested type is private to View).
struct EffPlacement {
    std::string        parent;
    Dock               side{Dock::Left};
    bool               floating{false};
    bool               override_has_size{false};
    std::optional<int> size;
    // Anchor corner for a DECLARED floating seed (offset counts inward from
    // it). Cleared whenever a runtime override supplies concrete x/y.
    std::optional<DockCorner> anchor;
    int x{0}, y{0}, w{0}, h{0};
};
EffPlacement effective_placement(
    std::string_view id, std::string parent, Dock side, DockState state,
    std::optional<int> size, std::optional<DockCorner> anchor,
    std::optional<std::pair<int, int>> offset,
    std::optional<std::pair<int, int>> float_size,
    const std::function<Document::DockPlacement(std::string_view)>& provider) {
    EffPlacement e;
    e.parent = std::move(parent);
    e.side = side;
    e.size = size;
    e.anchor = anchor;
    e.floating =
        (state == DockState::Detached || state == DockState::Tearoff);
    if (offset) { e.x = offset->first; e.y = offset->second; }
    if (float_size) { e.w = float_size->first; e.h = float_size->second; }
    if (provider) {
        const auto ov = provider(id);
        if (ov.present) {
            e.floating = ov.floating;
            if (ov.floating) {
                e.anchor.reset();
                e.x = ov.x;
                e.y = ov.y;
                if (ov.w > 0) e.w = ov.w;
                if (ov.h > 0) e.h = ov.h;
            } else {
                e.parent = ov.parent.empty() ? std::string("__document__")
                                             : ov.parent;
                e.side = static_cast<Dock>(ov.side);
                if (ov.size > 0) {
                    e.size = ov.size;
                    e.override_has_size = true;
                }
            }
        }
    }
    return e;
}
}  // namespace

View::DockNode View::resolve_dock(const DockRecorder& rec,
                                  std::string_view node_id,
                                  bool is_document) const {
    DockNode base;
    base.id = std::string(node_id);
    if (is_document) {
        base.is_document = true;
        base.title = rec.document_title;
        base.icon = rec.document_icon;
        base.content = rec.document_content;
        base.toolbar = rec.document_toolbar;
    }
    // A panel's effective placement (runtime override wins over the declared
    // DockLocation), used for every structural decision below.
    auto eff = [&](const DockRecorder::Spec& s) {
        return effective_placement(s.id, s.parent, s.side, s.state, s.size,
                                   s.anchor, s.offset, s.float_size,
                                   dock_placement_provider_);
    };
    int slot = 0;  // this node's slot size in px (0 = flexible: the center, or a
                   // sharing child nested inside another pane's slot)
    if (!is_document) {
        if (const auto* spec = rec.find(node_id)) {
            base.title = spec->title;
            base.icon = spec->icon;
            base.content = spec->content;
            base.toolbar = spec->toolbar;
            base.float_size = spec->float_size;
            const auto e = eff(*spec);
            base.placement_parent = e.parent;
            base.placement_side = e.side;
            // Saved size (from the workspace) wins over the declared seed.
            const int saved =
                dock_size_provider_ ? dock_size_provider_(node_id) : 0;
            slot = e.override_has_size
                       ? *e.size
                       : (saved > 0
                              ? saved
                              : (e.size ? *e.size : default_dock_size(e.side)));
        }
    }

    // Tabs: panels whose EFFECTIVE parent is here with Dock::Tab become co-tabs.
    for (const auto& p : rec.panels) {
        const auto e = eff(p);
        if (e.parent == node_id && e.side == Dock::Tab && !e.floating)
            base.tabs.push_back(&p);
    }
    if (dock_active_tab_provider_) {
        const std::string active = dock_active_tab_provider_(node_id);
        if (active == base.id) {
            base.active_tab.clear();
        } else {
            for (const auto* t : base.tabs) {
                if (t && t->id == active) {
                    base.active_tab = active;
                    break;
                }
            }
        }
    }

    // Directional children wrap `base` in splits, applied in DECLARATION ORDER
    // (the order they were listed in their container). Each panel wraps the
    // current accumulated layout on its side; so a panel docked Bottom after
    // the side panels spans the full width below them, etc. Floating panels are
    // skipped here (they are emitted as overlays by document_view).
    int split_index = 0;
    for (const auto& p : rec.panels) {
        const auto e = eff(p);
        if (e.parent != node_id || e.side == Dock::Tab || e.floating) continue;
        DockNode sub = resolve_dock(rec, p.id, false);
        // A child docked INTO a non-document node shares that node's slot, so it
        // must drop its own fixed basis (keeping it is what blew nested docks
        // out). The document keeps its children sized.
        if (!is_document) sub.size.reset();
        DockNode split;
        split.split = true;
        split.id = std::string(node_id) + "__split" +
                   std::to_string(split_index++);
        split.vertical = (e.side == Dock::Top || e.side == Dock::Bottom);
        if (e.side == Dock::Left || e.side == Dock::Top) {
            split.children.push_back(std::move(sub));
            split.children.push_back(std::move(base));
        } else {
            split.children.push_back(std::move(base));
            split.children.push_back(std::move(sub));
        }
        base = std::move(split);
    }
    // The OUTERMOST node carries this node's slot size: whether it stayed a bare
    // leaf or became a split group, it occupies the slot the pane would have had
    // (the group's sharing children split it). The center stays flexible.
    if (!is_document && slot > 0) base.size = slot;
    return base;
}

// Replay: convert a live Document::DockLayout node into the emit tree.
// Content/title/icon/toolbar come from the recorder by panel id; layout tabs
// whose panel is no longer declared are dropped (leaves that end up empty are
// pruned by the caller via children.empty()/id.empty()).
View::DockNode View::dock_node_from_layout(
    const Document::DockLayout::Node& n, const DockRecorder& rec) const {
    DockNode out;
    out.flex = n.flex;
    if (n.split) {
        out.split = true;
        out.vertical = n.vertical;
        for (const auto& c : n.children) {
            DockNode child = dock_node_from_layout(c, rec);
            const bool empty_leaf = !child.split && child.id.empty();
            const bool empty_split = child.split && child.children.empty();
            if (!empty_leaf && !empty_split) {
                out.children.push_back(std::move(child));
            }
        }
        // Identity is CONTENT-DERIVED, never a synthesis counter: a split
        // group is named by its axis + its children's ids, so an unchanged
        // arrangement resolves to the SAME StableId on every pass (zero
        // reconcile ops), and a genuinely rearranged one re-splices
        // wholesale — which is cheap by design. (A counter here re-created
        // the entire dock region on every rebuild.)
        out.id = n.vertical ? "v" : "h";
        for (const auto& child : out.children) {
            out.id += '+';
            out.id += child.id;
        }
        // decius leaves single-child docks AS-IS (a one-pane dock is the normal
        // one-panel state, not a wrapper to unwrap). We reproduce the live tree
        // faithfully and never collapse a nesting level — only empty children
        // are dropped above.
        return out;
    }
    // Leaf: first known tab is the primary; the rest become co-tabs.
    std::vector<std::string> known;
    for (const auto& id : n.tabs) {
        if (id == "__document__" || rec.find(id)) known.push_back(id);
    }
    if (known.empty()) return out;  // dropped (id stays empty)
    // Round-trip the dock-graph placement the live pane carried; without it
    // every replayed pane re-emits as document:left and the graph that
    // gestures reason about is corrupted after the first rebuild.
    if (!n.dock_parent.empty()) out.placement_parent = n.dock_parent;
    if (n.dock_side >= 0) out.placement_side = static_cast<Dock>(n.dock_side);
    const std::string& primary = known.front();
    out.id = primary;
    if (primary == "__document__") {
        out.is_document = true;
        out.title = rec.document_title;
        out.icon = rec.document_icon;
        out.content = rec.document_content;
        out.toolbar = rec.document_toolbar;
    } else if (const auto* spec = rec.find(primary)) {
        out.title = spec->title;
        out.icon = spec->icon;
        out.content = spec->content;
        out.toolbar = spec->toolbar;
        out.float_size = spec->float_size;
    }
    for (std::size_t i = 1; i < known.size(); ++i) {
        if (const auto* spec = rec.find(known[i])) out.tabs.push_back(spec);
    }
    if (!n.active.empty() && n.active != primary) out.active_tab = n.active;
    return out;
}

void View::set_dock_layout_provider(std::function<Document::DockLayout()> fn) {
    dock_layout_provider_ = std::move(fn);
}

void View::emit_dock_node(const DockNode& node, bool is_root,
                          const DockRecorder* rec,
                          std::source_location here) {
    if (node.split) {
        std::string cls = "dcs-dock";
        if (node.vertical) cls += " dcs-dock--v";
        auto& dock = open_node(WidgetKind::Container, "div", cls,
                               "dock-" + node.id,
                               std::source_location::current(), true);
        // A split GROUP that occupies a fixed slot (a nested dock replacing a
        // sized pane) carries that basis; the root/center group flexes to fill.
        const std::string split_style_prefix =
            std::string("display:flex;") +
            (node.vertical ? "flex-direction:column;" : "");
        std::string split_flex;
        if (!node.flex.empty()) {
            split_flex = "flex:" + node.flex;  // replayed exact slot flex
        } else if (node.size && *node.size > 0) {
            split_flex = "flex:0 0 " + std::to_string(*node.size) + "px";
        } else {
            split_flex = "flex:1 1 0px";
        }
        set_attr(dock, "style",
                 split_style_prefix + split_flex + ";min-width:0;min-height:0");
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            if (i > 0) splitter(node.vertical, "split-" + node.id + "-" +
                                                   std::to_string(i));
            emit_dock_node(node.children[i], false, rec, here);
        }
        close_node();
        return;
    }

    // Leaf: a dcs-dockpane carrying the panel(s). Build it directly so we can
    // set the flex-basis the resolver computed.
    const std::string body_id = node.id + "-body";
    std::string pane_cls = "dcs-dockpane";
    if (node.is_document) pane_cls += " dcs-dockpane--center";
    auto& pane = open_node(WidgetKind::Container, "section", pane_cls,
                           "pane-" + node.id, std::source_location::current(),
                           true);
    if (!node.flex.empty()) {
        // Replayed exact slot flex (dock-layout replay).
        set_attr(pane, "style",
                 "flex:" + node.flex + ";min-width:0;min-height:0");
    } else if (node.is_document) {
        set_attr(pane, "style", "flex:1 1 0px;min-width:0;min-height:0");
    } else if (node.size && *node.size > 0) {
        set_attr(pane, "style",
                 "flex:0 0 " + std::to_string(*node.size) +
                     "px;min-width:0;min-height:0");
    } else {
        // A nested sharing leaf (its slot belongs to the enclosing group): grow
        // to split that group's space evenly with its siblings.
        set_attr(pane, "style", "flex:1 1 0px;min-width:0;min-height:0");
    }
    if (!node.is_document) {
        set_attr(pane, "data-aui-dock-parent", node.placement_parent);
        set_attr(pane, "data-aui-dock-side",
                 std::to_string(static_cast<int>(node.placement_side)));
    }

    const bool primary_selected = node.active_tab.empty();

    // Tab bar: the primary tab plus any Dock::Tab co-panels.
    {
        auto& tabbar = open_node(WidgetKind::Container, "div",
                                 "dcs-dockpane__tabbar", "__tabbar",
                                 std::source_location::current(), true);
        (void) tabbar;
        auto& tabs = open_node(WidgetKind::Container, "div",
                               "dcs-dockpane__tabs", "__tabs",
                               std::source_location::current(), true);
        (void) tabs;
        auto emit_tab = [&](std::string_view title, std::string_view icon,
                            std::string_view target, bool selected,
                            std::string_view k,
                            std::optional<std::pair<int, int>> float_size) {
            auto& tab = open_node(WidgetKind::Button, "button",
                                  "dcs-dockpane__tab", k,
                                  std::source_location::current(), true);
            set_attr(tab, "type", "button");
            set_attr(tab, "aria-selected", selected ? "true" : "false");
            set_attr(tab, "data-dcs-target", "#" + std::string(target));
            if (float_size && float_size->first > 0 && float_size->second > 0) {
                set_attr(tab, "data-dcs-tearout-width",
                         std::to_string(float_size->first));
                set_attr(tab, "data-dcs-tearout-height",
                         std::to_string(float_size->second));
            }
            if (!icon.empty()) {
                open_node(WidgetKind::Container, "i",
                          "di di-" + std::string(icon), "__tab-icon",
                          std::source_location::current(), false);
            }
            // Title in its own span so the icon + label are distinct children.
            auto& label = open_node(WidgetKind::Container, "span",
                                    "dcs-dockpane__tab-label", "__tab-label",
                                    std::source_location::current(), false);
            set_text(label, title);
            close_node();  // tab
        };
        emit_tab(node.title, node.icon, body_id, primary_selected, "__tab",
                 node.float_size);
        int ti = 0;
        for (const auto* t : node.tabs) {
            emit_tab(t->title, t->icon, t->id + "-body",
                     node.active_tab == t->id,
                     "__tab-" + std::to_string(ti++), t->float_size);
        }
        close_node();  // tabs

        // Optional tab toolbar: the strip beside the tabs (filter buttons, a
        // search field, a viewport's mode/tool controls). Mirrors the decius
        // dcs-dockpane__toolbars > dcs-dockpane__toolbar[data-dcs-tabtoolbar]
        // structure so the bundle styles it (border-left separator, etc.).
        if (node.toolbar) {
            auto& toolbars = open_node(WidgetKind::Container, "div",
                                       "dcs-dockpane__toolbars", "__toolbars",
                                       std::source_location::current(), true);
            (void) toolbars;
            auto& toolbar = open_node(WidgetKind::Container, "div",
                                      "dcs-dockpane__toolbar", "__toolbar",
                                      std::source_location::current(), true);
            set_attr(toolbar, "data-dcs-tabtoolbar", "#" + body_id);
            node.toolbar(*this);
            close_node();  // toolbar
            close_node();  // toolbars
        }
        close_node();  // tabbar
    }

    // Shelf row (toolbar overflow target — decius.js drops a too-wide tab
    // toolbar here). Emitted hidden; part of the canonical pane chrome so
    // moved/split panes always have it.
    {
        auto& shelf = open_node(WidgetKind::Container, "div",
                                "dcs-dockpane__shelf", "__shelf",
                                std::source_location::current(), true);
        set_attr(shelf, "hidden", "");
        close_node();  // shelf
    }

    // Body: ONE .dcs-dockpane__body holding a [data-dcs-tabpanel] per tab —
    // the canonical decius shape (what decius.js mutates and index.html uses).
    // Tab switching toggles `hidden` on the tabpanels, not on sibling bodies.
    {
        auto& body = open_node(WidgetKind::Container, "div",
                               "dcs-dockpane__body", "__body",
                               std::source_location::current(), true);
        if (node.is_document) {
            set_attr(body, "data-dcs-float-host", "");
            // Stable id for the float-host rect (tests, app code) — distinct
            // from the document TABPANEL, which owns "<id>-body".
            set_attr(body, "id", node.id + "-host");
            set_attr(body, "style",
                     "position:relative;overflow:hidden;min-width:0;min-height:0");
        }
        auto emit_tabpanel = [&](std::string_view panel_body_id, bool selected,
                                 const std::function<void(View&)>& content) {
            auto& panel = open_node(WidgetKind::Container, "div", "",
                                    panel_body_id,
                                    std::source_location::current(), true);
            set_attr(panel, "id", panel_body_id);
            set_attr(panel, "data-dcs-tabpanel", "");
            if (!selected) set_attr(panel, "hidden", "");
            else remove_attr(panel, "hidden");
            if (selected && content) content(*this);
            close_node();  // tabpanel
        };
        emit_tabpanel(body_id, primary_selected, node.content);
        for (const auto* t : node.tabs) {
            emit_tabpanel(t->id + "-body", node.active_tab == t->id,
                          t->content);
        }
        close_node();  // body
    }

    close_node();  // pane
    (void) is_root;
}

void View::emit_one_floating_panel(const DockRecorder& rec,
                                   const std::string& primary_id,
                                   const std::vector<std::string>& co_tab_ids,
                                   int x, int y, int w, int h,
                                   const std::string& active_tab,
                                   std::optional<DockCorner> anchor,
                                   std::source_location here) {
    const auto* s = rec.find(primary_id);
    if (!s) return;
    std::vector<const DockRecorder::Spec*> tabs;
    for (const auto& id : co_tab_ids) {
        if (const auto* t = rec.find(id)) tabs.push_back(t);
    }
    const bool primary_selected =
        active_tab.empty() || active_tab == primary_id;
    bool has_any_toolbar = static_cast<bool>(s->toolbar);
    for (const auto* t : tabs) {
        if (t && t->toolbar) {
            has_any_toolbar = true;
            break;
        }
    }
    if (w <= 0) w = 320;
    if (h <= 0) h = 240;

    // A declared anchor places the seed inward from that corner (right:/
    // bottom: CSS), so a "hug the top-right" float stays put as the window
    // resizes. Replay and drag surgery always use concrete left/top — the
    // first drag commit rewrites the style with left/top and drops
    // right/bottom (with_float_position), so the anchor is seed-only.
    const bool from_right =
        anchor && (*anchor == DockCorner::TopRight ||
                   *anchor == DockCorner::BottomRight);
    const bool from_bottom =
        anchor && (*anchor == DockCorner::BottomLeft ||
                   *anchor == DockCorner::BottomRight);
    auto& panel = open_node(WidgetKind::Container, "section",
                            "dcs-panel dcs-panel--floating",
                            "float-" + s->id, here, true);
    set_attr(panel, "style",
             std::string("position:absolute;") +
                 (from_right ? "right:" : "left:") + std::to_string(x) +
                 "px;" + (from_bottom ? "bottom:" : "top:") +
                 std::to_string(y) + "px;width:" + std::to_string(w) +
                 "px;height:" + std::to_string(h) +
                 "px;z-index:60;display:flex;flex-direction:column;"
                 "pointer-events:auto");
    set_attr(panel, "data-dcs-drag", "");
    set_attr(panel, "data-dcs-drag-bounds", ".dcs-dock--floathost");
    set_attr(panel, "data-dcs-dock-id", s->id);

    std::string dock_cls = "dcs-dockpane";
    dock_cls += tabs.empty()
        ? " dcs-dockpane--single-tab dcs-dockpane--title-only"
        : " dcs-dockpane--multi-tab";
    if (has_any_toolbar) dock_cls += " dcs-dockpane--shelved";
    auto& dock = open_node(WidgetKind::Container, "section", dock_cls,
                           "pane-" + s->id, here, true);
    set_attr(dock, "style", "flex:1;min-width:0;min-height:0");
    set_attr(dock, "data-aui-dock-floating", "true");
    // Anchored seeds don't know their left/top until layout — leave the x/y
    // hints off so readers fall back to the live rect (the truth).
    if (!from_right) set_attr(dock, "data-aui-dock-x", std::to_string(x));
    if (!from_bottom) set_attr(dock, "data-aui-dock-y", std::to_string(y));
    set_attr(dock, "data-aui-dock-w", std::to_string(w));
    set_attr(dock, "data-aui-dock-h", std::to_string(h));

    auto emit_tab = [&](const DockRecorder::Spec& spec, bool selected,
                        std::string_view key, bool title_tab) {
        std::string cls = "dcs-dockpane__tab";
        if (title_tab) {
            cls += " dcs-panel__title dcs-panel__title--dock-tab";
        }
        auto& tab = open_node(WidgetKind::Button, "button", cls, key, here,
                              true);
        set_attr(tab, "type", "button");
        set_attr(tab, "aria-selected", selected ? "true" : "false");
        set_attr(tab, "data-dcs-target", "#" + spec.id + "-body");
        if (title_tab) {
            // The bundle's `.dcs-panel__title--dock-tab` rules own the look
            // (appearance reset, grab cursor, tab chrome suppressed) — same as
            // decius.js prepareTabForTitlebar, which only swaps classes.
            set_attr(tab, "data-dcs-title-tab", "");
        }
        if (spec.float_size && spec.float_size->first > 0 &&
            spec.float_size->second > 0) {
            set_attr(tab, "data-dcs-tearout-width",
                     std::to_string(spec.float_size->first));
            set_attr(tab, "data-dcs-tearout-height",
                     std::to_string(spec.float_size->second));
        }
        if (!spec.icon.empty()) {
            open_node(WidgetKind::Container, "i", "di di-" + spec.icon,
                      "__tab-icon", here, false);
        }
        auto& label = open_node(WidgetKind::Container, "span",
                                "dcs-dockpane__tab-label", "__tab-label",
                                here, false);
        set_text(label, spec.title);
        close_node();  // tab
    };
    auto emit_toolbar = [&](const DockRecorder::Spec& spec,
                            std::string_view key) {
        if (!spec.toolbar) return;
        const bool selected =
            (&spec == s) ? primary_selected : active_tab == spec.id;
        auto& toolbar = open_node(WidgetKind::Container, "div",
                                  "dcs-dockpane__toolbar", key, here, true);
        set_attr(toolbar, "data-dcs-tabtoolbar", "#" + spec.id + "-body");
        if (!selected) set_attr(toolbar, "hidden", "");
        else remove_attr(toolbar, "hidden");
        spec.toolbar(*this);
        close_node();
    };

    if (tabs.empty()) {
        auto& hd = open_node(WidgetKind::Container, "header",
                             "dcs-panel__header dcs-dockpane__titlebar",
                             "__fh-" + s->id, here, true);
        set_attr(hd, "data-dcs-drag-handle", "");
        emit_tab(*s, true, "__title-tab-" + s->id, true);
        open_node(WidgetKind::Container, "div", "dcs-panel__tools",
                  "__ftools-" + s->id, here, false);
        close_node();  // titlebar
        // The hidden tab row stays in the chrome (canonical decius shape) so a
        // tab dropped onto this title-only float can re-grow the tab row
        // (ensure_tabbed_dock moves the title tab back into it).
        auto& tabbar = open_node(WidgetKind::Container, "div",
                                 "dcs-dockpane__tabbar", "__tabbar-" + s->id,
                                 here, true);
        set_attr(tabbar, "hidden", "");
        open_node(WidgetKind::Container, "div", "dcs-dockpane__tabs",
                  "__tabs-" + s->id, here, true);
        close_node();  // tabs
        open_node(WidgetKind::Container, "div", "dcs-dockpane__toolbars",
                  "__toolbars-" + s->id, here, true);
        close_node();  // toolbars
        close_node();  // tabbar
    } else {
        auto& tabbar = open_node(WidgetKind::Container, "div",
                                 "dcs-dockpane__tabbar", "__tabbar-" + s->id,
                                 here, true);
        set_attr(tabbar, "data-dcs-drag-handle", "");
        open_node(WidgetKind::Container, "div", "dcs-dockpane__tabs",
                  "__tabs-" + s->id, here, true);
        emit_tab(*s, primary_selected, "__tab-" + s->id, false);
        int ti = 0;
        for (const auto* t : tabs) {
            emit_tab(*t, active_tab == t->id,
                     "__tab-" + std::to_string(ti++), false);
        }
        close_node();  // tabs
        open_node(WidgetKind::Container, "div", "dcs-dockpane__toolbars",
                  "__toolbars-" + s->id, here, true);
        close_node();  // toolbars
        close_node();  // tabbar
    }
    if (has_any_toolbar) {
        auto& shelf = open_node(WidgetKind::Container, "div",
                                "dcs-dockpane__shelf", "__shelf-" + s->id,
                                here, true);
        (void) shelf;
        emit_toolbar(*s, "__toolbar-" + s->id);
        int tbi = 0;
        for (const auto* t : tabs) {
            emit_toolbar(*t, "__toolbar-" + std::to_string(tbi++));
        }
        close_node();  // shelf
    }

    // ONE body holding a [data-dcs-tabpanel] per tab — the canonical decius
    // shape (same as emit_dock_node); tab switching toggles the tabpanels.
    auto& body = open_node(WidgetKind::Container, "div", "dcs-dockpane__body",
                           "__body-" + s->id, here, true);
    (void) body;
    auto emit_tabpanel = [&](const DockRecorder::Spec& spec, bool selected) {
        auto& tp = open_node(WidgetKind::Container, "div", "",
                             spec.id + "-body", here, true);
        set_attr(tp, "id", spec.id + "-body");
        set_attr(tp, "data-dcs-tabpanel", "");
        if (!selected) set_attr(tp, "hidden", "");
        else remove_attr(tp, "hidden");
        if (selected && spec.content) spec.content(*this);
        close_node();  // tabpanel
    };
    emit_tabpanel(*s, primary_selected);
    for (const auto* t : tabs) emit_tabpanel(*t, active_tab == t->id);
    close_node();  // body
    close_node();  // dockpane
    auto& resize_zones = open_node(WidgetKind::Container, "div",
                                   "dcs-panel__resize-zones",
                                   "__resize-zones-" + s->id, here, true);
    (void) resize_zones;
    const char* dirs[] = {"n", "s", "w", "e", "nw", "ne", "sw", "se"};
    for (const char* dir : dirs) {
        auto& zone = open_node(
            WidgetKind::Container, "div",
            std::string("dcs-panel__resize-zone dcs-panel__resize-zone--") +
                dir,
            std::string("__resize-zone-") + dir + "-" + s->id, here, false);
        set_attr(zone, "data-dir", dir);
    }
    close_node();  // resize zones
    open_node(WidgetKind::Container, "div", "dcs-panel__resize",
              "__resize-" + s->id, here, false);
    close_node();  // panel
}

void View::emit_floating_dock_panels(const DockRecorder& rec,
                                     std::source_location here) {
    // Declared-seed path: panels whose effective placement is floating
    // (declared DockState::Tearoff, or a runtime placement override).
    for (const auto& s : rec.panels) {
        const auto e = effective_placement(s.id, s.parent, s.side, s.state,
                                           s.size, s.anchor, s.offset,
                                           s.float_size,
                                           dock_placement_provider_);
        if (!e.floating) continue;
        std::vector<std::string> co_tab_ids;
        for (const auto& t : rec.panels) {
            if (&t == &s) continue;
            const auto te = effective_placement(
                t.id, t.parent, t.side, t.state, t.size, t.anchor, t.offset,
                t.float_size, dock_placement_provider_);
            if (!te.floating && te.parent == s.id && te.side == Dock::Tab)
                co_tab_ids.push_back(t.id);
        }
        std::string active_tab;
        if (dock_active_tab_provider_) {
            const std::string active = dock_active_tab_provider_(s.id);
            for (const auto& id : co_tab_ids) {
                if (id == active) {
                    active_tab = active;
                    break;
                }
            }
        }
        emit_one_floating_panel(rec, s.id, co_tab_ids, e.x, e.y,
                                e.w > 0 ? e.w : 320, e.h > 0 ? e.h : 240,
                                active_tab, e.anchor, here);
    }
}

void View::emit_layout_floats(const Document::DockLayout& layout,
                              const DockRecorder& rec,
                              std::source_location here) {
    // Replay path: floats exactly as the live DOM had them.
    for (const auto& f : layout.floats) {
        std::vector<std::string> known;
        for (const auto& id : f.pane.tabs) {
            if (rec.find(id)) known.push_back(id);
        }
        if (known.empty()) continue;
        const std::string primary = known.front();
        known.erase(known.begin());
        const std::string active =
            (!f.pane.active.empty() && f.pane.active != primary)
                ? f.pane.active
                : std::string();
        // Keep the position in the form the float's style used: a
        // corner-anchored float replays as right:/bottom: (still glued to its
        // corner), a dragged float as concrete left/top.
        std::optional<DockCorner> anchor;
        if (f.from_right && f.from_bottom) anchor = DockCorner::BottomRight;
        else if (f.from_right) anchor = DockCorner::TopRight;
        else if (f.from_bottom) anchor = DockCorner::BottomLeft;
        emit_one_floating_panel(rec, primary, known, f.x, f.y, f.w, f.h,
                                active, anchor, here);
    }
}

WidgetRef View::document_view(std::string_view key,
                              const std::function<void(View&)>& build,
                              std::source_location here) {
    // Collect declarations.
    DockRecorder recorder;
    DockRecorder* prev = dock_recorder_;
    dock_recorder_ = &recorder;
    if (build) build(*this);
    dock_recorder_ = prev;

    // Resolve the layout (declared seed). The container is the WORKSPACE HOST —
    // the stable, positioned coordinate frame for floats, previews and the drop
    // indicator. The split tree lives in ONE inner .dcs-dock child, so a
    // window-edge dock can wrap/replace the workspace dock without ever
    // touching the host (decius apps structure it the same way: .ps-body /
    // .dn-vp-canvas host a dock tree, they are not docks themselves).
    // REPLAY-or-SEED: when the Document's live dock layout covers every
    // declared panel, re-emit THAT arrangement (drag-to-dock / tearoff surgery
    // survives the rebuild); otherwise resolve the declared seed.
    Document::DockLayout saved;
    if (dock_layout_provider_) saved = dock_layout_provider_();
    bool replay = saved.present;
    if (replay) {
        std::vector<std::string> in_layout;
        std::function<void(const Document::DockLayout::Node&)> collect_ids =
            [&](const Document::DockLayout::Node& n) {
                for (const auto& id : n.tabs) in_layout.push_back(id);
                for (const auto& c : n.children) collect_ids(c);
            };
        collect_ids(saved.root);
        for (const auto& f : saved.floats) collect_ids(f.pane);
        // Misuse guard: a panel in the LIVE dock layout that was never
        // declared via dockpanel() cannot be replayed — its content isn't
        // ours to rebuild, so the tab would be silently dropped and the
        // user's arrangement scrambled on the next rebuild. Surface it.
        for (const auto& id : in_layout) {
            if (id == "__document__" || recorder.find(id)) continue;
            diagnostics_.push_back(
                "dock layout contains panel '" + id +
                "' that was never declared via dockpanel(); its placement "
                "cannot survive a rebuild. Declare it (e.g. "
                "DockLocation::floating()/tab().in(...)) instead of emitting "
                "raw dockpane markup.");
        }
        auto found = [&](std::string_view id) {
            return std::find(in_layout.begin(), in_layout.end(), id) !=
                   in_layout.end();
        };
        if (!found("__document__")) replay = false;
        for (const auto& p : recorder.panels) {
            if (!found(p.id)) {
                replay = false;  // panel set changed in code → stale layout
                break;
            }
        }
    }
    DockNode tree = replay ? dock_node_from_layout(saved.root, recorder)
                           : resolve_dock(recorder, "__document__", true);
    auto& root = open_node(WidgetKind::Container, "div", "dcs-dock--floathost",
                           key, here, true);
    if (stack_.size() == 2) root_app_shell_ = true;  // root-level dock = app shell
    set_attr(root, "style",
             "display:flex;flex:1 1 0px;height:0;min-width:0;min-height:0;"
             "position:relative;overflow:hidden");
    set_attr(root, "data-dcs-float-host", "");
    auto ref = ref_for_node(root, current_panel_id(stack_));
    {
        std::string dock_cls = "dcs-dock";
        if (tree.split && tree.vertical) dock_cls += " dcs-dock--v";
        auto& workdock = open_node(WidgetKind::Container, "div", dock_cls,
                                   "dock-__workspace__", here, true);
        std::string dock_style = "display:flex;";
        if (tree.split && tree.vertical) dock_style += "flex-direction:column;";
        dock_style += "flex:1 1 0px;min-width:0;min-height:0";
        set_attr(workdock, "style", dock_style);
        if (tree.split) {
            for (std::size_t i = 0; i < tree.children.size(); ++i) {
                if (i > 0)
                    splitter(tree.vertical, "split-root-" + std::to_string(i));
                emit_dock_node(tree.children[i], false, &recorder, here);
            }
        } else {
            emit_dock_node(tree, false, &recorder, here);
        }
        close_node();  // workspace dock
    }
    {
        auto& float_layer = open_node(WidgetKind::Container, "div",
                                      "dcs-dock__float-layer",
                                      "dock-float-layer", here, true);
        (void) float_layer;
        // Spans the host (not 0x0) so corner-anchored floats can resolve
        // right:/bottom: against the workspace size; pointer-events:none
        // keeps it hit-transparent like before.
        set_attr(float_layer, "style",
                 "position:absolute;left:0px;top:0px;right:0px;bottom:0px;"
                 "overflow:visible;z-index:60;pointer-events:none");
        if (replay) emit_layout_floats(saved, recorder, here);
        else emit_floating_dock_panels(recorder, here);
        close_node();
    }

    // Drop indicator overlay (hidden until a dock drag hovers a zone). Reuses
    // decius .dcs-drop--valid for the highlight; the interaction positions it.
    auto& dropind = open_node(WidgetKind::Container, "div",
                              "dcs-drop dcs-drop--valid", "dock-dropind", here,
                              true);
    set_attr(dropind, "id", "__dropind");
    set_attr(dropind, "hidden", "");
    set_attr(dropind, "style",
             "position:absolute;pointer-events:none;z-index:200;display:none;"
             "left:0px;top:0px;width:0px;height:0px");
    close_node();  // drop indicator

    close_node();  // container/root dock
    return ref;
}

DockHandle View::document(const std::function<void(View&)>& content,
                          std::string_view title, std::string_view icon,
                          std::source_location here) {
    (void) here;
    if (!dock_recorder_) {
        diagnostics_.push_back("View::document() called outside document_view");
        return {};
    }
    dock_recorder_->document_content = content;
    dock_recorder_->document_title = std::string(title);
    dock_recorder_->document_icon = std::string(icon);
    DockHandle h;
    h.id = "__document__";
    h.owner_ = this;
    return h;
}

void View::attach_dock_toolbar(std::string_view id,
                               const std::function<void(View&)>& build) {
    if (!dock_recorder_) {
        diagnostics_.push_back(
            "DockHandle::toolbar() called outside document_view");
        return;
    }
    if (id == "__document__") {
        dock_recorder_->document_toolbar = build;
        return;
    }
    for (auto& p : dock_recorder_->panels) {
        if (p.id == id) {
            p.toolbar = build;
            return;
        }
    }
}

DockHandle& DockHandle::toolbar(const std::function<void(View&)>& build) {
    if (owner_) owner_->attach_dock_toolbar(id, build);
    return *this;
}

void View::set_dock_size_provider(std::function<int(std::string_view)> fn) {
    dock_size_provider_ = std::move(fn);
}

void View::set_dock_placement_provider(
    std::function<Document::DockPlacement(std::string_view)> fn) {
    dock_placement_provider_ = std::move(fn);
}

void View::set_dock_active_tab_provider(
    std::function<std::string(std::string_view)> fn) {
    dock_active_tab_provider_ = std::move(fn);
}

DockHandle View::dockpanel(std::string_view title,
                           const DockLocation& where,
                           const std::function<void(View&)>& content,
                           std::string_view icon,
                           std::string_view key,
                           std::source_location here) {
    (void) here;
    if (!dock_recorder_) {
        diagnostics_.push_back("View::dockpanel() called outside document_view");
        return {};
    }
    DockRecorder::Spec spec;
    spec.id = key.empty() ? dom_id_fragment(title) : std::string(key);
    spec.title = std::string(title);
    spec.icon = std::string(icon);
    spec.parent = where.parent ? *where.parent : std::string{"__document__"};
    spec.side = where.side ? *where.side : Dock::Left;
    spec.state = where.state;
    spec.size = where.size;
    spec.anchor = where.anchor;
    spec.offset = where.offset;
    spec.float_size = where.float_size;
    spec.content = content;
    dock_recorder_->panels.push_back(std::move(spec));
    DockHandle h;
    h.id = key.empty() ? dom_id_fragment(title) : std::string(key);
    h.owner_ = this;
    return h;
}

View::Scope View::foldout(std::string_view title, bool expanded,
                          std::string_view key, std::source_location here) {
    const std::size_t depth_before = stack_.size();  // unwind here on close
    const bool decius = theme_ == ViewTheme::Decius;

    std::string fold_cls = decius ? "dcs-foldout" : "aui-foldout";
    if (!expanded) fold_cls += decius ? " dcs-foldout--collapsed"
                                      : " aui-foldout--collapsed";
    auto& fold = open_node(WidgetKind::Container, "div", fold_cls, key, here,
                           true);
    (void) fold;

    // Header: clickable row the collapse interaction toggles. Chevron + title.
    {
        auto& header = open_node(WidgetKind::Container, "div",
                                 decius ? "dcs-foldout__header"
                                        : "aui-foldout__header",
                                 "__header", here, true);
        (void) header;
        std::string chev_cls = decius ? "dcs-foldout__chevron"
                                      : "aui-foldout__chevron";
        if (expanded) chev_cls += decius ? " dcs-foldout__chevron--open"
                                         : " aui-foldout__chevron--open";
        auto& chev = open_node(WidgetKind::Container, "span", chev_cls,
                               "__chevron", here, true);
        (void) chev;
        open_node(WidgetKind::Container, "i", "di di-chevron-right",
                  "__chevron-icon", here, false);
        close_node();  // chevron
        auto& title_node = open_node(WidgetKind::Container, "span",
                                     decius ? "dcs-foldout__title"
                                            : "aui-foldout__title",
                                     "__title", here, false);
        set_text(title_node, title);
        close_node();  // header
    }

    // Body — the returned scope's fill target.
    auto& body = open_node(WidgetKind::Container, "div",
                           decius ? "dcs-foldout__body" : "aui-foldout__body",
                           "__body", here, true);
    set_text(body, {});
    return Scope{this, &body, depth_before};
}

WidgetRef View::vec(std::string_view label,
                    const std::vector<std::string>& channels,
                    const std::vector<double>& values, std::string_view key,
                    double step,
                    std::source_location here) {
    if (channels.size() < 2 || channels.size() > 4) {
        diagnostics_.push_back(
            "View::vec expects 2-4 channels; got " +
            std::to_string(channels.size()));
    }
    const auto group_recipe = default_element(theme_, FrameworkElement::FieldGroup);
    std::string group_classes{group_recipe.classes};
    if (theme_ == ViewTheme::Decius) {
        if (!group_classes.empty()) group_classes.push_back(' ');
        group_classes += "dcs-field--vec";
    }
    auto& group = open_node(WidgetKind::Container, group_recipe.tag,
                            group_classes, key, here, true);
    set_attr(group, "data-aui-widget", "vec");
    // No inline styles: the Decius bundle owns .dcs-vec (flex row, s-1 gap,
    // 72px per-item floor) and the framework-support sheet owns the
    // .dcs-field--vec companion rules (the engine toggles the companion class
    // in place of the bundle's :has() selectors).

    const auto label_recipe = default_element(theme_, FrameworkElement::FieldLabel);
    auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                 label_recipe.classes, "__label", here, false);
    set_text(label_node, label);

    auto& vec_node = open_node(WidgetKind::Container, "div",
                               theme_ == ViewTheme::Decius ? "dcs-vec"
                                                           : "aui-vec",
                               "__vec", here, true);
    (void) vec_node;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const double value = i < values.size() ? values[i] : 0.0;
        combo(channels[i], value, step,
              std::string(key.empty() ? "vec" : key) + "-" +
                  std::to_string(i),
              here);
    }
    close_node();  // vec
    close_node();  // group
    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::splitter(bool horizontal, std::string_view key,
                         std::source_location here) {
    std::string cls{default_class(theme_, FrameworkElement::Splitter)};
    if (theme_ == ViewTheme::Decius && horizontal) cls += " dcs-splitter--h";
    auto& node = open_node(WidgetKind::Container, "div", cls, key, here, false);
    set_attr(node, "data-dcs-splitter", horizontal ? "h" : "v");
    // A resize handle owns its cursor. (The bundle sets it via a child-combinator
    // rule that the rule-fill scanner skips, so make it explicit/inline.)
    set_attr(node, "style", horizontal ? "cursor:row-resize" : "cursor:col-resize");
    return ref_for_node(node, current_panel_id(stack_));
}

View::Scope View::tree(std::string_view key, std::source_location here) {
    const auto r = default_element(theme_, FrameworkElement::Tree);
    auto& node = open_node(WidgetKind::Container, r.tag, r.classes, key, here, true);
    set_attr(node, "data-dcs-select", "single");
    return scope_here(node);
}

WidgetRef View::tree_row(std::string_view label, bool selected, int depth,
                         std::string_view key, std::source_location here) {
    TreeRowOptions opts;
    opts.depth = depth;
    opts.selected = selected;
    return tree_row(label, opts, key, here);
}

WidgetRef View::tree_row(std::string_view label, const TreeRowOptions& opts,
                         std::string_view key, std::source_location here) {
    const auto r = default_element(theme_, FrameworkElement::TreeRow);
    // For Decius the row is the canonical <div class="dcs-tree__row"> with
    // chevron / icon / label / meta children. For other themes (e.g. the
    // Bootstrap list-group-item button) fall back to a labeled row.
    const bool decius = theme_ == ViewTheme::Decius;
    auto& node = open_node(WidgetKind::Container,
                           decius ? "div" : r.tag, r.classes, key, here,
                           decius);
    set_attr(node, "aria-selected", opts.selected ? "true" : "false");
    if (opts.draggable) set_attr(node, "draggable", "true");
    if (opts.depth > 0) {
        set_attr(node, "style", "--depth:" + std::to_string(opts.depth));
    }

    if (!decius) {
        set_attr(node, "type", "button");
        set_text(node, label);
        return ref_for_node(node, current_panel_id(stack_));
    }

    // Chevron: open/closed glyph when expandable, else an empty placeholder
    // span (keeps labels aligned across leaf and branch rows).
    {
        std::string chev_cls = "dcs-tree__chevron";
        if (opts.expandable && opts.expanded) chev_cls += " dcs-tree__chevron--open";
        auto& chev = open_node(WidgetKind::Container, "span", chev_cls,
                               "__chevron", here, true);
        (void) chev;
        if (opts.expandable) {
            open_node(WidgetKind::Container, "i", "di di-chevron-right",
                      "__chevron-icon", here, false);
        }
        close_node();  // chevron
    }
    // Type icon.
    if (!opts.icon.empty()) {
        auto& icon = open_node(WidgetKind::Container, "span", "dcs-tree__icon",
                               "__icon", here, true);
        (void) icon;
        open_node(WidgetKind::Container, "i",
                  "di di-" + std::string(opts.icon), "__icon-glyph", here,
                  false);
        close_node();  // icon
    }
    // Label.
    {
        auto& lbl = open_node(WidgetKind::Container, "span", "dcs-tree__label",
                              "__label", here, false);
        set_text(lbl, label);
    }
    // Trailing meta icon (e.g. visibility eye).
    if (!opts.meta_icon.empty()) {
        auto& meta = open_node(WidgetKind::Container, "span", "dcs-tree__meta",
                               "__meta", here, true);
        (void) meta;
        open_node(WidgetKind::Container, "i",
                  "di di-" + std::string(opts.meta_icon), "__meta-glyph", here,
                  false);
        close_node();  // meta
    }

    close_node();  // row
    return ref_for_node(node, current_panel_id(stack_));
}

View::Scope View::status_bar(std::string_view key, std::source_location here) {
    const auto r = default_element(theme_, FrameworkElement::Statusbar);
    auto& node = open_node(WidgetKind::Container, r.tag, r.classes, key, here, true);
    if (stack_.size() == 2) root_app_shell_ = true;
    return scope_here(node);
}

WidgetRef View::color_field(std::string_view label,
                            std::string_view value,
                            const std::vector<std::string>& swatches,
                            std::string_view key,
                            std::source_location here) {
    const auto group_recipe = default_element(theme_, FrameworkElement::FieldGroup);
    auto& group = open_node(WidgetKind::Container, group_recipe.tag,
                            group_recipe.classes, key, here, true);
    set_attr(group, "data-aui-widget", "color");
    set_attr(group, "data-value", value);

    const auto label_recipe = default_element(theme_, FrameworkElement::FieldLabel);
    auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                 label_recipe.classes, "__label", here, false);
    set_text(label_node, label);

    // Swatch trigger button — opens the picker popup via the menu toggle the
    // interaction layer already drives.
    const std::string menu_id =
        std::string(key.empty() ? "color" : key) + "__picker";
    auto& trigger = open_node(WidgetKind::Button, "button",
                              "dcs-swatch", "__trigger", here, false);
    set_attr(trigger, "type", "button");
    set_attr(trigger, "data-dcs-toggle", "menu");
    set_attr(trigger, "data-dcs-target", "#" + menu_id);
    set_attr(trigger, "data-dcs-placement", "bottom");
    set_attr(trigger, "style", "background:" + std::string(value));

    // Popup menu of swatches.
    auto& menu = open_node(WidgetKind::Container, "div",
                           "dcs-menu dcs-menu--swatches", "__menu", here, true);
    set_attr(menu, "id", menu_id);
    set_attr(menu, "hidden", "");
    for (std::size_t i = 0; i < swatches.size(); ++i) {
        auto& sw = open_node(WidgetKind::Button, "button", "dcs-swatch",
                             "__swatch-" + std::to_string(i), here, false);
        set_attr(sw, "type", "button");
        set_attr(sw, "data-dcs-value", swatches[i]);
        set_attr(sw, "style", "background:" + swatches[i]);
    }
    close_node();  // menu
    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::colorfield(std::string_view label, std::string_view value,
                           std::string_view key, std::source_location here) {
    const auto group_recipe = default_element(theme_, FrameworkElement::FieldGroup);
    auto& group = open_node(WidgetKind::Container, group_recipe.tag,
                            group_recipe.classes, key, here, true);
    set_attr(group, "data-aui-widget", "colorfield");
    set_attr(group, "data-value", value);

    const auto label_recipe = default_element(theme_, FrameworkElement::FieldLabel);
    auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                 label_recipe.classes, "__label", here, false);
    set_text(label_node, label);

    if (theme_ != ViewTheme::Decius) {
        // Non-Decius personalities: a native color input is the idiomatic
        // control. (ex11 is Decius-only; this keeps the component portable.)
        auto& input = open_node(WidgetKind::TextInput, "input", "form-control",
                                "__input", here, false);
        set_attr(input, "type", "color");
        set_attr(input, "value", value);
        close_node();  // group
        return ref_for_node(group, current_panel_id(stack_));
    }

    // Canonical Decius color field: chip + editable hex + caret → picker
    // popover. The chip drag-scrub and hex/picker sync are driven by the core
    // interaction layer; the caret's popover toggle is already handled there.
    const std::string base =
        dom_id_fragment(key.empty() ? group.remote_id : std::string(key));
    const std::string field_id = "aui-cf-" + base;
    const std::string picker_id = field_id + "-picker";

    auto& field = open_node(WidgetKind::Container, "div", "dcs-colorfield",
                            "__field", here, true);
    set_attr(field, "id", field_id);
    set_attr(field, "style", "position:relative");

    auto& chip = open_node(WidgetKind::Container, "span", "dcs-colorfield__chip",
                           "__chip", here, false);
    set_attr(chip, "data-dcs-color", value);
    set_attr(chip, "title",
             "drag: \xE2\x86\x90\xE2\x86\x92 hue \xC2\xB7 \xE2\x86\x95 value "
             "\xC2\xB7 Ctrl saturation");
    set_attr(chip, "style", "background:" + std::string(value));

    auto& hex = open_node(WidgetKind::TextInput, "input", "dcs-colorfield__hex",
                          "__hex", here, false);
    set_attr(hex, "type", "text");
    set_attr(hex, "value", value);
    set_attr(hex, "spellcheck", "false");

    auto& caret = open_node(WidgetKind::Container, "span",
                            "dcs-colorfield__caret", "__caret", here, true);
    set_attr(caret, "data-dcs-toggle", "popover");
    set_attr(caret, "data-dcs-target", "#" + picker_id);
    open_node(WidgetKind::Container, "i", "di di-chevron-down", "__caret-icon",
              here, false);
    close_node();  // caret

    // Picker popover: SV square + hue bar + preview/hex. Hidden until the
    // caret toggles it. (The HSV math + cursor sync is the interaction layer's
    // job; this is the canonical structure it drives.)
    auto& pop = open_node(WidgetKind::Container, "div", "dcs-popover",
                          "__picker", here, true);
    set_attr(pop, "id", picker_id);
    set_attr(pop, "style", "width:204px");
    set_attr(pop, "hidden", "");
    {
        auto& body = open_node(WidgetKind::Container, "div", "dcs-popover__body",
                               "__pop-body", here, true);
        set_attr(body, "style",
                 "padding:var(--dcs-s-3);display:flex;flex-direction:column;"
                 "gap:var(--dcs-s-2);flex-grow:0;flex-shrink:0;"
                 "width:100%;box-sizing:border-box;align-items:stretch");
        auto& picker = open_node(WidgetKind::Container, "div",
                                 "dcs-colorfield__picker", "__picker-body",
                                 here, true);
        set_attr(picker, "style",
                 "width:100%;min-width:188px;box-sizing:border-box;"
                 "align-self:stretch;display:flex;"
                 "flex-direction:column;gap:8px;flex-grow:0;"
                 "flex-shrink:0");
        auto& sv = open_node(WidgetKind::Container, "div", "dcs-color-square",
                             "__sv", here, true);
        set_attr(sv, "id", picker_id + "-sv");
        set_attr(sv, "style",
                 "width:100%;height:134px;aspect-ratio:1.4/1;"
                 "box-sizing:border-box;align-self:stretch;"
                 "flex-grow:0;flex-shrink:0");
        open_node(WidgetKind::Container, "div", "dcs-color-square__cursor",
                  "__sv-cursor", here, false);
        close_node();  // sv
        auto& hue = open_node(WidgetKind::Container, "div", "dcs-hue-bar",
                              "__hue", here, true);
        set_attr(hue, "id", picker_id + "-hue");
        set_attr(hue, "style",
                 "width:100%;height:12px;box-sizing:border-box;"
                 "align-self:stretch;flex-grow:0;flex-shrink:0");
        open_node(WidgetKind::Container, "div", "dcs-hue-bar__cursor",
                  "__hue-cursor", here, false);
        close_node();  // hue
        auto& preview = open_node(WidgetKind::Container, "div",
                                  "dcs-colorfield__picker-row", "__preview",
                                  here, true);
        set_attr(preview, "style",
                 "display:flex;align-items:center;gap:6px;"
                 "flex-grow:0;flex-shrink:0");
        auto& preview_chip = open_node(
            WidgetKind::Container, "div", "dcs-colorfield__picker-chip",
            "__preview-chip", here, false);
        set_attr(preview_chip,
                 "style",
                 "--c:" + std::string(value) + ";background:" +
                     std::string(value));
        auto& preview_hex = open_node(
            WidgetKind::TextInput, "input",
            "dcs-input dcs-mono dcs-colorfield__picker-input",
            "__preview-hex", here, false);
        set_attr(preview_hex, "type", "text");
        set_attr(preview_hex, "value", value);
        set_attr(preview_hex, "spellcheck", "false");
        auto& eyedropper = open_node(
            WidgetKind::Button, "button",
            "dcs-btn dcs-btn--icon dcs-btn--sm "
            "dcs-colorfield__picker-eyedropper",
            "__eyedropper", here, true);
        set_attr(eyedropper, "type", "button");
        set_attr(eyedropper, "title", "Pick colour from screen");
        open_node(WidgetKind::Container, "i", "di di-eyedropper",
                  "__eyedropper-icon", here, false);
        close_node();  // eyedropper
        close_node();  // preview
        close_node();  // picker
        close_node();  // body
    }
    close_node();  // popover

    close_node();  // field
    close_node();  // group
    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::combo(std::string_view label, double value, double step,
                      std::string_view key, std::source_location here) {
    // A bare dcs-combo (no field wrapper). Decius-specific control; for other
    // personalities fall back to a plain number input.
    if (theme_ != ViewTheme::Decius) {
        auto& input = open_node(WidgetKind::TextInput, "input", "form-control",
                                key, here, false);
        set_attr(input, "type", "number");
        set_attr(input, "value", number(value));
        set_attr(input, "step", number(step));
        if (!label.empty()) set_attr(input, "aria-label", label);
        return ref_for_node(input, current_panel_id(stack_));
    }

    auto& combo = open_node(WidgetKind::Container, "div", "dcs-combo", key,
                            here, true);
    set_attr(combo, "role", "spinbutton");
    set_attr(combo, "aria-valuenow", number(value));
    set_attr(combo, "data-dcs-combo", "");
    set_attr(combo, "data-value", number(value));
    set_attr(combo, "data-step", number(step));
    if (!label.empty()) set_attr(combo, "data-label", label);
    set_attr(combo, "style", "--fill:" + percent(0.5));

    open_node(WidgetKind::Container, "div", "dcs-combo__fill", "__fill", here,
              false);

    // The channel label (e.g. "X") — the element the dcs-combo__label CSS
    // styles. (decius.js builds this from data-label at runtime; we have no JS,
    // so the builder emits it.)
    if (!label.empty()) {
        auto& lbl = open_node(WidgetKind::Container, "div", "dcs-combo__label",
                              "__label", here, false);
        set_text(lbl, label);
    }

    auto& input = open_node(WidgetKind::TextInput, "input", "dcs-combo__value",
                            "__input", here, false);
    set_attr(input, "type", "number");
    set_attr(input, "value", number(value));
    set_attr(input, "data-fill-min", number(value - 1.0));
    set_attr(input, "data-fill-max", number(value + 1.0));

    close_node();  // combo
    return ref_for_node(combo, current_panel_id(stack_));
}

WidgetRef View::find_widget(std::string_view name) {
    // An empty name is "no name", not a wildcard — it must not resolve to
    // the first keyless widget in tree order.
    if (name.empty()) return WidgetRef{this, root_.id, {}, name};
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

void View::note_component_type_mismatch(std::string_view name) {
    diagnostics_.push_back("component<T>(\"" + std::string(name) +
                           "\"): widget exists but is not of the requested type");
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

std::vector<WidgetChangeBinding> View::commit_bindings() const {
    std::vector<WidgetChangeBinding> out;
    out.reserve(commit_handlers_.size());
    for (const auto& [id, handler] : commit_handlers_) {
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
    out += theme_link(theme_, framework_version_);
    out += "<style>";
    out += command_widget_style();
    out += "</style>";
    out += "</head><body";
    out += body_attrs(theme_, framework_version_, document_attrs_);
    out += "><main id=\"aui-root\" class=\"aui-root";
    if (root_app_shell_) out += " aui-root--shell";
    out += "\">";
    out += to_html_fragment();
    out += "</main></body></html>";
    return out;
}

std::string View::to_html_shell() const {
    std::string out;
    out += "<!doctype html><html><head><meta charset=\"utf-8\">";
    out += theme_link(theme_, framework_version_);
    out += "<style>";
    out += command_widget_style();
    out += "</style>";
    out += "</head><body";
    out += body_attrs(theme_, framework_version_, document_attrs_);
    out += "><main id=\"aui-root\" class=\"aui-root";
    if (root_app_shell_) out += " aui-root--shell";
    out += "\"></main></body></html>";
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
    node->src_file = here.file_name();
    node->src_line = here.line();
    if (public_widget_key(key) && (created || node->widget_name.empty())) {
        set_widget_name(*node, key);
    } else if (!public_widget_key(key) && !node->widget_name.empty()) {
        set_widget_name(*node, {});
    }
    node->cursor = 0;
    node->style_written = false;  // style writes compose per pass (§5.2)

    if (created && sink_) {
        if (kind == WidgetKind::Text) {
            sink_->create_text(*node, parent == &root_ ? nullptr : parent, index);
        } else if (kind == WidgetKind::RawHtml) {
            sink_->create_raw_html(*node, parent == &root_ ? nullptr : parent,
                                   index);
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

void View::flush_attr_diffs(WidgetNode& node) {
    if (node.attrs_touched) {
        if (sink_ != nullptr) {
            for (const auto& attr : node.attrs) {
                const auto* old = find_attr(node.attrs_before, attr.name);
                if (old == nullptr || old->value != attr.value) {
                    sink_->set_attribute(node, attr.name, attr.value);
                }
            }
            for (const auto& old : node.attrs_before) {
                if (find_attr(node.attrs, old.name) == nullptr) {
                    sink_->remove_attribute(node, old.name);
                }
            }
        }
        node.attrs_touched = false;
        node.attrs_before = std::vector<WidgetAttribute>{};  // release capacity
    }
    for (auto& child : node.children) flush_attr_diffs(child);
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

void View::close_to(std::size_t target) {
    // Unwind the open-node stack down to `target` (never past the root). Lets a
    // single Scope own several nested levels opened by a compound builder.
    if (target < 1) target = 1;
    while (stack_.size() > target) close_node();
}

namespace {

// Within one build pass, repeated `style` writes on a node COMPOSE
// (declaration merge, later same-property wins) instead of replacing —
// two builders styling one node must not lose each other's properties
// (WIDGET_RECONCILIATION.md §5.2). Simple ';'-split parse; style values
// emitted by builders are plain declarations (no urls/strings with ';').
std::string merge_style_declarations(std::string_view base,
                                     std::string_view add) {
    struct Decl {
        std::string prop;
        std::string text;
    };
    std::vector<Decl> decls;
    const auto append = [&decls](std::string_view css) {
        std::size_t start = 0;
        while (start <= css.size()) {
            std::size_t end = css.find(';', start);
            if (end == std::string_view::npos) end = css.size();
            std::string_view piece = css.substr(start, end - start);
            while (!piece.empty() &&
                   (piece.front() == ' ' || piece.front() == '\t')) {
                piece.remove_prefix(1);
            }
            while (!piece.empty() &&
                   (piece.back() == ' ' || piece.back() == '\t')) {
                piece.remove_suffix(1);
            }
            if (!piece.empty()) {
                const std::size_t colon = piece.find(':');
                std::string prop{colon == std::string_view::npos
                                     ? piece
                                     : piece.substr(0, colon)};
                while (!prop.empty() && prop.back() == ' ') prop.pop_back();
                bool replaced = false;
                for (auto& d : decls) {
                    if (d.prop == prop) {
                        d.text = std::string(piece);
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) decls.push_back({std::move(prop),
                                                std::string(piece)});
            }
            if (end == css.size()) break;
            start = end + 1;
        }
    };
    append(base);
    append(add);
    std::string out;
    for (const auto& d : decls) {
        if (!out.empty()) out += ';';
        out += d.text;
    }
    return out;
}

}  // namespace

void View::set_attr(WidgetNode& node,
                    std::string_view name,
                    std::string_view value) {
    auto it = std::find_if(node.attrs.begin(), node.attrs.end(),
        [&](const WidgetAttribute& attr) { return attr.name == name; });

    if (reconciling_) {
        // Coalescing path: mutate the node silently; end() emits the net
        // per-node diff vs the snapshot taken at first mutation this pass.
        std::string merged;
        if (name == "style") {
            if (node.style_written && it != node.attrs.end()) {
                merged = merge_style_declarations(it->value, value);
                value = merged;
            }
            node.style_written = true;
        }
        if (it != node.attrs.end() && it->value == value) return;
        if (!node.attrs_touched) {
            node.attrs_touched = true;
            node.attrs_before = node.attrs;  // final state of the last pass
            attr_coalesce_dirty_ = true;
        }
        if (it != node.attrs.end()) {
            it->value = std::string(value);
        } else {
            node.attrs.push_back({std::string(name), std::string(value)});
        }
        return;
    }

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
    if (reconciling_) {
        if (!node.attrs_touched) {
            node.attrs_touched = true;
            node.attrs_before = node.attrs;
            attr_coalesce_dirty_ = true;
        }
        node.attrs.erase(it);
        return;
    }
    node.attrs.erase(it);
    if (auto* sink = current_sink()) sink->remove_attribute(node, name);
}

void View::set_text(WidgetNode& node, std::string_view value) {
    if (node.text == value) return;
    node.text = std::string(value);
    if (node.kind == WidgetKind::RawHtml) {
        if (auto* sink = current_sink()) sink->set_raw_html(node, value);
        return;
    }
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

void View::set_commit_handler(WidgetNode& node,
                              std::function<void(std::string_view)> cb) {
    auto it = std::find_if(commit_handlers_.begin(), commit_handlers_.end(),
        [&](const auto& entry) { return entry.first == node.id; });
    if (it != commit_handlers_.end()) {
        it->second = std::move(cb);
    } else {
        commit_handlers_.push_back({node.id, std::move(cb)});
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
    // Unnamed nodes still get a valid id-addressed ref — the widget name
    // is only the re-find-after-reconciliation fallback, not a validity
    // gate. Builder helpers routinely set text/attrs through the ref of
    // a freshly opened (private-keyed) node.
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
