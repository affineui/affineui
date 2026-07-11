// e3d_controls.h — camera and object manipulation controllers, ported
// from three.js r170 addons (OrbitControls.js, TransformControls.js).
//
// Both controllers are engine-agnostic about input: the hosting
// viewport feeds them pointer positions (pixels for orbit, NDC for the
// gizmo) and reads back whether the event was consumed. Neither knows
// about affineui events — the viewport glue does that translation.
#pragma once

#include <functional>
#include <limits>
#include <memory>
#include <string>

#include "e3d_raycast.h"
#include "e3d_scene.h"

namespace e3d {

// ── OrbitControls ───────────────────────────────────────────────────

/// Orbit / dolly / pan camera rig around a target point, +Y up.
/// Feed pointer events, then call update() once per frame (required
/// when damping is on; harmless otherwise). update() returns true when
/// the camera actually moved — use that to request a repaint.
class OrbitControls {
public:
    enum class Action { None, Rotate, Dolly, Pan };

    explicit OrbitControls(std::shared_ptr<PerspectiveCamera> camera)
        : camera_(std::move(camera)) {}

    Vec3  target;
    bool  enabled{true};
    float min_distance{0.0f};
    float max_distance{std::numeric_limits<float>::infinity()};
    float min_polar_angle{0.0f};
    float max_polar_angle{kPi};
    bool  enable_damping{false};
    float damping_factor{0.05f};
    bool  enable_zoom{true};
    float zoom_speed{1.0f};
    bool  enable_rotate{true};
    float rotate_speed{1.0f};
    bool  enable_pan{true};
    float pan_speed{1.0f};
    bool  screen_space_panning{true};
    // Mouse button → action mapping (three.js controls.mouseButtons).
    Action button_left{Action::Rotate};
    Action button_middle{Action::Dolly};
    Action button_right{Action::Pan};

    /// Viewport size in pixels; needed to convert pointer deltas.
    void set_viewport(float width, float height) {
        view_w_ = width;
        view_h_ = height;
    }

    /// Pointer input, in viewport-local pixels. button: 0 left,
    /// 1 middle, 2 right. Modifier ctrl/meta/shift turns a left-drag
    /// into a pan, like the JS controls.
    void pointer_down(float x, float y, int button, bool pan_modifier = false);
    void pointer_move(float x, float y);
    void pointer_up();
    /// Wheel scroll (positive delta_y = away/zoom out, browser-style).
    void wheel(float delta_y);

    /// Apply accumulated deltas (and damping). True if camera moved.
    bool update();

    bool  dragging() const { return state_ != Action::None; }
    float distance() const {
        return camera_->position.distance_to(target);
    }
    PerspectiveCamera& camera() { return *camera_; }

private:
    void rotate_delta(float dx, float dy);
    void pan_delta(float dx, float dy);
    void dolly(float scale);

    std::shared_ptr<PerspectiveCamera> camera_;
    Action    state_{Action::None};
    float     view_w_{1.0f}, view_h_{1.0f};
    Vec2      last_pointer_;
    Spherical spherical_delta_;
    Vec3      pan_offset_;
    float     scale_{1.0f};
    Vec3      last_position_;
    Quat      last_quaternion_;
    Vec3      last_target_;
};

// ── TransformControls ───────────────────────────────────────────────

/// Translate / rotate / scale gizmo (three.js r160+ shape: the
/// controller is separate from its renderable helper). Add helper()
/// to the SCENE ROOT (its parts are posed in world space), attach() a
/// target, feed NDC pointer events, and call update() each frame
/// before rendering so the gizmo tracks the object and camera.
///
/// The drag-guide helper lines of the JS original are not ported; the
/// gizmo handles, hidden pickers, hover highlight and the full drag
/// math are.
class TransformControls {
public:
    enum class Mode { Translate, Rotate, Scale };
    enum class Space { World, Local };

    explicit TransformControls(std::shared_ptr<PerspectiveCamera> camera);

    /// Renderable gizmo root; add to the scene root once.
    const ObjectPtr& helper() const { return root_; }

    void attach(ObjectPtr object);
    void detach();
    const ObjectPtr& object() const { return object_; }

    Mode  mode() const { return mode_; }
    void  set_mode(Mode mode);
    Space space() const { return space_; }
    void  set_space(Space space) { space_ = space; }
    void  set_size(float size) { size_ = size; }

    float translation_snap{0.0f};  // 0 = off
    float rotation_snap{0.0f};
    float scale_snap{0.0f};
    bool  enabled{true};

    bool dragging() const { return dragging_; }
    /// Hovered/active handle name ("X", "XY", "XYZ", "E", ...), empty
    /// when none.
    const std::string& axis() const { return axis_; }

    std::function<void(bool)> on_dragging_changed;
    std::function<void()>     on_object_change;

    /// Pointer input in NDC (-1..1, +Y up). Return value = "consumed":
    /// hover returns true when the hovered axis changed (repaint);
    /// down returns true when a drag started (don't orbit/select);
    /// move returns true while dragging; up returns true if a drag
    /// ended.
    bool pointer_hover(const Vec2& ndc);
    bool pointer_down(const Vec2& ndc);
    bool pointer_move(const Vec2& ndc);
    bool pointer_up();

    /// Pose the gizmo for this frame (position/orientation/scale of
    /// every handle, camera-facing hiding, hover highlight). Call after
    /// moving the camera/object and before Renderer::render().
    void update();

private:
    struct Handle {
        ObjectPtr   node;
        std::string name;
        Color       base_color;
        float       base_opacity;
    };

    void  build_gizmo();
    Plane drag_plane() const;
    bool  intersect_plane(const Vec2& ndc, Vec3& out) const;

    std::shared_ptr<PerspectiveCamera> camera_;
    ObjectPtr root_;
    ObjectPtr object_;

    // gizmo_/picker_ per mode (0 translate, 1 rotate, 2 scale)
    ObjectPtr gizmo_[3];
    ObjectPtr picker_[3];
    std::vector<Handle> handles_;

    Mode        mode_{Mode::Translate};
    Space       space_{Space::World};
    float       size_{1.0f};
    std::string axis_;
    bool        dragging_{false};

    // World-space state captured by update() / pointer_down(), the
    // exact set the JS implementation maintains.
    Vec3 world_position_;
    Quat world_quaternion_;
    Quat world_quaternion_inv_;
    Vec3 world_position_start_;
    Quat world_quaternion_start_;
    Vec3 camera_position_;
    Quat camera_quaternion_;
    Vec3 eye_;
    Vec3 parent_position_;
    Quat parent_quaternion_;
    Quat parent_quaternion_inv_;
    Vec3 parent_scale_{1.0f, 1.0f, 1.0f};
    Vec3 point_start_;
    Vec3 position_start_;
    Quat quaternion_start_;
    Vec3 scale_start_{1.0f, 1.0f, 1.0f};
    Vec3 rotation_axis_;
};

}  // namespace e3d
