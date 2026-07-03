#include "app/styles.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace app {

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Directory containing the running executable (empty on failure). Assets
// shipped beside the app must resolve independently of the CWD — a tool
// launched from a shortcut, a shell in another directory, or a debugger
// all see different working directories.
std::filesystem::path executable_dir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
#elif defined(__APPLE__)
    char buf[4096]{};
    std::uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(buf, ec);
    return ec ? std::filesystem::path(buf).parent_path()
              : canon.parent_path();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : p.parent_path();
#endif
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

    // Resolution order: the frameworks/ copy shipped BESIDE THE EXE first
    // (deterministic regardless of CWD, and the build keeps it fresh via a
    // file-dependency stamp), then CWD-relative for running from a build
    // or examples directory, then the repo layout for running from the
    // repo root.
    const std::filesystem::path exe_dir = executable_dir();
    std::vector<std::filesystem::path> candidates;
    if (!exe_dir.empty()) candidates.push_back(exe_dir / href);
    candidates.emplace_back(href);
    candidates.emplace_back(std::filesystem::path{"examples"} / href);
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
