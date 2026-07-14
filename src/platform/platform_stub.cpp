// Platform seam: the no-op shell.
//
// Compiled everywhere macOS is not. The menu model and the title-bar config are
// platform-neutral by design, so an app declares them once and this file is
// what "not implemented here yet" looks like — the app still runs, it just
// keeps the OS title bar and the drawn in-window menu bar.
//
// Windows (WM_NCCALCSIZE / WM_NCHITTEST for a custom caption, HMENU for a
// native bar) and Linux/X11 (motif hints) plug in here; see affineui#62.

#include "platform/platform.h"

#if !defined(__APPLE__)

namespace affineui::platform {

bool has_native_menus() noexcept { return false; }

void install_menu(const Menu&, std::string_view, RoleHandler) {}

Rect window_controls_rect(TitleBarStyle, Point) { return {}; }

void apply_titlebar_style(TitleBarStyle, Point) {}
void sync_titlebar_chrome(TitleBarStyle, Point) {}

bool begin_window_drag() { return false; }

void minimize_window() {}
void toggle_maximize_window() {}
bool window_is_maximized() { return false; }
void set_window_fullscreen(bool) {}
bool window_is_fullscreen() { return false; }

}  // namespace affineui::platform

#endif  // !__APPLE__
