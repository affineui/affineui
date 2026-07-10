// e3d_scene.h — the retained scene graph: Object3D and its concrete
// kinds (groups, meshes, lines, cameras, lights), ported from the
// three.js r170 core the samples use.
//
// Ownership: parents own children through shared_ptr; `parent` is a
// non-owning back-pointer that add/remove keep consistent. Transform
// state is position / rotation (XYZ Euler) / quaternion / scale;
// rotation and quaternion are kept in sync through the setters, and
// world matrices are (re)composed by update_matrix_world().
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "e3d_geometry.h"
#include "e3d_material.h"
#include "e3d_math.h"

namespace e3d {

class Object3D;
using ObjectPtr = std::shared_ptr<Object3D>;

enum class ObjectKind {
    Object,
    Group,
    Scene,
    Mesh,
    Line,          // polyline (three.js Line)
    LineLoop,      // closed polyline
    LineSegments,  // independent segment pairs
    PerspectiveCamera,
    HemisphereLight,
    DirectionalLight,
    PointLight,
    SpotLight,
};

class Object3D : public std::enable_shared_from_this<Object3D> {
public:
    explicit Object3D(ObjectKind kind = ObjectKind::Object) : kind_(kind) {}
    virtual ~Object3D() = default;
    Object3D(const Object3D&) = delete;
    Object3D& operator=(const Object3D&) = delete;

    ObjectKind kind() const { return kind_; }
    bool is_light() const {
        return kind_ == ObjectKind::HemisphereLight ||
               kind_ == ObjectKind::DirectionalLight ||
               kind_ == ObjectKind::PointLight ||
               kind_ == ObjectKind::SpotLight;
    }
    bool is_line() const {
        return kind_ == ObjectKind::Line || kind_ == ObjectKind::LineLoop ||
               kind_ == ObjectKind::LineSegments;
    }

    std::string name;
    bool        visible{true};
    int         render_order{0};
    bool        cast_shadow{false};
    bool        receive_shadow{false};
    // When false, `matrix` is authoritative and compose() is skipped —
    // used by matrix-driven helpers like DENDER's selection box.
    bool        matrix_auto_update{true};

    Vec3 position;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};

    // rotation (Euler) and quaternion always describe the same
    // orientation; setting one updates the other, like three.js.
    const Euler& rotation() const { return rotation_; }
    const Quat&  quaternion() const { return quaternion_; }
    void set_rotation(const Euler& e) {
        rotation_ = e;
        quaternion_ = e.to_quat();
    }
    void set_quaternion(const Quat& q) {
        quaternion_ = q;
        rotation_ = Euler::from_quat(q);
    }

    Mat4 matrix;        // local, composed by update_matrix_world()
    Mat4 matrix_world;  // valid after update_matrix_world()

    Object3D*                     parent{nullptr};
    const std::vector<ObjectPtr>& children() const { return children_; }

    /// Add a child (re-parents if it already has a parent).
    void add(const ObjectPtr& child);
    /// Remove a direct child; no-op if it isn't one.
    void remove(const ObjectPtr& child);
    /// Remove this object from its parent.
    void remove_from_parent();
    /// three.js attach(): adopt `child` while preserving its world
    /// transform. Both subtrees' world matrices are refreshed.
    void attach(const ObjectPtr& child);

    /// Depth-first visit of this object and all descendants.
    void traverse(const std::function<void(Object3D&)>& fn);

    /// Recompose `matrix` (unless !matrix_auto_update) and propagate
    /// `matrix_world` down the subtree.
    void update_matrix_world();
    /// Refresh ancestors first, then this subtree — a cheap way to get
    /// a valid matrix_world for one object (three.js
    /// updateWorldMatrix(true, true)).
    void update_world_matrix(bool update_parents = true,
                             bool update_children = true);

    Vec3 world_position() const { return matrix_world.origin(); }
    Quat world_quaternion() const;

    /// three.js Object3D.lookAt: cameras and lights orient -Z at the
    /// target, everything else orients +Z.
    void look_at(const Vec3& target);

protected:
    Euler rotation_;
    Quat  quaternion_;

private:
    ObjectKind              kind_;
    std::vector<ObjectPtr>  children_;
};

// ── Concrete kinds ──────────────────────────────────────────────────

class Group : public Object3D {
public:
    Group() : Object3D(ObjectKind::Group) {}
};

class Scene : public Object3D {
public:
    Scene() : Object3D(ObjectKind::Scene) {}
    Color background{0.1f, 0.1f, 0.12f};
    float background_alpha{1.0f};
};

class Mesh : public Object3D {
public:
    Mesh(GeometryPtr geometry, MaterialPtr material)
        : Object3D(ObjectKind::Mesh),
          geometry(std::move(geometry)),
          material(std::move(material)) {}
    GeometryPtr geometry;
    MaterialPtr material;
};

class Line : public Object3D {
public:
    Line(GeometryPtr geometry, MaterialPtr material,
         ObjectKind kind = ObjectKind::Line)
        : Object3D(kind),
          geometry(std::move(geometry)),
          material(std::move(material)) {}
    GeometryPtr geometry;
    MaterialPtr material;
};

class LineSegments : public Line {
public:
    LineSegments(GeometryPtr geometry, MaterialPtr material)
        : Line(std::move(geometry), std::move(material),
               ObjectKind::LineSegments) {}
};

class LineLoop : public Line {
public:
    LineLoop(GeometryPtr geometry, MaterialPtr material)
        : Line(std::move(geometry), std::move(material),
               ObjectKind::LineLoop) {}
};

// ── Camera ──────────────────────────────────────────────────────────

class PerspectiveCamera : public Object3D {
public:
    // "near_plane"/"far_plane" because windows.h #defines near and far.
    PerspectiveCamera(float fov_deg = 50.0f, float aspect = 1.0f,
                      float near_plane = 0.1f, float far_plane = 2000.0f)
        : Object3D(ObjectKind::PerspectiveCamera),
          fov(fov_deg),
          aspect(aspect),
          near_plane(near_plane),
          far_plane(far_plane) {
        update_projection_matrix();
    }

    float fov;     // vertical, degrees (three.js convention)
    float aspect;
    float near_plane;
    float far_plane;

    /// Call after changing fov/aspect/near_plane/far_plane.
    void update_projection_matrix() {
        projection_matrix = Mat4::perspective(deg_to_rad(fov), aspect,
                                              near_plane, far_plane);
    }
    Mat4 projection_matrix;
};

// ── Lights ──────────────────────────────────────────────────────────

class Light : public Object3D {
public:
    Light(ObjectKind kind, std::uint32_t hex, float intensity)
        : Object3D(kind), color(hex), intensity(intensity) {}
    Color color;
    float intensity{1.0f};
};

class HemisphereLight : public Light {
public:
    HemisphereLight(std::uint32_t sky_hex, std::uint32_t ground_hex,
                    float intensity = 1.0f)
        : Light(ObjectKind::HemisphereLight, sky_hex, intensity),
          ground_color(ground_hex) {}
    Color ground_color;
};

class DirectionalLight : public Light {
public:
    explicit DirectionalLight(std::uint32_t hex, float intensity = 1.0f)
        : Light(ObjectKind::DirectionalLight, hex, intensity) {}
    /// World-space point the light aims at (three.js light.target,
    /// reduced to the position the samples actually set).
    Vec3 target;

    // Shadow-camera parameters, meaningful when cast_shadow is true.
    // The shadow camera is an ortho box centered on `target`, `extent`
    // half-width, looking along the light direction.
    struct Shadow {
        float extent{12.0f};
        float near_plane{0.5f};
        float far_plane{40.0f};
        int   map_size{2048};
        float bias{0.002f};
    } shadow;
};

class PointLight : public Light {
public:
    PointLight(std::uint32_t hex, float intensity = 1.0f,
               float distance = 0.0f, float decay = 2.0f)
        : Light(ObjectKind::PointLight, hex, intensity),
          distance(distance),
          decay(decay) {}
    float distance;  // cutoff; 0 = unlimited
    float decay;
};

class SpotLight : public Light {
public:
    SpotLight(std::uint32_t hex, float intensity = 1.0f,
              float distance = 0.0f, float angle = kPi / 3.0f,
              float penumbra = 0.0f, float decay = 2.0f)
        : Light(ObjectKind::SpotLight, hex, intensity),
          distance(distance),
          angle(angle),
          penumbra(penumbra),
          decay(decay) {}
    Vec3  target;
    float distance;
    float angle;     // cone half-angle, radians
    float penumbra;  // 0..1 soft edge fraction
    float decay;
};

// ── Helpers ─────────────────────────────────────────────────────────

/// three.js GridHelper: an XZ grid of `divisions` cells across `size`
/// units. Returned as a Group of two LineSegments so the center cross
/// can use its own color without vertex-color support.
std::shared_ptr<Group> make_grid_helper(float size = 10.0f,
                                        int divisions = 10,
                                        std::uint32_t center_hex = 0x444444,
                                        std::uint32_t grid_hex = 0x888888,
                                        float opacity = 1.0f);

/// Object-aligned selection outline, the box the web samples draw
/// around the selection: a unit-cube edge wireframe whose matrix is
/// composed from the union of the target subtree's geometry bounds in
/// the target's LOCAL frame — so unlike an axis-aligned Box3 helper it
/// rotates and scales with the object. Add it at the scene root and
/// call update_from() whenever the selection or its transform changes.
class SelectionBox : public LineSegments {
public:
    explicit SelectionBox(std::uint32_t hex = 0xe8943c);
    /// Refit to `target` (world matrices must be current); hides itself
    /// for null targets or geometry-less subtrees.
    void update_from(Object3D* target);
};

}  // namespace e3d
