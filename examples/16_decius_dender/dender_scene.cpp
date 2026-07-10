#include "dender_scene.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dender {

namespace {

// Strip a trailing ".NNN" numeric suffix so "Cube.001" uniquifies from the
// "Cube" stem (Blender / web uniqueName semantics).
std::string_view name_stem(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size()) return name;
    for (std::size_t i = dot + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return name;
    }
    return name.substr(0, dot);
}

}  // namespace

const std::vector<Primitive>& catalog() {
    static const std::vector<Primitive> kCatalog{
        {"Cube", "Mesh", {0.0, 0.8, 0.0}},
        {"UV Sphere", "Mesh", {0.0, 0.9, 0.0}},
        {"Icosphere", "Mesh", {0.0, 0.9, 0.0}},
        {"Cylinder", "Mesh", {0.0, 0.8, 0.0}},
        {"Cone", "Mesh", {0.0, 0.8, 0.0}},
        {"Torus", "Mesh", {0.0, 0.9, 0.0}},
        {"Plane", "Mesh", {0.0, 0.001, 0.0}},
        {"Point Light", "Light", {2.6, 2.4, 1.2}},
        {"Sun", "Light", {3.0, 4.0, 2.5}},
        {"Spot", "Light", {-2.4, 3.0, 1.8}},
        {"Camera", "", {-3.4, 2.0, -3.2}},
        {"Empty", "", {0.0, 1.0, 0.0}},
    };
    return kCatalog;
}

const Primitive* find_primitive(std::string_view type) {
    for (const auto& p : catalog()) {
        if (p.type == type) return &p;
    }
    return nullptr;
}

std::string_view icon_for(std::string_view type) {
    if (type == "Point Light" || type == "Sun" || type == "Spot") return "light";
    if (type == "Camera") return "camera";
    if (type == "Empty") return "cross-target";
    return "cube";
}

bool is_mesh(std::string_view type) {
    const Primitive* p = find_primitive(type);
    return p != nullptr && p->section == "Mesh";
}

std::vector<OrderedObject> ordered_objects(const app::Document& doc) {
    std::vector<OrderedObject> out;
    out.reserve(doc.objects().size());
    // Depth-first from the roots, registry order within a level (the web's
    // rebuildOutliner walk).
    auto visit = [&](auto& self, std::string_view parent, int depth) -> void {
        for (const auto& o : doc.objects()) {
            if (o.parent != parent) continue;
            out.push_back({&o, depth});
            self(self, o.id, depth + 1);
        }
    };
    visit(visit, std::string_view{}, 0);
    return out;
}

std::string unique_name(const app::Document& doc, std::string_view base,
                        std::string_view ignore_id) {
    auto taken = [&](std::string_view candidate) {
        for (const auto& o : doc.objects()) {
            if (o.id != ignore_id && o.name == candidate) return true;
        }
        return false;
    };
    if (!base.empty() && !taken(base)) return std::string(base);
    const std::string stem(name_stem(base.empty() ? "Object" : base));
    for (int i = 1;; ++i) {
        char suffix[8];
        std::snprintf(suffix, sizeof suffix, ".%03d", i);
        const std::string candidate = stem + suffix;
        if (!taken(candidate)) return candidate;
    }
}

bool parse_bool(std::string_view value) noexcept {
    return value == "true" || value == "1" || value == "on";
}

double parse_double_or(std::string_view value, double fallback) noexcept {
    // strtod, not std::from_chars: Apple's libc++ leaves the floating-point
    // from_chars overload `= delete`'d.
    if (value.empty()) return fallback;
    std::string tmp(value);
    char* end = nullptr;
    errno = 0;
    double out = std::strtod(tmp.c_str(), &end);
    return (end != tmp.c_str() && errno != ERANGE) ? out : fallback;
}

}  // namespace dender
