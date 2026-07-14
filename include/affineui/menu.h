#pragma once

// Application menus for AffineUI.
//
// A platform-neutral menu *model* that each shell translates: on macOS it
// becomes a real NSMenu installed as `NSApp.mainMenu` (the system bar at the
// top of the screen, which an app cannot draw itself); elsewhere it drives the
// in-window bar the demos already draw. The app declares its menus once.
//
// The shape deliberately mirrors Electron's `Menu.buildFromTemplate`, because
// that vocabulary is already in everyone's head and the mapping to AppKit is
// the one Electron itself uses:
//
//   app.set_menu({
//       MenuItem::sub("File", {
//           MenuItem::item("New Scene", "CmdOrCtrl+N", [&] { new_scene(); }),
//           MenuItem::separator(),
//           MenuItem::role(MenuRole::Quit),          // standard, auto-labelled
//       }),
//       MenuItem::sub("Edit", MenuItem::edit_menu()),  // Undo/Cut/Copy/Paste...
//   });
//
// Two things carry the weight:
//
//   * `role` — a standard item whose label, accelerator and behavior the
//     platform supplies. This is how the macOS application menu (About /
//     Services / Hide / Quit) and a working Edit menu (Cut/Copy/Paste wired to
//     the focused control, plus macOS's built-in Emoji & Symbols and dictation
//     entries) come out right without the app restating them per platform.
//
//   * `accelerator` — an Electron-style chord string ("CmdOrCtrl+S",
//     "Shift+CmdOrCtrl+Z"). `CmdOrCtrl` resolves to Command on macOS and
//     Control elsewhere, so an app declares a shortcut once.
//
// Rows that a drawn menu would custom-paint are expressed as data, not as
// arbitrary DOM: `checked` (a check mark), `icon` (a named glyph), and
// `swatch` (a solid color chip — accent pickers and the like). Each maps to a
// real NSMenuItem affordance, so custom-looking rows survive the trip to a
// native menu. Anything a platform cannot represent degrades to a plain
// labelled row rather than disappearing.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "affineui/types.h"

namespace affineui {

/// Standard items whose label/accelerator/behavior the platform supplies.
/// A role item needs no label and no callback; give it either and yours wins.
enum class MenuRole {
    None,
    // Application menu (macOS). Elsewhere these are still meaningful — Quit
    // and About are universal — but Services/Hide* are macOS-only and are
    // dropped from a drawn menu rather than shown dead.
    About,
    Services,
    Hide,
    HideOthers,
    Unhide,
    Preferences,
    Quit,
    // Edit. On macOS these get the standard AppKit selectors, so they act on
    // whatever control has focus (including native text fields) for free.
    Undo,
    Redo,
    Cut,
    Copy,
    Paste,
    SelectAll,
    // Window.
    Minimize,
    Zoom,
    Close,
    ToggleFullscreen,
};

enum class MenuItemType {
    Normal,
    Separator,
    Checkbox,
    Radio,
};

struct MenuItem {
    std::string  label;
    /// Electron-style chord: "CmdOrCtrl+S", "Shift+Alt+F". Empty for none.
    std::string  accelerator;
    MenuRole     item_role{MenuRole::None};
    MenuItemType type{MenuItemType::Normal};
    bool         enabled{true};
    bool         visible{true};
    /// Check mark. Meaningful for Checkbox/Radio; ignored for Normal.
    bool         checked{false};
    /// Named glyph (the same icon names the drawn menus use), or empty.
    std::string  icon;
    /// Solid color chip shown in the item's leading gutter — accent pickers,
    /// layer colors. Only drawn when `swatch.a` is non-zero.
    Color        swatch{0, 0, 0, 0};
    std::function<void()> on_select;
    std::vector<MenuItem> submenu;

    // ─── Builders ─────────────────────────────────────────────────────
    // Aggregate init works too; these just read better at the call site.

    [[nodiscard]] static MenuItem item(std::string label,
                                       std::string accelerator = {},
                                       std::function<void()> on_select = {}) {
        MenuItem m;
        m.label       = std::move(label);
        m.accelerator = std::move(accelerator);
        m.on_select   = std::move(on_select);
        return m;
    }

    [[nodiscard]] static MenuItem separator() {
        MenuItem m;
        m.type = MenuItemType::Separator;
        return m;
    }

    [[nodiscard]] static MenuItem sub(std::string label,
                                      std::vector<MenuItem> items) {
        MenuItem m;
        m.label   = std::move(label);
        m.submenu = std::move(items);
        return m;
    }

    /// A standard platform item. Label/accelerator are supplied by the shell
    /// unless you override them.
    [[nodiscard]] static MenuItem role(MenuRole r, std::string label = {}) {
        MenuItem m;
        m.item_role = r;
        m.label     = std::move(label);
        return m;
    }

    [[nodiscard]] static MenuItem check(std::string label, bool checked,
                                        std::string accelerator = {},
                                        std::function<void()> on_select = {}) {
        MenuItem m       = item(std::move(label), std::move(accelerator),
                                std::move(on_select));
        m.type           = MenuItemType::Checkbox;
        m.checked        = checked;
        return m;
    }

    // ─── Standard groups ──────────────────────────────────────────────

    /// The conventional Edit menu. On macOS these carry the AppKit selectors,
    /// so they operate on the focused control without app wiring.
    [[nodiscard]] static std::vector<MenuItem> edit_menu() {
        return {
            MenuItem::role(MenuRole::Undo),
            MenuItem::role(MenuRole::Redo),
            MenuItem::separator(),
            MenuItem::role(MenuRole::Cut),
            MenuItem::role(MenuRole::Copy),
            MenuItem::role(MenuRole::Paste),
            MenuItem::role(MenuRole::SelectAll),
        };
    }

    /// The conventional Window menu.
    [[nodiscard]] static std::vector<MenuItem> window_menu() {
        return {
            MenuItem::role(MenuRole::Minimize),
            MenuItem::role(MenuRole::Zoom),
            MenuItem::separator(),
            MenuItem::role(MenuRole::Close),
        };
    }
};

/// A menu bar: the top-level menus, left to right. On macOS the first one is
/// the application menu and is titled with the app name whatever its label
/// says — that is a platform rule, not a choice.
using Menu = std::vector<MenuItem>;

// ─── Accelerators ─────────────────────────────────────────────────────

/// A parsed accelerator. `key` is the normalized key token ("S", "F5",
/// "Enter", "["), uppercased for letters; empty when the string had no key.
struct Accelerator {
    bool        ctrl{false};
    bool        shift{false};
    bool        alt{false};
    /// Command on macOS, the Windows/Super key elsewhere.
    bool        super{false};
    std::string key;

    [[nodiscard]] bool valid() const noexcept { return !key.empty(); }
};

/// Parse an Electron-style accelerator ("Shift+CmdOrCtrl+Z").
///
/// `CmdOrCtrl`/`CommandOrControl` resolves to Command on macOS and Control
/// elsewhere — the point of the token is that an app writes the shortcut once.
/// `Cmd`/`Command`/`Super`/`Meta` are Command; `Ctrl`/`Control`; `Alt`/`Option`;
/// `Shift`. Unknown tokens are treated as the key. Returns an Accelerator whose
/// `valid()` is false when no key was found.
[[nodiscard]] Accelerator parse_accelerator(std::string_view spec);

/// Human-readable text for a drawn menu's shortcut column. On macOS this is
/// the glyph form ("⇧⌘Z"); elsewhere the spelled form ("Ctrl+Shift+Z").
[[nodiscard]] std::string accelerator_text(const Accelerator& accel);

}  // namespace affineui
