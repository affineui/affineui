// e3d_scene.cpp — Object3D graph mechanics (three.js r170 ports).
#include "e3d_scene.h"

#include <algorithm>

namespace e3d {

void Object3D::add(const ObjectPtr& child) {
    if (!child || child.get() == this) return;
    if (child->parent) child->remove_from_parent();
    child->parent = this;
    children_.push_back(child);
}

void Object3D::remove(const ObjectPtr& child) {
    const auto it = std::find(children_.begin(), children_.end(), child);
    if (it == children_.end()) return;
    (*it)->parent = nullptr;
    children_.erase(it);
}

void Object3D::remove_from_parent() {
    if (parent) parent->remove(shared_from_this());
}

void Object3D::attach(const ObjectPtr& child) {
    if (!child || child.get() == this) return;
    // three.js Object3D.attach: bring the child's world matrix into
    // this object's space, then re-parent.
    update_world_matrix(true, false);
    child->update_world_matrix(true, false);
    const Mat4 local = matrix_world.inverted() * child->matrix_world;

    Vec3 p, s;
    Quat q;
    local.decompose(p, q, s);
    child->position = p;
    child->set_quaternion(q);
    child->scale = s;

    add(child);
    child->update_world_matrix(false, true);
}

void Object3D::traverse(const std::function<void(Object3D&)>& fn) {
    fn(*this);
    for (const auto& c : children_) c->traverse(fn);
}

void Object3D::update_matrix_world() {
    if (matrix_auto_update) {
        matrix = Mat4::compose(position, quaternion_, scale);
    }
    matrix_world = parent ? parent->matrix_world * matrix : matrix;
    for (const auto& c : children_) c->update_matrix_world();
}

void Object3D::update_world_matrix(bool update_parents, bool update_children) {
    if (update_parents && parent) parent->update_world_matrix(true, false);
    if (matrix_auto_update) {
        matrix = Mat4::compose(position, quaternion_, scale);
    }
    matrix_world = parent ? parent->matrix_world * matrix : matrix;
    if (update_children) {
        for (const auto& c : children_) c->update_world_matrix(false, true);
    }
}

Quat Object3D::world_quaternion() const {
    Vec3 p, s;
    Quat q;
    matrix_world.decompose(p, q, s);
    return q;
}

void Object3D::look_at(const Vec3& target) {
    // three.js Object3D.lookAt (world-space): cameras/lights face -Z at
    // the target, other objects face +Z.
    update_world_matrix(true, false);
    const Vec3 eye = matrix_world.origin();

    const bool camera_style =
        kind() == ObjectKind::PerspectiveCamera || is_light();
    const Mat4 orient = camera_style ? Mat4::look_at(eye, target, up)
                                     : Mat4::look_at(target, eye, up);
    Quat q;
    q.set_from_rotation_matrix(orient);

    if (parent) {
        // Strip the parent's world rotation so the local quaternion
        // produces the desired world orientation.
        Vec3 pp, ps;
        Quat pq;
        parent->matrix_world.decompose(pp, pq, ps);
        q = pq.inverted() * q;
    }
    set_quaternion(q);
}

// ── GridHelper ──────────────────────────────────────────────────────

std::shared_ptr<Group> make_grid_helper(float size, int divisions,
                                        std::uint32_t center_hex,
                                        std::uint32_t grid_hex,
                                        float opacity) {
    auto group = std::make_shared<Group>();
    group->name = "GridHelper";

    const float half = size / 2.0f;
    const float step = size / static_cast<float>(divisions);
    const int   center = divisions / 2;

    std::vector<Vec3> center_pts, grid_pts;
    for (int i = 0; i <= divisions; ++i) {
        const float k = -half + static_cast<float>(i) * step;
        auto& pts = (i == center) ? center_pts : grid_pts;
        pts.push_back({-half, 0.0f, k});
        pts.push_back({half, 0.0f, k});
        pts.push_back({k, 0.0f, -half});
        pts.push_back({k, 0.0f, half});
    }

    const auto add_lines = [&](const std::vector<Vec3>& pts,
                               std::uint32_t hex) {
        if (pts.empty()) return;
        auto lines = std::make_shared<LineSegments>(
            make_from_points(pts), Material::line(hex, opacity));
        group->add(lines);
    };
    add_lines(center_pts, center_hex);
    add_lines(grid_pts, grid_hex);
    return group;
}

// ── SelectionBox ────────────────────────────────────────────────────

SelectionBox::SelectionBox(std::uint32_t hex)
    : LineSegments(make_edges(*make_box(1.0f, 1.0f, 1.0f)),
                   Material::line(hex)) {
    name = "SelectionBox";
    material->depth_test = false;
    material->transparent = true;
    render_order = 900000;  // above the scene, below the gizmo
    matrix_auto_update = false;
    visible = false;
}

void SelectionBox::update_from(Object3D* target) {
    if (target == nullptr) {
        visible = false;
        return;
    }
    // Union every descendant geometry's bounds, mapped into the
    // target's local frame, then drive the unit cube with one composed
    // matrix (ports the web samples' updateSelectionBox).
    Box3 box;
    const Mat4 root_inv = target->matrix_world.inverted();
    target->traverse([&](Object3D& node) {
        const BufferGeometry* g = nullptr;
        if (node.kind() == ObjectKind::Mesh) {
            g = static_cast<Mesh&>(node).geometry.get();
        } else if (node.is_line()) {
            g = static_cast<Line&>(node).geometry.get();
        }
        if (g == nullptr || g->positions.empty()) return;
        box.union_with(
            g->bounding_box().transformed(root_inv * node.matrix_world));
    });
    if (box.empty()) {
        visible = false;
        return;
    }
    visible = true;
    matrix = target->matrix_world *
             Mat4::compose(box.center(), Quat{}, box.size());
    update_matrix_world();
}

}  // namespace e3d
