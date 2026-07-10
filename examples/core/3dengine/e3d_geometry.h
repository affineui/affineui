// e3d_geometry.h — triangle/line geometry storage and the standard
// three.js primitive generators (r170 ports, minus UVs — the engine is
// untextured).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "e3d_math.h"

namespace e3d {

class BufferGeometry;

namespace detail {
// Installed by the Renderer so that destroying a BufferGeometry frees
// any GPU buffers uploaded for it. Null when no renderer exists.
using GeometryReleaseFn = void (*)(const BufferGeometry*);
extern GeometryReleaseFn geometry_release_hook;
}  // namespace detail

// CPU-side geometry: tightly packed float triples. Triangle meshes have
// normals; line geometry has positions only. `indices` is optional —
// empty means non-indexed. Mutate through the helpers (or bump
// `version` after direct edits) so the renderer re-uploads and cached
// bounds refresh.
class BufferGeometry {
public:
    std::vector<float>         positions;  // xyz per vertex
    std::vector<float>         normals;    // xyz per vertex (meshes only)
    std::vector<std::uint32_t> indices;

    BufferGeometry() = default;
    BufferGeometry(const BufferGeometry&) = delete;
    BufferGeometry& operator=(const BufferGeometry&) = delete;
    ~BufferGeometry();

    std::size_t vertex_count() const { return positions.size() / 3; }
    Vec3        position_at(std::size_t i) const {
        return {positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
    }

    /// Replace positions from a point list (three.js setFromPoints).
    void set_from_points(const Vec3* points, std::size_t count);

    /// Bake a transform into the vertex data (three.js applyMatrix4) —
    /// used by the transform gizmo to pre-orient handle geometry.
    void apply_transform(const Mat4& m);

    /// Call after mutating the arrays directly: invalidates cached
    /// bounds and tells the renderer to re-upload.
    void mark_changed();

    const Box3&   bounding_box() const;
    const Sphere& bounding_sphere() const;

    /// Unique edge list (i0, i1 pairs) derived from the triangles —
    /// used to draw `Material::wireframe` meshes as lines.
    const std::vector<std::uint32_t>& wireframe_indices() const;

    /// Monotonic change counter; the renderer compares it against the
    /// version it last uploaded.
    std::uint64_t version() const { return version_; }

private:
    mutable Box3   bbox_;
    mutable Sphere bsphere_;
    mutable bool   bounds_valid_{false};
    mutable std::vector<std::uint32_t> wireframe_;
    mutable bool   wireframe_valid_{false};
    std::uint64_t  version_{1};
};

using GeometryPtr = std::shared_ptr<BufferGeometry>;

// ── Primitive generators (three.js r170 ports) ──────────────────────

GeometryPtr make_box(float width = 1.0f, float height = 1.0f,
                     float depth = 1.0f);
GeometryPtr make_sphere(float radius = 1.0f, int width_segments = 32,
                        int height_segments = 16);
GeometryPtr make_icosahedron(float radius = 1.0f, int detail = 0);
GeometryPtr make_octahedron(float radius = 1.0f, int detail = 0);
GeometryPtr make_cylinder(float radius_top = 1.0f, float radius_bottom = 1.0f,
                          float height = 1.0f, int radial_segments = 32,
                          int height_segments = 1, bool open_ended = false,
                          float theta_start = 0.0f,
                          float theta_length = 2.0f * kPi);
GeometryPtr make_cone(float radius = 1.0f, float height = 1.0f,
                      int radial_segments = 32, int height_segments = 1,
                      bool open_ended = false);
GeometryPtr make_torus(float radius = 1.0f, float tube = 0.4f,
                       int radial_segments = 12, int tubular_segments = 48,
                       float arc = 2.0f * kPi);
/// XY plane facing +Z, like three.js PlaneGeometry.
GeometryPtr make_plane(float width = 1.0f, float height = 1.0f);

/// Boundary + hard edges of a triangle mesh as line segment pairs
/// (three.js EdgesGeometry). Threshold is the dihedral angle in degrees.
GeometryPtr make_edges(const BufferGeometry& source,
                       float threshold_angle_deg = 1.0f);

/// Line geometry from a point list.
GeometryPtr make_from_points(const Vec3* points, std::size_t count);
inline GeometryPtr make_from_points(const std::vector<Vec3>& points) {
    return make_from_points(points.data(), points.size());
}

}  // namespace e3d
