#pragma once

#include <functional>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dender {

struct SceneNode {
    std::string id;
    std::string label;
    int depth{0};
    bool branch{false};
};

struct TimelineKey {
    double position_percent{0.0};
};

struct Vec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

using PropertyValue = std::variant<bool, double, std::string, Vec3>;

struct GraphProperty {
    std::string id;
    std::string label;
    std::string category;
    std::string unit;
    mutable PropertyValue value;
    std::function<PropertyValue(const class PropertyGraph&)> evaluator;
    bool input{true};
    mutable bool dirty{false};
    mutable bool evaluating{false};
};

class PropertyGraph {
public:
    void add_input(std::string id,
                   std::string label,
                   std::string category,
                   PropertyValue value,
                   std::string unit = {});
    void add_computed(
        std::string id,
        std::string label,
        std::string category,
        std::function<PropertyValue(const PropertyGraph&)> evaluator,
        std::string unit = {});

    void set(std::string_view id, PropertyValue value);
    [[nodiscard]] const PropertyValue* value(std::string_view id) const;

    template <typename T>
    [[nodiscard]] T get_or(std::string_view id, T fallback) const {
        const auto* v = value(id);
        if (!v) return fallback;
        if (const auto* typed = std::get_if<T>(v)) return *typed;
        return fallback;
    }

    [[nodiscard]] const std::vector<GraphProperty>& properties() const noexcept {
        return properties_;
    }

private:
    [[nodiscard]] GraphProperty* find(std::string_view id) noexcept;
    [[nodiscard]] const GraphProperty* find(std::string_view id) const noexcept;
    void mark_computed_dirty() noexcept;

    std::vector<GraphProperty> properties_;
};

struct EvaluatedScene {
    Vec3 location{};
    std::string tint;
    double roughness{0.0};
    bool show_wireframe{false};
    double cube_left_percent{44.0};
    double cube_top_percent{36.0};
    std::string cube_style;
};

class DenderDocument {
public:
    DenderDocument();

    [[nodiscard]] const std::vector<SceneNode>& scene_nodes() const noexcept {
        return scene_nodes_;
    }
    [[nodiscard]] const std::vector<TimelineKey>& timeline_keys() const noexcept {
        return timeline_keys_;
    }

    [[nodiscard]] std::string_view selected_node_id() const noexcept {
        return selected_node_id_;
    }
    [[nodiscard]] std::string_view selected_object() const noexcept;
    [[nodiscard]] std::string_view title() const noexcept { return title_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] std::string tint() const;
    [[nodiscard]] double roughness() const;
    [[nodiscard]] Vec3 location() const;
    [[nodiscard]] bool show_wireframe() const;
    [[nodiscard]] EvaluatedScene evaluate_scene() const;
    [[nodiscard]] const PropertyGraph& graph() const noexcept { return graph_; }

    void select_node(std::string_view id);
    void rename_selected(std::string_view name);
    void reset_selection();
    void set_tint(std::string_view value);
    void set_roughness(double value) noexcept;
    void set_location_x(double value) noexcept;
    void set_location_y(double value) noexcept;
    void set_location_z(double value) noexcept;
    void set_show_wireframe(bool value) noexcept;

private:
    [[nodiscard]] SceneNode* find_node(std::string_view id) noexcept;
    [[nodiscard]] const SceneNode* find_node(std::string_view id) const noexcept;

    std::vector<SceneNode> scene_nodes_;
    std::vector<TimelineKey> timeline_keys_;
    std::string selected_node_id_{"cube"};
    std::string title_{"Blockout Scene.dndr"};
    bool dirty_{false};
    PropertyGraph graph_;
};

[[nodiscard]] bool parse_bool(std::string_view value) noexcept;

}  // namespace dender
