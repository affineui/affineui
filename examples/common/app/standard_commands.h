#pragma once

// ── DCC template: standard edit commands ────────────────────────────────────
//
// EXAMPLE / TEMPLATE CODE. Registers the everyday edit commands every DCC tool
// has — delete / duplicate / cut / copy / paste, plus undo / redo wired to the
// command stack — into a CommandRegistry, with sensible enabled-state (e.g.
// "delete" disabled when nothing is selected). They operate over the current
// Selection + Document, and each is undoable like any other command.
//
// Call register_standard_commands(ctx) once at startup, then bind ids to keys
// and reference them from menus. The command ids and label keys are public so
// apps can rebind, relabel (via localization), or override them.

#include <string_view>

#include "app/context.h"
#include "app/localization.h"
#include "affineui/keymap.h"

namespace app {

// Stable command ids. (Also the scripting names.)
namespace cmd {
inline constexpr std::string_view undo      = "edit.undo";
inline constexpr std::string_view redo      = "edit.redo";
inline constexpr std::string_view delete_   = "edit.delete";
inline constexpr std::string_view duplicate = "edit.duplicate";
inline constexpr std::string_view cut       = "edit.cut";
inline constexpr std::string_view copy      = "edit.copy";
inline constexpr std::string_view paste     = "edit.paste";
inline constexpr std::string_view select_all = "edit.selectAll";
}  // namespace cmd

/// Register the standard edit commands into ctx.registry().
void register_standard_commands(Context& ctx);

/// Add the conventional accelerators for the standard commands to a keymap
/// (Ctrl Z / Ctrl Shift Z / Del / Ctrl D / Ctrl X / Ctrl C / Ctrl V / Ctrl A).
/// Keymap lives in the AffineUI library (affineui::Keymap), not this template.
void register_standard_keys(affineui::Keymap& keymap);

/// Add the standard command label strings to a locale catalog (English here);
/// apps merge their own and additional locales. Returns the catalog so it can
/// be passed straight to Localizer::add_locale.
[[nodiscard]] Localizer::Catalog standard_command_labels_en();

}  // namespace app
