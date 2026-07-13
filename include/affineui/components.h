#pragma once

// Strongly-typed components for AffineUI.
//
// The View builder layer aggregates HTML into working widgets and hands back a
// WidgetRef — an id-addressed handle that survives reconciliation (it re-finds
// its node by widget-name after a refresh) and degrades gracefully when the
// node is gone (every mutator no-ops). That is the right substrate, but it is
// *stringly* typed: you read and write `value` / `data-*` / `aria-*` by hand.
//
// This header adds the missing piece: thin, strongly-typed wrappers over a
// WidgetRef. Each component (Dropdown, Slider, Checkbox, TextField, ColorField,
// Tree, Toolbar, Foldout, Statusbar, …) exposes semantic accessors —
// `selected()`, `value()`, `checked()`, `set_options()` — instead of attribute
// strings. This is the Dear-ImGui / Qt ergonomic: typed widgets you can hold,
// query by id, and store a handle to.
//
// HARD-TO-CRASH CONTRACT. A wrapper never crashes when its target is gone:
//   - reads return a default/empty/typed-zero value,
//   - writes no-op (the underlying WidgetRef already no-ops),
//   - `explicit operator bool()` reports whether the component still resolves.
// Use `if (auto d = view.component<Dropdown>("blend")) d.value();` to act only
// when present, or just call accessors and accept the safe defaults.
//
// Obtaining a typed component:
//   - builders return one:           Dropdown d = view.dropdown("Blend", o, s, "blend");
//   - query a live one by id:        Dropdown d = view.component<Dropdown>("blend");
//
// Mixing in raw HTML/CSS stays first-class — a typed component is just a view
// over a node; you can still drop `view.html(...)` or custom-class containers
// anywhere around it.

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "affineui/callback.h"
#include "affineui/types.h"
#include "affineui/view.h"

namespace affineui {

/// Base for every typed component: holds the underlying WidgetRef plus a
/// validity tag, and forwards the graceful-degradation surface. Derive and add
/// semantic accessors that early-out unless `valid()`.
///
/// Three states (see ComponentValidity): a wrong-type query stays *attached*
/// to whatever node it found — so you can ask `kind()` / inspect it — but is
/// not `valid()`, and all typed reads/writes are inert. Nothing ever crashes.
class Component {
public:
    Component() = default;

    /// Wrap a ref with an explicit validity (used by View::component<T>).
    Component(WidgetRef ref, ComponentValidity validity) noexcept
        : ref_(std::move(ref)), validity_(validity) {}

    /// Adopt a builder-returned ref. Present iff it resolves; type is the
    /// builder's responsibility, so this is treated as valid-if-present.
    explicit Component(WidgetRef ref) noexcept : ref_(std::move(ref)) {
        validity_ = static_cast<bool>(ref_) ? ComponentValidity::Valid
                                            : ComponentValidity::NotPresent;
    }

    /// True only when bound to a live node of the expected type.
    [[nodiscard]] bool valid() const {
        return validity_ == ComponentValidity::Valid &&
               static_cast<bool>(ref_);
    }
    [[nodiscard]] explicit operator bool() const { return valid(); }
    [[nodiscard]] ComponentValidity validity() const noexcept { return validity_; }

    /// True if some node is attached (even if the wrong type) — useful for
    /// debugging a WrongType wrapper.
    [[nodiscard]] bool attached() const { return static_cast<bool>(ref_); }

    /// The component's stable id (widget-name), or empty if unbound.
    [[nodiscard]] std::string_view id() const { return ref_.name(); }

    /// The actual widget kind of the attached node (for diagnosing WrongType),
    /// or Root when nothing is attached.
    [[nodiscard]] WidgetKind kind() const {
        const WidgetNode* n = ref_.node();
        return n ? n->kind : WidgetKind::Root;
    }

    // ── Generic element operations ──────────────────────────────────────
    // These are NOT type-specific, so they work whenever a node is attached —
    // including in WrongType mode (you can still read/write attributes, text,
    // and visibility of whatever node you actually found). Type-specific
    // accessors on the derived classes instead require valid().

    [[nodiscard]] std::string_view attr(std::string_view name,
                                        std::string_view fallback = {}) const {
        return ref_.attr_value(name, fallback);
    }
    Component& set_attr(std::string_view name, std::string_view value) {
        ref_.attr(name, value);
        return *this;
    }
    [[nodiscard]] std::string_view text() const { return ref_.text_value(); }
    Component& set_text(std::string_view value) {
        ref_.text(value);
        return *this;
    }
    [[nodiscard]] bool visible() const { return !ref_.has_attr("hidden"); }
    Component& set_visible(bool on) {
        if (on) ref_.remove_attr("hidden");
        else    ref_.attr("hidden", "");
        return *this;
    }

    /// Escape hatch to the underlying handle for anything not yet typed.
    [[nodiscard]] WidgetRef& ref() noexcept { return ref_; }
    [[nodiscard]] const WidgetRef& ref() const noexcept { return ref_; }

protected:
    WidgetRef         ref_;
    ComponentValidity validity_{ComponentValidity::NotPresent};
};

/// A push button. Click handler is a safe Callback (no-ops after its target
/// dies); a plain lambda works too.
class Button : public Component {
public:
    using Component::Component;

    [[nodiscard]] static bool matches(const WidgetNode& n) {
        return n.kind == WidgetKind::Button;
    }

    Button& set_label(std::string_view text) {
        if (valid()) ref_.text(text);
        return *this;
    }
    [[nodiscard]] std::string_view label() const {
        return valid() ? ref_.text_value() : std::string_view{};
    }

    Button& set_enabled(bool on) {
        if (!valid()) return *this;
        if (on) ref_.remove_attr("disabled");
        else    ref_.attr("disabled", "");
        return *this;
    }
    [[nodiscard]] bool enabled() const {
        return valid() && !ref_.has_attr("disabled");
    }

    Button& on_click(ClickCallback cb) {
        if (valid()) ref_.on_click(cb.to_function());
        return *this;
    }
};

/// A checkbox / toggle. State is the `aria-checked` attribute the Decius and
/// Bootstrap themes both read.
class Checkbox : public Component {
public:
    using Component::Component;

    [[nodiscard]] static bool matches(const WidgetNode& n) {
        return n.kind == WidgetKind::Checkbox;
    }

    [[nodiscard]] bool checked() const {
        return valid() && ref_.attr_value("aria-checked") == "true";
    }
    Checkbox& set_checked(bool on) {
        if (valid()) ref_.attr("aria-checked", on ? "true" : "false");
        return *this;
    }
    Checkbox& on_change(ChangeCallback cb) {
        if (valid()) ref_.on_change(cb.to_function());
        return *this;
    }
};

/// A single-line text/number/etc. input. `value()` reads the live value.
class TextField : public Component {
public:
    using Component::Component;

    [[nodiscard]] static bool matches(const WidgetNode& n) {
        return n.kind == WidgetKind::TextInput;
    }

    [[nodiscard]] std::string value() const {
        return valid() ? std::string(ref_.attr_value("value")) : std::string{};
    }
    TextField& set_value(std::string_view v) {
        if (valid()) ref_.attr("value", v);
        return *this;
    }
    TextField& on_change(ChangeCallback cb) {
        if (valid()) ref_.on_change(cb.to_function());
        return *this;
    }
};

/// A dropdown / select. `selected()` reads the chosen option value.
class Dropdown : public Component {
public:
    using Component::Component;

    [[nodiscard]] static bool matches(const WidgetNode& n) {
        return n.kind == WidgetKind::Dropdown;
    }

    [[nodiscard]] std::string selected() const {
        return valid() ? std::string(ref_.attr_value("data-value"))
                       : std::string{};
    }
    Dropdown& set_selected(std::string_view value) {
        if (valid()) ref_.attr("data-value", value);
        return *this;
    }
    Dropdown& on_change(ChangeCallback cb) {
        if (valid()) ref_.on_change(cb.to_function());
        return *this;
    }
};

/// A slider / range. Values are doubles read from data-value.
class Slider : public Component {
public:
    using Component::Component;

    [[nodiscard]] static bool matches(const WidgetNode& n) {
        return n.kind == WidgetKind::Slider;
    }

    [[nodiscard]] double value(double fallback = 0.0) const {
        if (!valid()) return fallback;
        // The group node carries the a11y value (aria-valuenow); the inner
        // control mirrors it in data-value. Read the group-level source.
        auto text = ref_.attr_value("aria-valuenow");
        if (text.empty()) text = ref_.attr_value("data-value");
        if (text.empty()) return fallback;
        return std::strtod(std::string(text).c_str(), nullptr);
    }
    Slider& on_change(ChangeCallback cb) {
        if (valid()) ref_.on_change(cb.to_function());
        return *this;
    }
};

/// A color field/swatch that opens a picker popup. `color()` reads the current
/// CSS color string (data-value) the app sets/reads.
class ColorField : public Component {
public:
    using Component::Component;

    [[nodiscard]] static bool matches(const WidgetNode& n) {
        // A color field is an input (its builder sets type=color) or a swatch
        // container carrying the color picker toggle.
        return n.kind == WidgetKind::TextInput ||
               n.kind == WidgetKind::Container;
    }

    [[nodiscard]] std::string color() const {
        if (!valid()) return {};
        auto v = ref_.attr_value("data-value");
        if (v.empty()) v = ref_.attr_value("value");
        return std::string(v);
    }
    ColorField& set_color(std::string_view css_color) {
        if (valid()) ref_.attr("data-value", css_color);
        return *this;
    }
    ColorField& on_change(ChangeCallback cb) {
        if (valid()) ref_.on_change(cb.to_function());
        return *this;
    }
};

/// A dockable panel. Typed surface for the active tab; visibility uses the
/// generic Component::visible(). Tabs/splitters are driven by the existing
/// interaction layer.
class DockPanel : public Component {
public:
    using Component::Component;

    [[nodiscard]] static bool matches(const WidgetNode& n) {
        return n.kind == WidgetKind::Container ||
               n.kind == WidgetKind::Panel ||
               n.kind == WidgetKind::Card;
    }

    [[nodiscard]] std::string_view active_tab() const {
        return valid() ? ref_.attr_value("data-active-tab") : std::string_view{};
    }
    DockPanel& set_active_tab(std::string_view tab_id) {
        if (valid()) ref_.attr("data-active-tab", tab_id);
        return *this;
    }
};

/// A foldout / collapsible section.
class Foldout : public Component {
public:
    using Component::Component;

    [[nodiscard]] static bool matches(const WidgetNode& n) {
        return n.kind == WidgetKind::Container;
    }

    [[nodiscard]] bool open() const {
        return valid() && ref_.attr_value("aria-expanded") != "false";
    }
    Foldout& set_open(bool on) {
        if (valid()) ref_.attr("aria-expanded", on ? "true" : "false");
        return *this;
    }
};

}  // namespace affineui
