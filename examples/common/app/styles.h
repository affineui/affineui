#pragma once

// ── DCC template: framework stylesheet loading ──────────────────────────────
//
// EXAMPLE / TEMPLATE CODE. "Init just loads the styles" — rather than depend on
// runtime <link> resolution (which is sensitive to the working directory), an
// app reads its CSS framework bundle from disk at startup and installs it as
// the document stylesheet. The bundle filename is *versioned* (the app declares
// which framework version it ships and was tested against); this helper finds
// that exact file next to the executable (where the build copies frameworks/)
// or under examples/ when run from the repo.
//
// Loose vs strict (matches the framework-version policy): loose (default)
// returns whatever it finds and empty if absent; strict requires the declared
// version's bundle and logs a clear error if it is missing — so an app can fail
// loud rather than render unstyled against the wrong/absent version.

#include <string>
#include <string_view>

#include <affineui/view.h>

namespace app {

/// Read a CSS framework bundle for `theme` at `version` (empty = the theme's
/// default version) from the conventional on-disk locations. Returns the CSS
/// text, or empty if not found.
[[nodiscard]] std::string read_framework_bundle(affineui::ViewTheme theme,
                                                std::string_view version = {});

/// Same, but also reports the directory the bundle was loaded from as a base
/// URL (with a trailing slash). Pass it to App::set_stylesheet / Document::
/// set_user_stylesheet so url()s inside the bundle (e.g. the icon font's
/// url(../fonts/...)) resolve relative to the sheet — standard CSS behavior,
/// identical to a <link>ed sheet. Empty base if the bundle is not found.
[[nodiscard]] std::string read_framework_bundle(affineui::ViewTheme theme,
                                                std::string_view version,
                                                std::string& out_base_url);

/// Strict load: like read_framework_bundle, but if the declared version's
/// bundle is not found it logs an error to stderr and returns empty. Returns
/// whether the bundle was found. Use when an app must run against an exact
/// framework version.
[[nodiscard]] bool require_framework_bundle(affineui::ViewTheme theme,
                                            std::string_view version,
                                            std::string& out_css);

}  // namespace app
