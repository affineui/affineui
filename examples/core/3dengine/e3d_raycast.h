// e3d_raycast.h — CPU picking, ported from three.js Raycaster plus the
// Mesh and Line raycast routines the samples rely on.
#pragma once

#include <limits>
#include <vector>

#include "e3d_math.h"
#include "e3d_scene.h"

namespace e3d {

struct RayHit {
    float     distance{0.0f};  // world-space distance from ray origin
    Vec3      point;           // world-space intersection
    Object3D* object{nullptr};
};

// World matrices must be current before intersecting (call
// scene->update_matrix_world(), as the renderer does each frame).
class Raycaster {
public:
    Ray   ray;
    float near_clip{0.0f};
    float far_clip{std::numeric_limits<float>::infinity()};
    /// World-units pick tolerance for line objects
    /// (three.js raycaster.params.Line.threshold).
    float line_threshold{0.1f};
    /// three.js raycasts invisible objects too; TransformControls
    /// filters them back in for its hidden pickers. false = drop hits
    /// on invisible objects (the useful default for app picking).
    bool  include_invisible{false};

    /// Aim the ray through an NDC point (-1..1, +Y up) on the camera.
    void set_from_camera(const Vec2& ndc, const PerspectiveCamera& camera);

    /// Intersect one object (optionally its whole subtree). Results
    /// are appended unsorted; use intersect_objects for sorted output.
    void intersect_object(Object3D& object, bool recursive,
                          std::vector<RayHit>& out) const;

    /// Intersect several roots, sorted nearest-first.
    std::vector<RayHit> intersect_objects(
        const std::vector<ObjectPtr>& roots, bool recursive = true) const;
    std::vector<RayHit> intersect_object_sorted(Object3D& object,
                                                bool recursive = true) const;
};

}  // namespace e3d
