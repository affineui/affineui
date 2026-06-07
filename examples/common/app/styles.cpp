#include "app/styles.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace app {

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

std::string read_framework_bundle(affineui::ViewTheme theme,
                                  std::string_view version) {
    std::string base;
    return read_framework_bundle(theme, version, base);
}

std::string read_framework_bundle(affineui::ViewTheme theme,
                                  std::string_view version,
                                  std::string& out_base_url) {
    out_base_url.clear();
    const std::string href = affineui::framework_bundle_href(theme, version);
    if (href.empty()) return {};  // Plain theme: no bundle.

    // The build copies frameworks/ next to the exe; the repo keeps it under
    // examples/. Try both so the app works from either working directory.
    const std::filesystem::path candidates[] = {
        std::filesystem::path{href},
        std::filesystem::path{"examples"} / href,
    };
    for (const auto& path : candidates) {
        if (std::string css = read_file(path); !css.empty()) {
            // Report the stylesheet's own location so the caller can hand it to
            // the document as the base URL. Per CSS semantics, url()s inside the
            // sheet (e.g. the icon font's url(../fonts/decius-icons.woff2))
            // resolve relative to the sheet's URL — the document resolves them
            // exactly as it does for a <link>ed stylesheet. (Trailing slash so
            // it reads as a directory base.)
            out_base_url = path.parent_path().generic_string();
            if (!out_base_url.empty() && out_base_url.back() != '/') {
                out_base_url.push_back('/');
            }
            return css;
        }
    }
    return {};
}

bool require_framework_bundle(affineui::ViewTheme theme,
                              std::string_view version,
                              std::string& out_css) {
    out_css = read_framework_bundle(theme, version);
    if (out_css.empty()) {
        const std::string href = affineui::framework_bundle_href(theme, version);
        std::fprintf(stderr,
                     "[affineui_app] required stylesheet not found: \"%s\". "
                     "Ship this exact version next to your app, or relax to a "
                     "loose load.\n",
                     href.c_str());
        return false;
    }
    return true;
}

}  // namespace app
