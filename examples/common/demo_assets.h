#pragma once

#include <affineui/affineui.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace demo {

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.good()) return {};
    std::stringstream bytes;
    bytes << file.rdbuf();
    return bytes.str();
}

inline std::vector<std::filesystem::path> resource_roots() {
    return {
        std::filesystem::path{AFFINEUI_EXAMPLES_SOURCE_DIR},
        std::filesystem::current_path(),
        std::filesystem::current_path() / "examples",
        std::filesystem::current_path() / ".." / "examples",
        std::filesystem::current_path() / ".." / ".." / "examples",
        std::filesystem::current_path() / ".." / ".." / ".." / "examples",
    };
}

inline std::string read_first_existing(std::initializer_list<std::string_view> paths) {
    for (auto path : paths) {
        if (auto bytes = read_file(std::filesystem::path{std::string(path)}); !bytes.empty()) {
            return bytes;
        }
        for (const auto& root : resource_roots()) {
            if (auto bytes = read_file(root / std::string(path)); !bytes.empty()) {
                return bytes;
            }
        }
    }
    return {};
}

inline bool local_resource_url(std::string_view url) {
    return !url.empty() &&
           url.find("://") == std::string_view::npos &&
           url.rfind("data:", 0) != 0;
}

inline void install_resource_loader(affineui::Ui& ui) {
    ui.document().set_resource_loader([](std::string_view url) -> std::string {
        if (!local_resource_url(url)) return {};
        const std::string raw{url};
        for (const auto& root : resource_roots()) {
            if (auto bytes = read_file(root / raw); !bytes.empty()) {
                return bytes;
            }
        }
        return {};
    });
}

inline std::string html_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

}  // namespace demo
