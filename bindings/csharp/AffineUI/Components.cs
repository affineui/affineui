// Strongly-typed components — a faithful C# port of include/affineui/components.h
// semantics, implemented over Widget attribute access plus
// affineui_widget_get_kind (typed components deliberately get no C entry
// points; the attribute names are part of the framework contract).
//
// HARD-TO-CRASH CONTRACT (mirrors the C++ header):
//   - reads return a default/empty/typed-zero value,
//   - writes no-op,
//   - IsValid reports whether the component still resolves to a node of the
//     expected kind.
// Three states (Validity): a WrongType component stays attached to whatever
// node it found — generic attribute/text access still works so you can
// inspect it — but all typed accessors are inert. Nothing ever crashes.

using System.Globalization;

namespace AffineUI;

/// <summary>
/// Base for every typed component: holds the underlying <see cref="Widget"/>
/// plus a validity tag and forwards the graceful-degradation surface.
/// </summary>
public abstract class Component
{
    /// <summary>The underlying widget handle (escape hatch for anything not
    /// yet typed).</summary>
    public Widget Widget { get; }

    /// <summary>How this component bound to its node.</summary>
    public Validity Validity { get; }

    private protected Component(Widget widget, Func<WidgetKind, bool> matches)
    {
        Widget = widget;
        WidgetKind kind = widget.Kind;
        Validity = kind == WidgetKind.None ? Validity.NotPresent
                 : matches(kind) ? Validity.Valid
                 : Validity.WrongType;
    }

    /// <summary>True only when bound to a live node of the expected type.</summary>
    public bool IsValid => Validity == AffineUI.Validity.Valid && Widget.IsValid;

    /// <summary>True if some node is attached, even if the wrong type —
    /// useful for diagnosing a <see cref="AffineUI.Validity.WrongType"/>
    /// component.</summary>
    public bool IsAttached => Widget.IsValid;

    /// <summary>The component's stable id (widget-name), or empty.</summary>
    public string Id => Widget.Name;

    /// <summary>The actual widget kind of the attached node (for diagnosing
    /// WrongType), or <see cref="WidgetKind.None"/>.</summary>
    public WidgetKind Kind => Widget.Kind;

    // ── Generic element operations ───────────────────────────────────────
    // NOT type-specific, so they work whenever a node is attached — including
    // in WrongType mode. Type-specific accessors on the derived classes
    // require IsValid instead.

    public string Attr(string name, string fallback = "") => Widget.Attr(name, fallback);

    public void SetAttr(string name, string value) => Widget.SetAttr(name, value);

    public string Text
    {
        get => Widget.Text;
        set => Widget.SetText(value);
    }

    public bool Visible
    {
        get => !Widget.HasAttr("hidden");
        set
        {
            if (value) Widget.RemoveAttr("hidden");
            else Widget.SetAttr("hidden", "");
        }
    }
}

/// <summary>A push button.</summary>
public sealed class Button : Component
{
    internal Button(Widget widget) : base(widget, static k => k == WidgetKind.Button) { }

    public string Label
    {
        get => IsValid ? Widget.Text : "";
        set { if (IsValid) Widget.SetText(value); }
    }

    public bool Enabled
    {
        get => IsValid && !Widget.HasAttr("disabled");
        set
        {
            if (!IsValid) return;
            if (value) Widget.RemoveAttr("disabled");
            else Widget.SetAttr("disabled", "");
        }
    }

    public Button OnClick(Action handler)
    {
        if (IsValid) Widget.OnClick(handler);
        return this;
    }
}

/// <summary>A checkbox / toggle. State is the <c>aria-checked</c> attribute
/// the Decius and Bootstrap themes both read.</summary>
public sealed class Checkbox : Component
{
    internal Checkbox(Widget widget) : base(widget, static k => k == WidgetKind.Checkbox) { }

    public bool Checked
    {
        get => IsValid && Widget.Attr("aria-checked") == "true";
        set { if (IsValid) Widget.SetAttr("aria-checked", value ? "true" : "false"); }
    }

    public Checkbox OnChange(Action<string> handler)
    {
        if (IsValid) Widget.OnChange(handler);
        return this;
    }
}

/// <summary>A single-line text/number/etc. input. <see cref="Value"/> reads
/// the live <c>value</c> attribute.</summary>
public sealed class TextField : Component
{
    internal TextField(Widget widget) : base(widget, static k => k == WidgetKind.TextInput) { }

    public string Value
    {
        get => IsValid ? Widget.Attr("value") : "";
        set { if (IsValid) Widget.SetAttr("value", value); }
    }

    public TextField OnChange(Action<string> handler)
    {
        if (IsValid) Widget.OnChange(handler);
        return this;
    }
}

/// <summary>A dropdown / select. <see cref="Selected"/> reads the chosen
/// option value (<c>data-value</c>).</summary>
public sealed class Dropdown : Component
{
    internal Dropdown(Widget widget) : base(widget, static k => k == WidgetKind.Dropdown) { }

    public string Selected
    {
        get => IsValid ? Widget.Attr("data-value") : "";
        set { if (IsValid) Widget.SetAttr("data-value", value); }
    }

    public Dropdown OnChange(Action<string> handler)
    {
        if (IsValid) Widget.OnChange(handler);
        return this;
    }
}

/// <summary>A slider / range. The group node carries the a11y value
/// (<c>aria-valuenow</c>); the inner control mirrors it in
/// <c>data-value</c>.</summary>
public sealed class Slider : Component
{
    internal Slider(Widget widget) : base(widget, static k => k == WidgetKind.Slider) { }

    /// <summary>Current value, or 0 when unresolved.</summary>
    public double Value => GetValue(0.0);

    /// <summary>Current value, or <paramref name="fallback"/> when the node
    /// is gone or carries no value.</summary>
    public double GetValue(double fallback)
    {
        if (!IsValid) return fallback;
        string text = Widget.Attr("aria-valuenow");
        if (text.Length == 0) text = Widget.Attr("data-value");
        if (text.Length == 0) return fallback;
        return double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out double v)
            ? v : fallback;
    }

    public Slider OnChange(Action<string> handler)
    {
        if (IsValid) Widget.OnChange(handler);
        return this;
    }
}

/// <summary>A color field/swatch that opens a picker popup.
/// <see cref="Color"/> reads the current CSS color string.</summary>
public sealed class ColorField : Component
{
    // A color field is an input (its builder sets type=color) or a swatch
    // container carrying the color picker toggle.
    internal ColorField(Widget widget)
        : base(widget, static k => k is WidgetKind.TextInput or WidgetKind.Container) { }

    public string Color
    {
        get
        {
            if (!IsValid) return "";
            string v = Widget.Attr("data-value");
            if (v.Length == 0) v = Widget.Attr("value");
            return v;
        }
        set { if (IsValid) Widget.SetAttr("data-value", value); }
    }

    public ColorField OnChange(Action<string> handler)
    {
        if (IsValid) Widget.OnChange(handler);
        return this;
    }
}

/// <summary>A dockable panel. Typed surface for the active tab; visibility
/// uses the generic <see cref="Component.Visible"/>. Tabs/splitters are
/// driven by the interaction layer.</summary>
public sealed class DockPanel : Component
{
    internal DockPanel(Widget widget)
        : base(widget, static k => k is WidgetKind.Container or WidgetKind.Panel or WidgetKind.Card) { }

    public string ActiveTab
    {
        get => IsValid ? Widget.Attr("data-active-tab") : "";
        set { if (IsValid) Widget.SetAttr("data-active-tab", value); }
    }
}

/// <summary>A foldout / collapsible section (<c>aria-expanded</c>).</summary>
public sealed class Foldout : Component
{
    internal Foldout(Widget widget) : base(widget, static k => k == WidgetKind.Container) { }

    public bool Open
    {
        get => IsValid && Widget.Attr("aria-expanded") != "false";
        set { if (IsValid) Widget.SetAttr("aria-expanded", value ? "true" : "false"); }
    }
}
