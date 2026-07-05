//! Strongly-typed components over [`Widget`] — the Rust port of the C++
//! `components.h` semantics (implemented wrapper-side per
//! docs/LANGUAGE_BINDINGS.md §3.7: the attribute names are part of the
//! framework contract, no C entry points needed).
//!
//! Hard-to-crash contract, mirrored exactly:
//! - a query for a missing id yields `Validity::NotPresent`;
//! - a query that finds a node of the WRONG kind stays *attached*
//!   (generic attr/text/visible ops still work) but is not valid, and
//!   all typed accessors are inert;
//! - reads on an invalid component return defaults, writes no-op.

use crate::view::{View, Widget, WidgetKind};

/// Validity of a typed component wrapper (mirrors
/// `affineui::ComponentValidity`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Validity {
    /// Bound to a live node of the expected kind.
    Valid,
    /// Bound to a live node, but NOT this component's kind.
    WrongType,
    /// No node with that id resolves.
    NotPresent,
}

macro_rules! component {
    ($(#[$doc:meta])* $name:ident, matches = $matches:expr) => {
        $(#[$doc])*
        pub struct $name {
            widget: Widget,
            validity: Validity,
        }

        impl $name {
            /// Wrap a queried widget, classifying its validity.
            pub fn from_widget(widget: Widget) -> $name {
                let matches: fn(WidgetKind) -> bool = $matches;
                let validity = if !widget.is_valid() {
                    Validity::NotPresent
                } else if matches(widget.kind()) {
                    Validity::Valid
                } else {
                    Validity::WrongType
                };
                $name { widget, validity }
            }

            pub(crate) fn query(view: &View, name: &str) -> $name {
                Self::from_widget(view.find_widget(name))
            }

            /// True only when bound to a live node of the expected kind.
            pub fn is_valid(&self) -> bool {
                self.validity == Validity::Valid && self.widget.is_valid()
            }

            pub fn validity(&self) -> Validity {
                self.validity
            }

            /// True if some node is attached (even if the wrong kind).
            pub fn attached(&self) -> bool {
                self.widget.is_valid()
            }

            /// The component's stable id (widget key), or empty.
            pub fn id(&self) -> String {
                self.widget.name()
            }

            /// The actual kind of the attached node (for diagnosing
            /// WrongType), or `WidgetKind::None`.
            pub fn kind(&self) -> WidgetKind {
                self.widget.kind()
            }

            // Generic element ops: work whenever a node is attached,
            // including WrongType (components.h contract).

            pub fn attr(&self, name: &str, fallback: &str) -> String {
                self.widget.attr(name, fallback)
            }
            pub fn set_attr(&self, name: &str, value: &str) -> &Self {
                self.widget.set_attr(name, value);
                self
            }
            pub fn text(&self) -> String {
                self.widget.text()
            }
            pub fn set_text(&self, value: &str) -> &Self {
                self.widget.set_text(value);
                self
            }
            pub fn visible(&self) -> bool {
                !self.widget.has_attr("hidden")
            }
            pub fn set_visible(&self, on: bool) -> &Self {
                if on {
                    self.widget.remove_attr("hidden");
                } else {
                    self.widget.set_attr("hidden", "");
                }
                self
            }

            /// Escape hatch to the underlying widget handle.
            pub fn widget(&self) -> &Widget {
                &self.widget
            }
        }
    };
}

component!(
    /// A push button.
    Button,
    matches = |k| k == WidgetKind::Button
);

impl Button {
    pub fn label(&self) -> String {
        if self.is_valid() { self.widget.text() } else { String::new() }
    }
    pub fn set_label(&self, label: &str) -> &Self {
        if self.is_valid() {
            self.widget.set_text(label);
        }
        self
    }
    pub fn enabled(&self) -> bool {
        self.is_valid() && !self.widget.has_attr("disabled")
    }
    pub fn set_enabled(&self, on: bool) -> &Self {
        if self.is_valid() {
            if on {
                self.widget.remove_attr("disabled");
            } else {
                self.widget.set_attr("disabled", "");
            }
        }
        self
    }
    pub fn on_click(&self, f: impl FnMut() + 'static) -> &Self {
        if self.is_valid() {
            self.widget.on_click(f);
        }
        self
    }
}

component!(
    /// A checkbox / toggle. State is `aria-checked`.
    Checkbox,
    matches = |k| k == WidgetKind::Checkbox
);

impl Checkbox {
    pub fn checked(&self) -> bool {
        self.is_valid() && self.widget.attr("aria-checked", "") == "true"
    }
    pub fn set_checked(&self, on: bool) -> &Self {
        if self.is_valid() {
            self.widget.set_attr("aria-checked", if on { "true" } else { "false" });
        }
        self
    }
    pub fn on_change(&self, f: impl FnMut(&str) + 'static) -> &Self {
        if self.is_valid() {
            self.widget.on_change(f);
        }
        self
    }
}

component!(
    /// A single-line text-like input.
    TextField,
    matches = |k| k == WidgetKind::TextInput
);

impl TextField {
    pub fn value(&self) -> String {
        if self.is_valid() { self.widget.attr("value", "") } else { String::new() }
    }
    pub fn set_value(&self, value: &str) -> &Self {
        if self.is_valid() {
            self.widget.set_attr("value", value);
        }
        self
    }
    pub fn on_change(&self, f: impl FnMut(&str) + 'static) -> &Self {
        if self.is_valid() {
            self.widget.on_change(f);
        }
        self
    }
}

component!(
    /// A dropdown / select.
    Dropdown,
    matches = |k| k == WidgetKind::Dropdown
);

impl Dropdown {
    pub fn selected(&self) -> String {
        if self.is_valid() { self.widget.attr("data-value", "") } else { String::new() }
    }
    pub fn set_selected(&self, value: &str) -> &Self {
        if self.is_valid() {
            self.widget.set_attr("data-value", value);
        }
        self
    }
    pub fn on_change(&self, f: impl FnMut(&str) + 'static) -> &Self {
        if self.is_valid() {
            self.widget.on_change(f);
        }
        self
    }
}

component!(
    /// A slider / range (value read from `aria-valuenow` / `data-value`).
    Slider,
    matches = |k| k == WidgetKind::Slider
);

impl Slider {
    pub fn value(&self, fallback: f64) -> f64 {
        if !self.is_valid() {
            return fallback;
        }
        let mut text = self.widget.attr("aria-valuenow", "");
        if text.is_empty() {
            text = self.widget.attr("data-value", "");
        }
        text.parse().unwrap_or(fallback)
    }
    pub fn on_change(&self, f: impl FnMut(&str) + 'static) -> &Self {
        if self.is_valid() {
            self.widget.on_change(f);
        }
        self
    }
}

component!(
    /// A color field/swatch that opens a picker.
    ColorField,
    matches = |k| k == WidgetKind::TextInput || k == WidgetKind::Container
);

impl ColorField {
    pub fn color(&self) -> String {
        if !self.is_valid() {
            return String::new();
        }
        let v = self.widget.attr("data-value", "");
        if v.is_empty() { self.widget.attr("value", "") } else { v }
    }
    pub fn set_color(&self, css_color: &str) -> &Self {
        if self.is_valid() {
            self.widget.set_attr("data-value", css_color);
        }
        self
    }
    pub fn on_change(&self, f: impl FnMut(&str) + 'static) -> &Self {
        if self.is_valid() {
            self.widget.on_change(f);
        }
        self
    }
}

component!(
    /// A dockable panel (active tab; visibility via the generic ops).
    DockPanel,
    matches = |k| {
        k == WidgetKind::Container || k == WidgetKind::Panel || k == WidgetKind::Card
    }
);

impl DockPanel {
    pub fn active_tab(&self) -> String {
        if self.is_valid() { self.widget.attr("data-active-tab", "") } else { String::new() }
    }
    pub fn set_active_tab(&self, tab_id: &str) -> &Self {
        if self.is_valid() {
            self.widget.set_attr("data-active-tab", tab_id);
        }
        self
    }
}

component!(
    /// A foldout / collapsible section.
    Foldout,
    matches = |k| k == WidgetKind::Container
);

impl Foldout {
    pub fn open(&self) -> bool {
        self.is_valid() && self.widget.attr("aria-expanded", "") != "false"
    }
    pub fn set_open(&self, on: bool) -> &Self {
        if self.is_valid() {
            self.widget.set_attr("aria-expanded", if on { "true" } else { "false" });
        }
        self
    }
}

// ── Typed queries on View ────────────────────────────────────────────

impl View {
    pub fn button_at(&self, name: &str) -> Button {
        Button::query(self, name)
    }
    pub fn checkbox_at(&self, name: &str) -> Checkbox {
        Checkbox::query(self, name)
    }
    pub fn text_field_at(&self, name: &str) -> TextField {
        TextField::query(self, name)
    }
    pub fn dropdown_at(&self, name: &str) -> Dropdown {
        Dropdown::query(self, name)
    }
    pub fn slider_at(&self, name: &str) -> Slider {
        Slider::query(self, name)
    }
    pub fn color_field_at(&self, name: &str) -> ColorField {
        ColorField::query(self, name)
    }
    pub fn dock_panel_at(&self, name: &str) -> DockPanel {
        DockPanel::query(self, name)
    }
    pub fn foldout_at(&self, name: &str) -> Foldout {
        Foldout::query(self, name)
    }
}
