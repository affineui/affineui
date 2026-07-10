// e3d_controls_orbit.cpp — OrbitControls port (three.js r170
// examples/jsm/controls/OrbitControls.js, mouse subset: rotate /
// dolly / pan with optional damping; no touch, keys, or ortho-zoom).
#include "e3d_controls.h"

namespace e3d {

namespace {
constexpr float kEps = 0.000001f;
constexpr float kTwoPi = 2.0f * kPi;
}  // namespace

void OrbitControls::pointer_down(float x, float y, int button,
                                 bool pan_modifier) {
    if (!enabled) return;
    Action action = Action::None;
    switch (button) {
    case 0: action = button_left; break;
    case 1: action = button_middle; break;
    case 2: action = button_right; break;
    default: break;
    }
    // Left-drag with ctrl/meta/shift pans, like the JS controls.
    if (action == Action::Rotate && pan_modifier) action = Action::Pan;

    if (action == Action::Rotate && !enable_rotate) action = Action::None;
    if (action == Action::Pan && !enable_pan) action = Action::None;
    if (action == Action::Dolly && !enable_zoom) action = Action::None;

    state_ = action;
    last_pointer_ = {x, y};
}

void OrbitControls::pointer_move(float x, float y) {
    if (!enabled || state_ == Action::None) return;
    const float dx = x - last_pointer_.x;
    const float dy = y - last_pointer_.y;
    last_pointer_ = {x, y};

    switch (state_) {
    case Action::Rotate:
        rotate_delta(dx * rotate_speed, dy * rotate_speed);
        break;
    case Action::Pan:
        pan_delta(dx * pan_speed, dy * pan_speed);
        break;
    case Action::Dolly:
        // Drag down = dolly out (matches _handleMouseMoveDolly).
        if (dy != 0.0f) {
            const float s = std::pow(0.95f, zoom_speed *
                                                std::abs(dy * 0.01f));
            dolly(dy > 0.0f ? 1.0f / s : s);
        }
        break;
    default:
        break;
    }
}

void OrbitControls::pointer_up() { state_ = Action::None; }

void OrbitControls::wheel(float delta_y) {
    if (!enabled || !enable_zoom) return;
    const float s =
        std::pow(0.95f, zoom_speed * std::abs(delta_y * 0.01f));
    // Wheel up (negative deltaY) dollies in.
    dolly(delta_y < 0.0f ? s : 1.0f / s);
}

void OrbitControls::rotate_delta(float dx, float dy) {
    // Full-height drag = one revolution (yes, height for both axes).
    spherical_delta_.theta -= kTwoPi * dx / view_h_;
    spherical_delta_.phi -= kTwoPi * dy / view_h_;
}

void OrbitControls::pan_delta(float dx, float dy) {
    camera_->update_world_matrix(true, false);
    const Mat4& m = camera_->matrix;

    // Perspective pan: scale pixel motion by the visible height at the
    // target distance.
    float target_distance = camera_->position.distance_to(target);
    target_distance *= std::tan(deg_to_rad(camera_->fov) / 2.0f);

    const float left = 2.0f * dx * target_distance / view_h_;
    const float up = 2.0f * dy * target_distance / view_h_;

    pan_offset_.add_scaled(m.basis_x(), -left);
    if (screen_space_panning) {
        pan_offset_.add_scaled(m.basis_y(), up);
    } else {
        pan_offset_.add_scaled(camera_->up.cross(m.basis_x()), up);
    }
}

void OrbitControls::dolly(float scale) { scale_ *= scale; }

bool OrbitControls::update() {
    // Offset in "y-axis-is-up" space (camera_.up is +Y in practice; the
    // JS quat dance is the identity then).
    Quat up_quat;
    up_quat.set_from_unit_vectors(camera_->up.normalized(), Vec3::unit_y());
    const Quat up_quat_inv = up_quat.inverted();

    Vec3 offset = (camera_->position - target).applied(up_quat);
    Spherical spherical;
    spherical.set_from_vec3(offset);

    const float k = enable_damping ? damping_factor : 1.0f;
    spherical.theta += spherical_delta_.theta * k;
    spherical.phi += spherical_delta_.phi * k;
    spherical.phi = clampf(spherical.phi, min_polar_angle, max_polar_angle);
    spherical.make_safe();

    target.add_scaled(pan_offset_, k);

    spherical.radius =
        clampf(spherical.radius * scale_, min_distance, max_distance);

    offset = spherical.to_vec3().applied(up_quat_inv);
    camera_->position = target + offset;
    camera_->look_at(target);

    if (enable_damping) {
        spherical_delta_.theta *= 1.0f - damping_factor;
        spherical_delta_.phi *= 1.0f - damping_factor;
        pan_offset_ *= 1.0f - damping_factor;
    } else {
        spherical_delta_ = {};
        spherical_delta_.radius = 0.0f;
        pan_offset_ = {};
    }
    scale_ = 1.0f;

    // Change detection (small-angle approximation, like the JS).
    if (last_position_.distance_to_sq(camera_->position) > kEps ||
        8.0f * (1.0f - (last_quaternion_.x * camera_->quaternion().x +
                        last_quaternion_.y * camera_->quaternion().y +
                        last_quaternion_.z * camera_->quaternion().z +
                        last_quaternion_.w * camera_->quaternion().w)) >
            kEps ||
        last_target_.distance_to_sq(target) > kEps) {
        last_position_ = camera_->position;
        last_quaternion_ = camera_->quaternion();
        last_target_ = target;
        return true;
    }
    return false;
}

}  // namespace e3d
