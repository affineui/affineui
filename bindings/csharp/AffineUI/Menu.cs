// Application menus — the platform-neutral menu MODEL (include/affineui/menu.h).
// On macOS it becomes a real NSMenu installed as the system bar at the top of
// the SCREEN (which an app cannot draw itself); elsewhere it drives the
// in-window bar the app draws. Declare the menus once, hand them to
// App.SetMenu, and re-set them whenever their checked/enabled state changes.
//
// The shape mirrors Electron's Menu.buildFromTemplate, because that vocabulary
// is already in everyone's head:
//
//     app.SetMenu(new Menu
//     {
//         MenuItem.Sub("File",
//             MenuItem.Item("New Scene", "CmdOrCtrl+N", NewScene),
//             MenuItem.Separator(),
//             MenuItem.Role(MenuRole.Quit)),          // standard, auto-labelled
//         MenuItem.Sub("Edit", MenuItem.EditMenu()),  // Undo/Cut/Copy/Paste...
//     });
//
// Two things carry the weight: `Role` (a standard item whose label, accelerator
// and behavior the platform supplies) and `accelerator` (an Electron-style chord
// — "CmdOrCtrl+S" — where CmdOrCtrl resolves to Command on macOS and Control
// elsewhere, so an app declares a shortcut once).

namespace AffineUI;

/// <summary>
/// Standard items whose label, accelerator and behavior the platform supplies.
/// Mirrors <c>affineui_menu_role</c> / <c>affineui::MenuRole</c>. A role item
/// needs no label and no callback.
/// </summary>
public enum MenuRole
{
    None = 0,
    // Application menu (macOS). Quit and About are universal; Services/Hide*
    // are macOS-only and are dropped from a drawn menu rather than shown dead.
    About = 1,
    Services = 2,
    Hide = 3,
    HideOthers = 4,
    Unhide = 5,
    Preferences = 6,
    Quit = 7,
    // Edit. On macOS these get the standard AppKit selectors, so they act on
    // whatever control has focus (native text fields included) for free.
    Undo = 8,
    Redo = 9,
    Cut = 10,
    Copy = 11,
    Paste = 12,
    SelectAll = 13,
    // Window.
    Minimize = 14,
    Zoom = 15,
    Close = 16,
    ToggleFullscreen = 17,
}

/// <summary>
/// One row of a menu. Build with the factories (<see cref="Item"/>,
/// <see cref="Check"/>, <see cref="Separator"/>, <see cref="Sub"/>,
/// <see cref="Role"/>) — they read at the call site the way the C++ and
/// Electron ones do.
///
/// <para>Rows a drawn menu would custom-paint are expressed as DATA, not as
/// arbitrary DOM: <see cref="Checked"/>, <see cref="Swatch"/> (a solid color
/// chip — accent pickers and the like). Each maps to a real NSMenuItem
/// affordance, so a custom-looking row survives the trip to a native menu.</para>
/// </summary>
public sealed class MenuItem
{
    /// <summary>Which of the C ABI's add_* calls emits this row.</summary>
    internal enum Kind { Normal, Check, Separator, Role, Submenu }

    internal Kind ItemKind { get; init; } = Kind.Normal;

    /// <summary>Row label. A role item's is supplied by the platform when empty.</summary>
    public string Label { get; init; } = string.Empty;

    /// <summary>Electron-style chord: "CmdOrCtrl+S", "Shift+Alt+F". Empty for none.</summary>
    public string Accelerator { get; init; } = string.Empty;

    /// <summary>The standard item this row is, if any.</summary>
    public MenuRole ItemRole { get; init; } = MenuRole.None;

    /// <summary>Check mark. Meaningful for <see cref="Check"/> rows.</summary>
    public bool Checked { get; init; }

    /// <summary>Invoked when the row is chosen. Ignored on separators and on
    /// role rows (whose behavior the platform supplies).</summary>
    public Action? OnSelect { get; init; }

    /// <summary>A submenu's children.</summary>
    public IReadOnlyList<MenuItem> Submenu { get; init; } = Array.Empty<MenuItem>();

    /// <summary>Greyed out and unselectable when false. Default true.</summary>
    public bool Enabled { get; set; } = true;

    /// <summary>A solid color chip in the row's leading gutter — accent
    /// pickers, layer colors. Null (the default) draws none.</summary>
    public Color? Swatch { get; set; }

    // ── Builders ─────────────────────────────────────────────────────────
    // Object initializers work too; these just read better at the call site.

    public static MenuItem Item(string label, string accelerator = "", Action? onSelect = null) =>
        new() { Label = label, Accelerator = accelerator, OnSelect = onSelect };

    public static MenuItem Check(string label, bool isChecked, string accelerator = "",
                                 Action? onSelect = null) =>
        new()
        {
            ItemKind = Kind.Check,
            Label = label,
            Checked = isChecked,
            Accelerator = accelerator,
            OnSelect = onSelect,
        };

    public static MenuItem Separator() => new() { ItemKind = Kind.Separator };

    public static MenuItem Sub(string label, params MenuItem[] items) =>
        new() { ItemKind = Kind.Submenu, Label = label, Submenu = items };

    public static MenuItem Sub(string label, IReadOnlyList<MenuItem> items) =>
        new() { ItemKind = Kind.Submenu, Label = label, Submenu = items };

    /// <summary>A standard platform item. Its label and accelerator come from
    /// the shell unless <paramref name="label"/> overrides them.</summary>
    public static MenuItem Role(MenuRole role, string label = "") =>
        new() { ItemKind = Kind.Role, ItemRole = role, Label = label };

    /// <summary>Fluent <see cref="Swatch"/>, for collection initializers.</summary>
    public MenuItem WithSwatch(Color color)
    {
        Swatch = color;
        return this;
    }

    /// <summary>Fluent <see cref="Enabled"/>, for collection initializers.</summary>
    public MenuItem WithEnabled(bool enabled)
    {
        Enabled = enabled;
        return this;
    }

    // ── Standard groups ──────────────────────────────────────────────────

    /// <summary>The conventional Edit menu. On macOS these carry the AppKit
    /// selectors, so they operate on the focused control without app wiring.</summary>
    public static IReadOnlyList<MenuItem> EditMenu() => new[]
    {
        Role(MenuRole.Undo),
        Role(MenuRole.Redo),
        Separator(),
        Role(MenuRole.Cut),
        Role(MenuRole.Copy),
        Role(MenuRole.Paste),
        Role(MenuRole.SelectAll),
    };

    /// <summary>The conventional Window menu.</summary>
    public static IReadOnlyList<MenuItem> WindowMenu() => new[]
    {
        Role(MenuRole.Minimize),
        Role(MenuRole.Zoom),
        Separator(),
        Role(MenuRole.Close),
    };
}

/// <summary>
/// A menu bar: the top-level menus, left to right (<c>affineui::Menu</c>, which
/// is just a list of <see cref="MenuItem"/>). On macOS the FIRST one is the
/// application menu and is titled with the app name whatever its label says —
/// that is a platform rule, not a choice.
/// </summary>
public sealed class Menu : IEnumerable<MenuItem>
{
    private readonly List<MenuItem> _items = new();

    public Menu() { }

    public Menu(IEnumerable<MenuItem> items) => _items.AddRange(items);

    /// <summary>Appends a top-level menu (enables collection-initializer syntax).</summary>
    public void Add(MenuItem item)
    {
        ArgumentNullException.ThrowIfNull(item);
        _items.Add(item);
    }

    public int Count => _items.Count;

    public MenuItem this[int index] => _items[index];

    public IEnumerator<MenuItem> GetEnumerator() => _items.GetEnumerator();

    System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() =>
        GetEnumerator();
}
