#pragma once

// The platform seam for window chrome and native menus.
//
// The core stays platform-neutral and hands a *model* across this seam; each
// shell translates it (the same layering rule as the IME intents — core states
// what it wants, the shell speaks to the OS). Every function here is a no-op on
// platforms that have no implementation, so callers never branch on the OS.

#include <functional>
#include <string_view>

#include "affineui/app.h"
#include "affineui/menu.h"
#include "affineui/types.h"

namespace affineui::platform {

/// True when this platform owns a system menu bar that set_menu() drives.
/// False everywhere the drawn in-window bar is still the only bar — callers
/// use this to decide whether to suppress the drawn one.
[[nodiscard]] bool has_native_menus() noexcept;

/// Roles the shell cannot service by itself and hands back to the core:
/// the edit group (which must act on the focused DOM text control, not on an
/// AppKit responder) and Quit (which must go through the close-request intent).
using RoleHandler = std::function<void(MenuRole)>;

/// Build and install the application menu. `app_name` titles the macOS
/// application menu. Replaces any previously installed menu.
void install_menu(const Menu& menu, std::string_view app_name,
                  RoleHandler on_role);

/// Apply the window's title-bar style. Called once the native window exists.
void apply_titlebar_style(TitleBarStyle style, Point traffic_light_position);

/// Re-place the traffic lights after a resize (their position is in window
/// coordinates, so a resize moves them off the mark). No-op unless the style
/// is Hidden/HiddenInset with a non-zero position.
void sync_titlebar_chrome(TitleBarStyle style, Point traffic_light_position);

/// A press landed in a `--affineui-app-region: drag` region. Starts a native
/// window drag, or zooms on a double-click — the two things a title bar does.
/// Returns true when the platform took over the press.
bool begin_window_drag();

/// The region the SYSTEM's own window controls occupy, in logical points,
/// relative to the window's top-left — the space an app-drawn title bar must
/// not put anything into. Empty when the system draws none (TitleBarStyle::
/// Default, where the OS owns the whole bar, and Frameless, where the app
/// draws the buttons itself).
///
/// On macOS the traffic lights sit at the LEFT, so this is a left inset; on
/// Windows the caption buttons sit at the right. The App turns it into the
/// `--affineui-titlebar-area-*` CSS variables so a bar can pad itself
/// correctly on every platform without knowing which side it's on.
[[nodiscard]] Rect window_controls_rect(TitleBarStyle style,
                                        Point traffic_light_position);

// ─── Window controls ──────────────────────────────────────────────────
// What an app-drawn close/minimize/maximize button has to call. The window
// is the OS's, so these have to cross the seam even when the button that
// triggered them was painted by us.

void minimize_window();
/// Toggle zoomed/maximized, the way the OS's own button does.
void toggle_maximize_window();
[[nodiscard]] bool window_is_maximized();
void set_window_fullscreen(bool on);
[[nodiscard]] bool window_is_fullscreen();

}  // namespace affineui::platform
