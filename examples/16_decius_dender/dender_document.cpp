#include "dender_document.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace dender {

DenderDocument::DenderDocument()
    : scene_nodes_{
          {"scene", "Scene", 0, true},
          {"collection", "Collection", 1, true},
          {"cube", "Cube", 2, false},
          {"camera", "Camera", 2, false},
          {"key-light", "Key Light", 2, false},
          {"world", "World", 0, false},
      },
      timeline_keys_{
          {18.0},
          {34.0},
          {70.0},
      } {
    graph_.add_input("transform.location.x", "Location X", "Transform", 0.0, "m");
    graph_.add_input("transform.location.y", "Location Y", "Transform", 0.0, "m");
    graph_.add_input("transform.location.z", "Location Z", "Transform", 0.0, "m");
    graph_.add_input("material.tint", "Tint", "Material", std::string{"#e8843a"});
    graph_.add_input("material.roughness", "Roughness", "Material", 0.42);
    graph_.add_input("viewport.show_wireframe", "Wire Overlay", "Viewport", true);
    graph_.add_computed(
        "transform.location",
        "Location", "Transform",
        [](const PropertyGraph& graph) -> PropertyValue {
            return Vec3{
                graph.get_or<double>("transform.location.x", 0.0),
                graph.get_or<double>("transform.location.y", 0.0),
                graph.get_or<double>("transform.location.z", 0.0),
            };
        },
        "m");
    graph_.add_computed(
        "viewport.cube_left", "Viewport X", "Viewport",
        [](const PropertyGraph& graph) -> PropertyValue {
            const auto x = graph.get_or<double>("transform.location.x", 0.0);
            return std::clamp(44.0 + x * 8.0, 18.0, 72.0);
        },
        "%");
    graph_.add_computed(
        "viewport.cube_top", "Viewport Y", "Viewport",
        [](const PropertyGraph& graph) -> PropertyValue {
            const auto z = graph.get_or<double>("transform.location.z", 0.0);
            return std::clamp(36.0 - z * 5.0, 16.0, 64.0);
        },
        "%");
    graph_.add_computed(
        "viewport.cube_style", "Cube Style", "Viewport",
        [](const PropertyGraph& graph) -> PropertyValue {
            const auto left = graph.get_or<double>("viewport.cube_left", 44.0);
            const auto top = graph.get_or<double>("viewport.cube_top", 36.0);
            const auto roughness = graph.get_or<double>("material.roughness", 0.42);
            const auto tint = graph.get_or<std::string>("material.tint",
                                                        std::string{"#e8843a"});
            std::ostringstream style;
            style << "left:" << left << "%;top:" << top
                  << "%;border-color:" << tint
                  << ";opacity:" << std::clamp(1.0 - roughness * 0.18, 0.72, 1.0);
            return style.str();
        });
}

void PropertyGraph::add_input(std::string id,
                              std::string label,
                              std::string category,
                              PropertyValue value,
                              std::string unit) {
    properties_.push_back({
        std::move(id),
        std::move(label),
        std::move(category),
        std::move(unit),
        std::move(value),
        {},
        true,
        false,
        false,
    });
}

void PropertyGraph::add_computed(
        std::string id,
        std::string label,
        std::string category,
        std::function<PropertyValue(const PropertyGraph&)> evaluator,
        std::string unit) {
    properties_.push_back({
        std::move(id),
        std::move(label),
        std::move(category),
        std::move(unit),
        {},
        std::move(evaluator),
        false,
        true,
        false,
    });
}

void PropertyGraph::set(std::string_view id, PropertyValue value) {
    auto* property = find(id);
    if (!property || !property->input) return;
    property->value = std::move(value);
    mark_computed_dirty();
}

const PropertyValue* PropertyGraph::value(std::string_view id) const {
    const auto* property = find(id);
    if (!property) return nullptr;
    if (!property->input && property->dirty && property->evaluator &&
        !property->evaluating) {
        property->evaluating = true;
        property->value = property->evaluator(*this);
        property->dirty = false;
        property->evaluating = false;
    }
    return &property->value;
}

GraphProperty* PropertyGraph::find(std::string_view id) noexcept {
    auto it = std::find_if(properties_.begin(), properties_.end(),
        [&](const GraphProperty& property) { return property.id == id; });
    return it == properties_.end() ? nullptr : &*it;
}

const GraphProperty* PropertyGraph::find(std::string_view id) const noexcept {
    auto it = std::find_if(properties_.begin(), properties_.end(),
        [&](const GraphProperty& property) { return property.id == id; });
    return it == properties_.end() ? nullptr : &*it;
}

void PropertyGraph::mark_computed_dirty() noexcept {
    for (auto& property : properties_) {
        if (!property.input) property.dirty = true;
    }
}

std::string_view DenderDocument::selected_object() const noexcept {
    if (const auto* node = find_node(selected_node_id_)) return node->label;
    return {};
}

std::string DenderDocument::tint() const {
    return graph_.get_or<std::string>("material.tint", std::string{"#e8843a"});
}

double DenderDocument::roughness() const {
    return graph_.get_or<double>("material.roughness", 0.42);
}

Vec3 DenderDocument::location() const {
    return graph_.get_or<Vec3>("transform.location", {});
}

bool DenderDocument::show_wireframe() const {
    return graph_.get_or<bool>("viewport.show_wireframe", true);
}

EvaluatedScene DenderDocument::evaluate_scene() const {
    EvaluatedScene scene;
    scene.location = location();
    scene.tint = tint();
    scene.roughness = roughness();
    scene.show_wireframe = show_wireframe();
    scene.cube_left_percent = graph_.get_or<double>("viewport.cube_left", 44.0);
    scene.cube_top_percent = graph_.get_or<double>("viewport.cube_top", 36.0);
    scene.cube_style = graph_.get_or<std::string>(
        "viewport.cube_style", std::string{"left:44%;top:36%"});
    return scene;
}

void DenderDocument::select_node(std::string_view id) {
    if (find_node(id) != nullptr) selected_node_id_ = std::string(id);
}

void DenderDocument::rename_selected(std::string_view name) {
    if (auto* node = find_node(selected_node_id_)) {
        node->label = std::string(name);
        dirty_ = true;
    }
}

void DenderDocument::reset_selection() {
    if (auto* cube = find_node("cube")) cube->label = "Cube";
    selected_node_id_ = "cube";
    graph_.set("transform.location.x", 0.0);
    graph_.set("transform.location.y", 0.0);
    graph_.set("transform.location.z", 0.0);
    graph_.set("material.roughness", 0.42);
    graph_.set("material.tint", std::string{"#e8843a"});
    graph_.set("viewport.show_wireframe", true);
    dirty_ = false;
}

void DenderDocument::set_tint(std::string_view value) {
    graph_.set("material.tint", std::string(value));
    dirty_ = true;
}

void DenderDocument::set_roughness(double value) noexcept {
    graph_.set("material.roughness", std::clamp(value, 0.0, 1.0));
    dirty_ = true;
}

void DenderDocument::set_location_x(double value) noexcept {
    graph_.set("transform.location.x", value);
    dirty_ = true;
}

void DenderDocument::set_location_y(double value) noexcept {
    graph_.set("transform.location.y", value);
    dirty_ = true;
}

void DenderDocument::set_location_z(double value) noexcept {
    graph_.set("transform.location.z", value);
    dirty_ = true;
}

void DenderDocument::set_show_wireframe(bool value) noexcept {
    graph_.set("viewport.show_wireframe", value);
    dirty_ = true;
}

SceneNode* DenderDocument::find_node(std::string_view id) noexcept {
    auto it = std::find_if(scene_nodes_.begin(), scene_nodes_.end(),
        [&](const SceneNode& node) { return node.id == id; });
    return it == scene_nodes_.end() ? nullptr : &*it;
}

const SceneNode* DenderDocument::find_node(std::string_view id) const noexcept {
    auto it = std::find_if(scene_nodes_.begin(), scene_nodes_.end(),
        [&](const SceneNode& node) { return node.id == id; });
    return it == scene_nodes_.end() ? nullptr : &*it;
}

bool parse_bool(std::string_view value) noexcept {
    return value == "true" || value == "1" || value == "on";
}

}  // namespace dender
