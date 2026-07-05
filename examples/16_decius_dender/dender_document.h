#pragma once

// DENDER scene document — the app-side model behind the Blender-style
// showcase. An object registry (id / unique name / type / parent / TRS),
// selection, the primitive catalog with the web sample's spawn positions,
// plus the viewport / timeline / theme state the chrome reflects. Mirrors
// the web reference's viewport.js registry semantics (obj_N ids, .001 name
// suffixes, re-homing children on delete, cycle-safe reparent).

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dender {

struct Vec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

struct SceneObject {
    std::string id;      // "obj_N" (monotonic)
    std::string name;    // unique within the scene ("Cube", "Cube.001", …)
    std::string type;    // primitive catalog key ("Cube", "Point Light", …)
    std::string parent;  // parent object id; empty = scene-collection root
    Vec3 position{};
    Vec3 rotation{};     // radians (the inspector displays degrees)
    Vec3 scale{1.0, 1.0, 1.0};
};

/// One entry of the Add-menu catalog (the web app.js runtime menu).
struct Primitive {
    std::string_view type;
    std::string_view section;  // "Mesh" / "Light" / "" (top level)
    Vec3 spawn;
};

enum class TransformMode { Translate, Rotate, Scale };
enum class Shading { Wireframe, Solid, Texture, Rendered };

struct Timeline {
    int start{1};
    int end{250};
    int frame{24};
    std::vector<int> keys{1, 24, 48, 72, 96, 130, 175, 220};
};

struct ThemeState {
    std::string accent{"orange"};
    std::string density{"comfortable"};
    std::string style{"flat"};  // "flat" or "3d"
};

class DenderDocument {
public:
    DenderDocument();

    // ── Registry ─────────────────────────────────────────────────────────
    [[nodiscard]] const std::vector<SceneObject>& objects() const noexcept {
        return objects_;
    }
    [[nodiscard]] const SceneObject* find(std::string_view id) const noexcept;
    [[nodiscard]] std::size_t object_count() const noexcept {
        return objects_.size();
    }
    /// Outliner order: depth-first from the collection roots, registry order
    /// within a level. `depth` is 0 for roots (the outliner adds its own base
    /// indent for Scene > Collection).
    struct OrderedObject {
        const SceneObject* object{nullptr};
        int depth{0};
    };
    [[nodiscard]] std::vector<OrderedObject> ordered_objects() const;

    /// Add a primitive from the catalog (web default spawn position). Returns
    /// the new object's id, or empty for an unknown type. `name` empty = the
    /// type name (uniquified).
    std::string add(std::string_view type, std::string_view name = {});

    /// Everything remove() undid, so a command can restore it exactly.
    struct RemovedObject {
        SceneObject object;
        std::size_t index{0};
        std::vector<std::string> rehomed_children;  // re-homed to obj's parent
        std::string was_active;
        bool valid{false};
    };
    RemovedObject remove(std::string_view id);
    void restore(const RemovedObject& removed);

    /// Duplicate: copy at x+0.5 with a uniquified name (web Shift+D). Returns
    /// the new id (empty if the source is unknown).
    std::string duplicate(std::string_view id);

    /// Rename (uniquified against the other objects). Returns the applied name.
    std::string rename(std::string_view id, std::string_view name);

    /// Cycle-safe reparent (`new_parent` empty = collection root). Returns
    /// false when it would create a cycle or ids are unknown.
    bool reparent(std::string_view id, std::string_view new_parent);

    bool set_position(std::string_view id, int axis, double value);
    bool set_rotation(std::string_view id, int axis, double radians);
    bool set_scale(std::string_view id, int axis, double value);
    [[nodiscard]] static double axis(const Vec3& v, int axis) noexcept;

    // ── Selection ────────────────────────────────────────────────────────
    [[nodiscard]] std::string_view active_id() const noexcept { return active_; }
    [[nodiscard]] const SceneObject* active() const noexcept {
        return find(active_);
    }
    [[nodiscard]] bool is_selected(std::string_view id) const noexcept;
    void select(std::string_view id);        // active + clears the multi set
    void toggle_multi(std::string_view id);  // shift-select membership
    void deselect();

    // ── Viewport state ───────────────────────────────────────────────────
    [[nodiscard]] TransformMode transform_mode() const noexcept { return mode_; }
    void set_transform_mode(TransformMode mode) noexcept { mode_ = mode; }
    [[nodiscard]] Shading shading() const noexcept { return shading_; }
    void set_shading(Shading shading) noexcept { shading_ = shading; }
    [[nodiscard]] bool wireframe() const noexcept {
        return shading_ == Shading::Wireframe;
    }

    // ── Timeline ─────────────────────────────────────────────────────────
    [[nodiscard]] const Timeline& timeline() const noexcept { return timeline_; }
    /// Scrub: clamp to [start, end] (the web ruler drag).
    void scrub_frame(int frame) noexcept;
    /// Combo edits cross-validate so start <= frame <= end always holds —
    /// nudging one past a sibling pushes the sibling (web wireFrameValidation).
    void set_frame(int frame) noexcept;
    void set_start(int start) noexcept;
    void set_end(int end) noexcept;
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    void set_playing(bool playing) noexcept { playing_ = playing; }

    // ── Theme ────────────────────────────────────────────────────────────
    [[nodiscard]] const ThemeState& theme() const noexcept { return theme_; }
    void set_accent(std::string_view accent) { theme_.accent = accent; }
    void set_density(std::string_view density) { theme_.density = density; }
    void set_style(std::string_view style) { theme_.style = style; }

    // ── Catalog / presentation helpers ───────────────────────────────────
    [[nodiscard]] static const std::vector<Primitive>& catalog();
    [[nodiscard]] static const Primitive* find_primitive(std::string_view type);
    /// Decius icon glyph for an object type (web ICON_FOR).
    [[nodiscard]] static std::string_view icon_for(std::string_view type);
    [[nodiscard]] static bool is_mesh(std::string_view type);

private:
    [[nodiscard]] SceneObject* find_mutable(std::string_view id) noexcept;
    [[nodiscard]] std::string unique_name(std::string_view base,
                                          std::string_view ignore_id) const;
    [[nodiscard]] std::string next_id();
    [[nodiscard]] bool is_ancestor(std::string_view maybe_ancestor,
                                   std::string_view id) const;

    std::vector<SceneObject> objects_;
    std::vector<std::string> multi_;  // shift-selected in addition to active_
    std::string active_;
    int counter_{0};

    TransformMode mode_{TransformMode::Translate};
    Shading shading_{Shading::Solid};
    Timeline timeline_{};
    bool playing_{false};
    ThemeState theme_{};
};

[[nodiscard]] bool parse_bool(std::string_view value) noexcept;
[[nodiscard]] double parse_double_or(std::string_view value,
                                     double fallback) noexcept;

}  // namespace dender
