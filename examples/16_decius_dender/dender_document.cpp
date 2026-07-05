#include "dender_document.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

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

DenderDocument::DenderDocument() {
    // The web sample's exact initial scene (viewport.js boot): Cube at y=0.8
    // (active), a point light named "Light", and the camera.
    const std::string cube = add("Cube", "Cube");
    add("Point Light", "Light");
    add("Camera", "Camera");
    select(cube);
}

const std::vector<Primitive>& DenderDocument::catalog() {
    // The runtime Add menu from app.js (NOT the static Blender-style HTML) with
    // viewport.js's default spawn positions.
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

const Primitive* DenderDocument::find_primitive(std::string_view type) {
    for (const auto& p : catalog()) {
        if (p.type == type) return &p;
    }
    return nullptr;
}

std::string_view DenderDocument::icon_for(std::string_view type) {
    if (type == "Point Light" || type == "Sun" || type == "Spot") return "light";
    if (type == "Camera") return "camera";
    if (type == "Empty") return "cross-target";
    return "cube";
}

bool DenderDocument::is_mesh(std::string_view type) {
    const Primitive* p = find_primitive(type);
    return p != nullptr && p->section == "Mesh";
}

const SceneObject* DenderDocument::find(std::string_view id) const noexcept {
    for (const auto& o : objects_) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

SceneObject* DenderDocument::find_mutable(std::string_view id) noexcept {
    for (auto& o : objects_) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

std::vector<DenderDocument::OrderedObject> DenderDocument::ordered_objects()
    const {
    std::vector<OrderedObject> out;
    out.reserve(objects_.size());
    // Depth-first from the roots, registry order within a level (the web's
    // rebuildOutliner walk).
    auto visit = [&](auto& self, std::string_view parent, int depth) -> void {
        for (const auto& o : objects_) {
            if (o.parent != parent) continue;
            out.push_back({&o, depth});
            self(self, o.id, depth + 1);
        }
    };
    visit(visit, "", 0);
    return out;
}

std::string DenderDocument::next_id() {
    return "obj_" + std::to_string(++counter_);
}

std::string DenderDocument::unique_name(std::string_view base,
                                        std::string_view ignore_id) const {
    auto taken = [&](std::string_view candidate) {
        for (const auto& o : objects_) {
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

std::string DenderDocument::add(std::string_view type, std::string_view name) {
    const Primitive* primitive = find_primitive(type);
    if (primitive == nullptr) return {};
    SceneObject object;
    object.id = next_id();
    object.type = std::string(type);
    object.name = unique_name(name.empty() ? type : name, object.id);
    object.position = primitive->spawn;
    objects_.push_back(std::move(object));
    return objects_.back().id;
}

DenderDocument::RemovedObject DenderDocument::remove(std::string_view id) {
    RemovedObject removed;
    const auto it = std::find_if(objects_.begin(), objects_.end(),
                                 [&](const SceneObject& o) { return o.id == id; });
    if (it == objects_.end()) return removed;
    removed.object = *it;
    removed.index = static_cast<std::size_t>(it - objects_.begin());
    removed.was_active = active_;
    removed.valid = true;
    objects_.erase(it);
    // Re-home children to the removed object's parent (web VP.remove).
    for (auto& o : objects_) {
        if (o.parent == id) {
            removed.rehomed_children.push_back(o.id);
            o.parent = removed.object.parent;
        }
    }
    if (active_ == id) active_.clear();
    multi_.erase(std::remove(multi_.begin(), multi_.end(), std::string(id)),
                 multi_.end());
    return removed;
}

void DenderDocument::restore(const RemovedObject& removed) {
    if (!removed.valid) return;
    const std::size_t index = std::min(removed.index, objects_.size());
    objects_.insert(objects_.begin() + static_cast<std::ptrdiff_t>(index),
                    removed.object);
    for (const auto& child : removed.rehomed_children) {
        if (auto* o = find_mutable(child)) o->parent = removed.object.id;
    }
    active_ = removed.was_active;
}

std::string DenderDocument::duplicate(std::string_view id) {
    const SceneObject* source = find(id);
    if (source == nullptr) return {};
    SceneObject copy = *source;
    copy.id = next_id();
    copy.name = unique_name(source->name, copy.id);
    copy.position.x += 0.5;  // web Shift+D offset
    objects_.push_back(std::move(copy));
    return objects_.back().id;
}

std::string DenderDocument::rename(std::string_view id, std::string_view name) {
    auto* object = find_mutable(id);
    if (object == nullptr) return {};
    object->name = unique_name(name.empty() ? object->type : name, id);
    return object->name;
}

bool DenderDocument::is_ancestor(std::string_view maybe_ancestor,
                                 std::string_view id) const {
    const SceneObject* o = find(id);
    while (o != nullptr && !o->parent.empty()) {
        if (o->parent == maybe_ancestor) return true;
        o = find(o->parent);
    }
    return false;
}

bool DenderDocument::reparent(std::string_view id, std::string_view new_parent) {
    auto* object = find_mutable(id);
    if (object == nullptr) return false;
    if (!new_parent.empty()) {
        if (find(new_parent) == nullptr) return false;
        if (new_parent == id || is_ancestor(id, new_parent)) return false;
    }
    object->parent = std::string(new_parent);
    return true;
}

double DenderDocument::axis(const Vec3& v, int axis) noexcept {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

namespace {
void set_axis(Vec3& v, int axis, double value) noexcept {
    (axis == 0 ? v.x : (axis == 1 ? v.y : v.z)) = value;
}
}  // namespace

bool DenderDocument::set_position(std::string_view id, int axis, double value) {
    auto* o = find_mutable(id);
    if (o == nullptr || axis < 0 || axis > 2) return false;
    set_axis(o->position, axis, value);
    return true;
}

bool DenderDocument::set_rotation(std::string_view id, int axis, double radians) {
    auto* o = find_mutable(id);
    if (o == nullptr || axis < 0 || axis > 2) return false;
    set_axis(o->rotation, axis, radians);
    return true;
}

bool DenderDocument::set_scale(std::string_view id, int axis, double value) {
    auto* o = find_mutable(id);
    if (o == nullptr || axis < 0 || axis > 2) return false;
    set_axis(o->scale, axis, value);
    return true;
}

bool DenderDocument::is_selected(std::string_view id) const noexcept {
    if (id == active_) return true;
    return std::find(multi_.begin(), multi_.end(), id) != multi_.end();
}

void DenderDocument::select(std::string_view id) {
    if (find(id) == nullptr) return;
    active_ = std::string(id);
    multi_.clear();
}

void DenderDocument::toggle_multi(std::string_view id) {
    if (find(id) == nullptr || id == active_) return;
    const auto it = std::find(multi_.begin(), multi_.end(), id);
    if (it == multi_.end()) multi_.emplace_back(id);
    else multi_.erase(it);
}

void DenderDocument::deselect() {
    active_.clear();
    multi_.clear();
}

void DenderDocument::scrub_frame(int frame) noexcept {
    timeline_.frame = std::clamp(frame, timeline_.start, timeline_.end);
}

void DenderDocument::set_frame(int frame) noexcept {
    timeline_.frame = frame;
    if (timeline_.start > frame) timeline_.start = frame;
    if (timeline_.end < frame) timeline_.end = frame;
}

void DenderDocument::set_start(int start) noexcept {
    timeline_.start = start;
    if (timeline_.end < start) timeline_.end = start;
    if (timeline_.frame < start) timeline_.frame = start;
}

void DenderDocument::set_end(int end) noexcept {
    timeline_.end = end;
    if (timeline_.start > end) timeline_.start = end;
    if (timeline_.frame > end) timeline_.frame = end;
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
