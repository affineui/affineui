// e3d_raycast.cpp — ports of three.js r170 Raycaster, Mesh.raycast and
// Line.raycast (triangle hit with bounding-sphere pre-test; segment
// distance with a world threshold).
#include "e3d_raycast.h"

#include <algorithm>

namespace e3d {

void Raycaster::set_from_camera(const Vec2& ndc,
                                const PerspectiveCamera& camera) {
    ray.origin = camera.matrix_world.origin();
    ray.direction = Vec3{ndc.x, ndc.y, 0.5f}
                        .applied(camera.projection_matrix.inverted())
                        .applied(camera.matrix_world) -
                    ray.origin;
    ray.direction.normalize();
}

namespace {

void raycast_mesh(const Raycaster& rc, Mesh& mesh, std::vector<RayHit>& out) {
    const BufferGeometry& g = *mesh.geometry;
    if (g.positions.empty()) return;

    // World-space bounding sphere pre-test.
    const Sphere world_sphere =
        g.bounding_sphere().transformed(mesh.matrix_world);
    if (!rc.ray.intersects_sphere(world_sphere)) return;

    // Per-triangle test in local space.
    const Ray  local_ray = rc.ray.transformed(mesh.matrix_world.inverted());
    const bool cull = !mesh.material->double_sided;

    const std::size_t tri_indices =
        g.indices.empty() ? g.vertex_count() : g.indices.size();
    for (std::size_t i = 0; i + 2 < tri_indices; i += 3) {
        const auto vertex = [&](std::size_t v) {
            const std::size_t idx = g.indices.empty() ? i + v
                                                      : g.indices[i + v];
            return g.position_at(idx);
        };
        const float t = local_ray.intersect_triangle(vertex(0), vertex(1),
                                                     vertex(2), cull);
        if (t < 0.0f) continue;
        const Vec3  world_point = local_ray.at(t).applied(mesh.matrix_world);
        const float distance = rc.ray.origin.distance_to(world_point);
        if (distance < rc.near_clip || distance > rc.far_clip) continue;
        out.push_back({distance, world_point, &mesh});
    }
}

void raycast_line(const Raycaster& rc, Line& line, std::vector<RayHit>& out) {
    const BufferGeometry& g = *line.geometry;
    if (g.vertex_count() < 2) return;

    // three.js Line.raycast: widen the bounding sphere by the pick
    // threshold, then test segments in local space with the threshold
    // divided by the object's average world scale.
    Sphere world_sphere = g.bounding_sphere().transformed(line.matrix_world);
    world_sphere.radius += rc.line_threshold;
    if (!rc.ray.intersects_sphere(world_sphere)) return;

    const Vec3  s = line.matrix_world.scale_of();
    const float avg_scale = (s.x + s.y + s.z) / 3.0f;
    const float local_threshold =
        rc.line_threshold / (avg_scale != 0.0f ? avg_scale : 1.0f);
    const float local_threshold_sq = local_threshold * local_threshold;

    const Ray local_ray = rc.ray.transformed(line.matrix_world.inverted());
    const std::size_t step =
        line.kind() == ObjectKind::LineSegments ? 2 : 1;

    for (std::size_t i = 0; i + 1 < g.vertex_count(); i += step) {
        Vec3 on_ray;
        const float d2 = local_ray.distance_sq_to_segment(
            g.position_at(i), g.position_at(i + 1), &on_ray, nullptr);
        if (d2 > local_threshold_sq) continue;
        const Vec3  world_point = on_ray.applied(line.matrix_world);
        const float distance = rc.ray.origin.distance_to(world_point);
        if (distance < rc.near_clip || distance > rc.far_clip) continue;
        out.push_back({distance, world_point, &line});
    }
}

}  // namespace

void Raycaster::intersect_object(Object3D& object, bool recursive,
                                 std::vector<RayHit>& out) const {
    if (include_invisible || object.visible) {
        if (object.kind() == ObjectKind::Mesh) {
            raycast_mesh(*this, static_cast<Mesh&>(object), out);
        } else if (object.is_line()) {
            raycast_line(*this, static_cast<Line&>(object), out);
        }
    }
    if (recursive) {
        for (const auto& c : object.children()) {
            intersect_object(*c, true, out);
        }
    }
}

std::vector<RayHit> Raycaster::intersect_objects(
    const std::vector<ObjectPtr>& roots, bool recursive) const {
    std::vector<RayHit> out;
    for (const auto& r : roots) {
        if (r) intersect_object(*r, recursive, out);
    }
    std::sort(out.begin(), out.end(),
              [](const RayHit& a, const RayHit& b) {
                  return a.distance < b.distance;
              });
    return out;
}

std::vector<RayHit> Raycaster::intersect_object_sorted(Object3D& object,
                                                       bool recursive) const {
    std::vector<RayHit> out;
    intersect_object(object, recursive, out);
    std::sort(out.begin(), out.end(),
              [](const RayHit& a, const RayHit& b) {
                  return a.distance < b.distance;
              });
    return out;
}

}  // namespace e3d
