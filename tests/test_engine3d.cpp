// Tests for the example 3D engine (examples/core/3dengine) â€” math
// invariants and geometry generators. Pure CPU; no GPU context needed.
#include <doctest/doctest.h>

#include "e3d_controls.h"
#include "e3d_geometry.h"
#include "e3d_math.h"
#include "e3d_raycast.h"
#include "e3d_scene.h"

using namespace e3d;

namespace {

bool approx(float a, float b, float eps = 1e-4f) {
    return std::abs(a - b) <= eps;
}
bool approx(const Vec3& a, const Vec3& b, float eps = 1e-4f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) &&
           approx(a.z, b.z, eps);
}

}  // namespace

// â”€â”€ Math â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST_CASE("e3d quaternion: axis-angle rotation matches expectation") {
    Quat q;
    q.set_from_axis_angle(Vec3::unit_y(), kPi / 2.0f);
    // +X rotated 90Â° about Y lands on -Z.
    CHECK(approx(Vec3::unit_x().applied(q), {0.0f, 0.0f, -1.0f}));
}

TEST_CASE("e3d quaternion: set_from_unit_vectors rotates from onto to") {
    const Vec3 from = Vec3{1.0f, 2.0f, 0.5f}.normalized();
    const Vec3 to = Vec3{-0.3f, 0.7f, -1.0f}.normalized();
    Quat q;
    q.set_from_unit_vectors(from, to);
    CHECK(approx(from.applied(q), to));

    // Opposite vectors (the degenerate branch).
    q.set_from_unit_vectors(Vec3::unit_z(), {0.0f, 0.0f, -1.0f});
    CHECK(approx(Vec3::unit_z().applied(q), {0.0f, 0.0f, -1.0f}));
}

TEST_CASE("e3d euler <-> quaternion roundtrip (XYZ order)") {
    const Euler e{0.3f, -0.8f, 1.2f};
    const Quat q = e.to_quat();
    const Euler back = Euler::from_quat(q);
    CHECK(approx(back.x, e.x));
    CHECK(approx(back.y, e.y));
    CHECK(approx(back.z, e.z));
}

TEST_CASE("e3d matrix: compose/decompose roundtrip") {
    const Vec3 p{1.0f, -2.0f, 3.0f};
    Quat q;
    q.set_from_axis_angle(Vec3{1.0f, 1.0f, 0.0f}.normalized(), 0.7f);
    const Vec3 s{2.0f, 0.5f, 1.5f};

    const Mat4 m = Mat4::compose(p, q, s);
    Vec3 p2, s2;
    Quat q2;
    m.decompose(p2, q2, s2);
    CHECK(approx(p2, p));
    CHECK(approx(s2, s));
    CHECK(q2.angle_to(q) < 1e-3f);
}

TEST_CASE("e3d matrix: inverse of TRS is identity when multiplied") {
    Quat q;
    q.set_from_axis_angle(Vec3::unit_z(), 0.4f);
    const Mat4 m = Mat4::compose({3.0f, 1.0f, -2.0f}, q, {1.0f, 2.0f, 1.0f});
    const Mat4 id = m * m.inverted();
    for (int i = 0; i < 16; ++i) {
        CHECK(approx(id.e[i], Mat4::identity().e[i], 1e-4f));
    }
}

TEST_CASE("e3d matrix: look_at orients -Z at the target") {
    const Vec3 eye{0.0f, 0.0f, 5.0f};
    const Vec3 target{0.0f, 0.0f, 0.0f};
    const Mat4 m = Mat4::look_at(eye, target, Vec3::unit_y());
    // Camera looks down -Z already: basis stays canonical.
    CHECK(approx(m.basis_z(), Vec3::unit_z()));

    const Mat4 m2 = Mat4::look_at({5.0f, 0.0f, 0.0f}, target, Vec3::unit_y());
    CHECK(approx(m2.basis_z(), Vec3::unit_x()));  // back-axis points at eye
}

TEST_CASE("e3d matrix: normal matrix handles non-uniform scale") {
    const Mat4 m = Mat4::scaling({2.0f, 1.0f, 1.0f});
    // A normal of a plane tilted 45Â° in XY: under x2 scale in X the
    // *surface* flattens, so the normal must steepen, not flatten.
    const Vec3 n = Vec3{1.0f, 1.0f, 0.0f}.normalized();
    const Vec3 out = n.transformed_direction(m.normal_matrix());
    CHECK(out.x < out.y);
    CHECK(approx(out.length(), 1.0f));
}

TEST_CASE("e3d ray: plane intersection") {
    Ray r;
    r.origin = {0.0f, 5.0f, 0.0f};
    r.direction = {0.0f, -1.0f, 0.0f};
    const Plane ground = Plane::from_normal_and_point(Vec3::unit_y(), {});
    CHECK(approx(r.intersect_plane(ground), 5.0f));
    CHECK(approx(r.at(5.0f), {0.0f, 0.0f, 0.0f}));

    // Pointing away â†’ miss.
    r.direction = {0.0f, 1.0f, 0.0f};
    CHECK(r.intersect_plane(ground) < 0.0f);
}

TEST_CASE("e3d ray: triangle intersection with backface culling") {
    Ray r;
    r.origin = {0.25f, 0.25f, 1.0f};
    r.direction = {0.0f, 0.0f, -1.0f};
    const Vec3 a{0, 0, 0}, b{1, 0, 0}, c{0, 1, 0};  // CCW seen from +Z
    CHECK(approx(r.intersect_triangle(a, b, c, true), 1.0f));
    // From behind, the culled test misses but the two-sided test hits.
    r.origin = {0.25f, 0.25f, -1.0f};
    r.direction = {0.0f, 0.0f, 1.0f};
    CHECK(r.intersect_triangle(a, b, c, true) < 0.0f);
    CHECK(approx(r.intersect_triangle(a, b, c, false), 1.0f));
}

TEST_CASE("e3d ray: distance to segment") {
    Ray r;
    r.origin = {0.0f, 0.0f, 0.0f};
    r.direction = {0.0f, 0.0f, -1.0f};
    // Segment crossing the ray's path 5 units out, offset 2 in X.
    const float d2 = r.distance_sq_to_segment({2.0f, -1.0f, -5.0f},
                                              {2.0f, 1.0f, -5.0f});
    CHECK(approx(d2, 4.0f));
}

TEST_CASE("e3d ray: sphere pre-test") {
    Ray r;
    r.origin = {0.0f, 0.0f, 5.0f};
    r.direction = {0.0f, 0.0f, -1.0f};
    CHECK(r.intersects_sphere({{0.0f, 0.0f, 0.0f}, 1.0f}));
    CHECK(!r.intersects_sphere({{3.0f, 0.0f, 0.0f}, 1.0f}));
    // Behind the origin.
    CHECK(!r.intersects_sphere({{0.0f, 0.0f, 8.0f}, 1.0f}));
}

TEST_CASE("e3d spherical <-> vec3 roundtrip") {
    const Vec3 v{2.0f, 3.0f, -1.0f};
    Spherical s;
    s.set_from_vec3(v);
    CHECK(approx(s.to_vec3(), v));
}

TEST_CASE("e3d box3: transform refits corners") {
    Box3 b{{-1, -1, -1}, {1, 1, 1}};
    Quat q;
    q.set_from_axis_angle(Vec3::unit_y(), kPi / 4.0f);
    const Box3 t = b.transformed(Mat4::rotation(q));
    // Rotated cube's AABB grows to sqrt(2) on X/Z.
    CHECK(approx(t.max.x, std::sqrt(2.0f)));
    CHECK(approx(t.max.y, 1.0f));
}

TEST_CASE("e3d color: hex converts sRGB to linear") {
    const Color mid(0x808080);
    CHECK(mid.r > 0.2f);  // ~0.5 sRGB â‰ˆ 0.216 linear
    CHECK(mid.r < 0.23f);
    const Color white(0xffffff);
    CHECK(approx(white.r, 1.0f));
    CHECK(approx(Color(0x000000).g, 0.0f));
}

// â”€â”€ Geometry â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST_CASE("e3d geometry: box has 24 verts, 36 indices, unit-length normals") {
    auto g = make_box(1.6f, 1.6f, 1.6f);
    CHECK(g->vertex_count() == 24);
    CHECK(g->indices.size() == 36);
    CHECK(g->normals.size() == g->positions.size());
    const Box3& b = g->bounding_box();
    CHECK(approx(b.min, {-0.8f, -0.8f, -0.8f}));
    CHECK(approx(b.max, {0.8f, 0.8f, 0.8f}));
}

TEST_CASE("e3d geometry: box winding is CCW from outside") {
    auto g = make_box(2.0f, 2.0f, 2.0f);
    // Every triangle's geometric normal must point away from center.
    for (std::size_t i = 0; i + 2 < g->indices.size(); i += 3) {
        const Vec3 a = g->position_at(g->indices[i]);
        const Vec3 b = g->position_at(g->indices[i + 1]);
        const Vec3 c = g->position_at(g->indices[i + 2]);
        const Vec3 n = (b - a).cross(c - a);
        const Vec3 centroid = (a + b + c) / 3.0f;
        CHECK(n.dot(centroid) > 0.0f);
    }
}

TEST_CASE("e3d geometry: sphere bounds and normals") {
    auto g = make_sphere(0.9f, 32, 24);
    const Sphere& s = g->bounding_sphere();
    CHECK(approx(s.radius, 0.9f * std::sqrt(3.0f), 0.05f));  // box-derived
    // Normals point radially.
    for (std::size_t i = 0; i < g->vertex_count(); i += 37) {
        const Vec3 p = g->position_at(i);
        const Vec3 n{g->normals[i * 3], g->normals[i * 3 + 1],
                     g->normals[i * 3 + 2]};
        CHECK(approx(p.normalized().dot(n), 1.0f, 1e-3f));
    }
}

TEST_CASE("e3d geometry: icosahedron detail levels") {
    auto flat = make_icosahedron(0.9f, 0);
    CHECK(flat->vertex_count() == 60);  // 20 faces, non-indexed
    auto smooth = make_icosahedron(0.9f, 1);
    CHECK(smooth->vertex_count() == 240);  // 4x subdivision
    // Every vertex sits on the sphere.
    for (std::size_t i = 0; i < smooth->vertex_count(); ++i) {
        CHECK(approx(smooth->position_at(i).length(), 0.9f, 1e-3f));
    }
}

TEST_CASE("e3d geometry: cylinder and cone") {
    auto cyl = make_cylinder(0.7f, 0.7f, 1.6f, 32);
    const Box3& b = cyl->bounding_box();
    CHECK(approx(b.min.y, -0.8f));
    CHECK(approx(b.max.y, 0.8f));
    CHECK(approx(b.max.x, 0.7f, 1e-2f));

    // Cone = zero top radius; still closed at the bottom.
    auto cone = make_cone(0.85f, 1.6f, 32);
    CHECK(!cone->indices.empty());
    CHECK(approx(cone->bounding_box().max.y, 0.8f));

    // Open-ended cone (DENDER's spot-light helper) has no cap.
    auto open_cone = make_cone(0.5f, 1.2f, 16, 1, true);
    CHECK(open_cone->indices.size() < cone->indices.size());
}

TEST_CASE("e3d geometry: torus bounds") {
    auto g = make_torus(0.8f, 0.25f, 16, 48);
    const Box3& b = g->bounding_box();
    CHECK(approx(b.max.x, 1.05f, 1e-2f));
    CHECK(approx(b.max.z, 0.25f, 1e-2f));
}

TEST_CASE("e3d geometry: plane faces +Z") {
    auto g = make_plane(2.0f, 2.0f);
    CHECK(g->vertex_count() == 4);
    CHECK(g->indices.size() == 6);
    CHECK(approx(g->normals[2], 1.0f));
    // Winding CCW from +Z.
    const Vec3 a = g->position_at(g->indices[0]);
    const Vec3 b = g->position_at(g->indices[1]);
    const Vec3 c = g->position_at(g->indices[2]);
    CHECK((b - a).cross(c - a).z > 0.0f);
}

TEST_CASE("e3d geometry: edges of a cube are its 12 hard edges") {
    auto box = make_box(1.0f, 1.0f, 1.0f);
    auto edges = make_edges(*box, 1.0f);
    // 12 edges â†’ 24 vertices, positions only.
    CHECK(edges->vertex_count() == 24);
    CHECK(edges->normals.empty());
    // A sphere at high tessellation has no edges above 40Â°.
    auto sphere = make_sphere(1.0f, 32, 24);
    auto sphere_edges = make_edges(*sphere, 40.0f);
    CHECK(sphere_edges->vertex_count() == 0);
}

TEST_CASE("e3d geometry: wireframe indices are unique edges") {
    auto box = make_box(1.0f, 1.0f, 1.0f);
    // 6 faces x (4 border + 1 diagonal) = 30 edges = 60 indices.
    CHECK(box->wireframe_indices().size() == 60);
}

// â”€â”€ Scene graph â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST_CASE("e3d scene: add/remove keep parent pointers consistent") {
    auto root = std::make_shared<Group>();
    auto a = std::make_shared<Group>();
    auto b = std::make_shared<Group>();
    root->add(a);
    a->add(b);
    CHECK(b->parent == a.get());
    // Re-parenting via add() removes from the old parent.
    root->add(b);
    CHECK(b->parent == root.get());
    CHECK(a->children().empty());
    b->remove_from_parent();
    CHECK(b->parent == nullptr);
    CHECK(root->children().size() == 1);
}

TEST_CASE("e3d scene: world matrices compose down the hierarchy") {
    auto root = std::make_shared<Group>();
    auto child = std::make_shared<Group>();
    root->add(child);
    root->position = {10.0f, 0.0f, 0.0f};
    child->position = {0.0f, 5.0f, 0.0f};
    root->update_matrix_world();
    CHECK(approx(child->world_position(), {10.0f, 5.0f, 0.0f}));

    // Parent rotation carries the child around.
    Quat q;
    q.set_from_axis_angle(Vec3::unit_y(), kPi / 2.0f);
    root->set_quaternion(q);
    root->update_matrix_world();
    CHECK(approx(child->world_position(), {10.0f, 5.0f, 0.0f}));
    child->position = {1.0f, 0.0f, 0.0f};
    root->update_matrix_world();
    // +X in root space rotated 90Â° about Y â†’ -Z in world.
    CHECK(approx(child->world_position(), {10.0f, 0.0f, -1.0f}));
}

TEST_CASE("e3d scene: attach preserves world transform") {
    auto scene = std::make_shared<Scene>();
    auto group = std::make_shared<Group>();
    auto obj = std::make_shared<Group>();
    scene->add(group);
    scene->add(obj);
    group->position = {5.0f, 0.0f, 0.0f};
    Quat q;
    q.set_from_axis_angle(Vec3::unit_y(), 0.7f);
    group->set_quaternion(q);
    obj->position = {1.0f, 2.0f, 3.0f};
    scene->update_matrix_world();
    const Vec3 world_before = obj->world_position();

    group->attach(obj);  // re-parent under the transformed group
    CHECK(obj->parent == group.get());
    scene->update_matrix_world();
    CHECK(approx(obj->world_position(), world_before));
}

TEST_CASE("e3d scene: traverse visits the whole subtree") {
    auto root = std::make_shared<Group>();
    auto a = std::make_shared<Group>();
    auto b = std::make_shared<Group>();
    root->add(a);
    a->add(b);
    int count = 0;
    root->traverse([&](Object3D&) { ++count; });
    CHECK(count == 3);
}

TEST_CASE("e3d scene: camera look_at aims -Z at target") {
    auto cam = std::make_shared<PerspectiveCamera>(38.0f, 1.5f, 0.1f, 200.0f);
    cam->position = {4.0f, 3.0f, 6.0f};
    cam->look_at({0.0f, 0.0f, 0.0f});
    cam->update_matrix_world();
    // The camera's forward (-Z basis) points at the origin.
    const Vec3 forward = -cam->matrix_world.basis_z();
    const Vec3 expected = (Vec3{} - cam->position).normalized();
    CHECK(approx(forward, expected, 1e-3f));
}

TEST_CASE("e3d scene: rotation and quaternion stay in sync") {
    Group g;
    g.set_rotation({0.0f, kPi / 2.0f, 0.0f});
    CHECK(approx(Vec3::unit_x().applied(g.quaternion()),
                 {0.0f, 0.0f, -1.0f}));
    Quat q;
    q.set_from_axis_angle(Vec3::unit_x(), 0.5f);
    g.set_quaternion(q);
    CHECK(approx(g.rotation().x, 0.5f));
}

TEST_CASE("e3d scene: grid helper splits center and grid lines") {
    auto grid = make_grid_helper(20.0f, 20, 0x40444c, 0x2a2d34);
    REQUIRE(grid->children().size() == 2);
    const auto& center =
        static_cast<LineSegments&>(*grid->children()[0]);
    const auto& rest = static_cast<LineSegments&>(*grid->children()[1]);
    CHECK(center.geometry->vertex_count() == 4);        // 2 center lines
    CHECK(rest.geometry->vertex_count() == 20 * 4);     // 20 others
}

// â”€â”€ Raycaster â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

namespace {

std::shared_ptr<PerspectiveCamera> test_camera() {
    auto cam = std::make_shared<PerspectiveCamera>(45.0f, 1.0f, 0.1f, 100.0f);
    cam->position = {0.0f, 0.0f, 10.0f};
    cam->look_at({0.0f, 0.0f, 0.0f});
    cam->update_matrix_world();
    return cam;
}

}  // namespace

TEST_CASE("e3d raycast: center ray hits a cube dead-on") {
    auto cam = test_camera();
    auto cube = std::make_shared<Mesh>(make_box(2.0f, 2.0f, 2.0f),
                                       Material::standard(0x9aa1ad));
    cube->update_matrix_world();

    Raycaster rc;
    rc.set_from_camera({0.0f, 0.0f}, *cam);
    auto hits = rc.intersect_object_sorted(*cube);
    REQUIRE(!hits.empty());
    CHECK(approx(hits[0].distance, 9.0f));  // camera z=10, cube face z=1
    CHECK(approx(hits[0].point, {0.0f, 0.0f, 1.0f}));
    CHECK(hits[0].object == cube.get());
}

TEST_CASE("e3d raycast: nearest object sorts first, invisible filtered") {
    auto cam = test_camera();
    std::vector<ObjectPtr> roots;
    auto near_cube = std::make_shared<Mesh>(make_box(1.0f, 1.0f, 1.0f),
                                            Material::standard(0xffffff));
    near_cube->position = {0.0f, 0.0f, 5.0f};
    auto far_cube = std::make_shared<Mesh>(make_box(1.0f, 1.0f, 1.0f),
                                           Material::standard(0xffffff));
    far_cube->position = {0.0f, 0.0f, -5.0f};
    near_cube->update_matrix_world();
    far_cube->update_matrix_world();
    roots = {near_cube, far_cube};

    Raycaster rc;
    rc.set_from_camera({0.0f, 0.0f}, *cam);
    auto hits = rc.intersect_objects(roots);
    REQUIRE(hits.size() >= 2);
    CHECK(hits[0].object == near_cube.get());

    near_cube->visible = false;
    hits = rc.intersect_objects(roots);
    REQUIRE(!hits.empty());
    CHECK(hits[0].object == far_cube.get());

    rc.include_invisible = true;
    hits = rc.intersect_objects(roots);
    CHECK(hits[0].object == near_cube.get());
}

TEST_CASE("e3d raycast: transformed mesh picks in world space") {
    auto cam = test_camera();
    auto cube = std::make_shared<Mesh>(make_box(1.0f, 1.0f, 1.0f),
                                       Material::standard(0xffffff));
    cube->position = {3.0f, 0.0f, 0.0f};
    cube->scale = {2.0f, 2.0f, 2.0f};
    cube->update_matrix_world();

    Raycaster rc;
    // Ray straight along -Z offset to x=3: aim via a custom ray.
    rc.ray.origin = {3.0f, 0.0f, 10.0f};
    rc.ray.direction = {0.0f, 0.0f, -1.0f};
    auto hits = rc.intersect_object_sorted(*cube);
    REQUIRE(!hits.empty());
    CHECK(approx(hits[0].point, {3.0f, 0.0f, 1.0f}));  // scaled face at z=1
}

TEST_CASE("e3d raycast: line segments pick within threshold") {
    auto pts = std::vector<Vec3>{{-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    auto line = std::make_shared<LineSegments>(make_from_points(pts),
                                               Material::line(0xffffff));
    line->update_matrix_world();

    Raycaster rc;
    rc.ray.origin = {0.0f, 0.05f, 5.0f};  // slightly above the line
    rc.ray.direction = {0.0f, 0.0f, -1.0f};
    rc.line_threshold = 0.1f;
    auto hits = rc.intersect_object_sorted(*line);
    REQUIRE(!hits.empty());
    CHECK(approx(hits[0].distance, 5.0f, 1e-2f));

    rc.line_threshold = 0.01f;  // tighter than the 0.05 offset â†’ miss
    hits = rc.intersect_object_sorted(*line);
    CHECK(hits.empty());
}

TEST_CASE("e3d raycast: ndc corners diverge from center ray") {
    auto cam = test_camera();
    Raycaster center, corner;
    center.set_from_camera({0.0f, 0.0f}, *cam);
    corner.set_from_camera({1.0f, 1.0f}, *cam);
    CHECK(center.ray.direction.angle_to(corner.ray.direction) > 0.1f);
    // Both originate at the camera.
    CHECK(approx(center.ray.origin, cam->position));
}

TEST_CASE("e3d geometry: set_from_points and version bump") {
    auto g = make_from_points({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}});
    CHECK(g->vertex_count() == 3);
    const auto v1 = g->version();
    g->positions[0] = 5.0f;
    g->mark_changed();
    CHECK(g->version() > v1);
    CHECK(approx(g->bounding_box().max.x, 5.0f));
}

TEST_CASE("e3d geometry: octahedron and apply_transform") {
    auto g = make_octahedron(0.1f, 0);
    CHECK(g->vertex_count() == 24);  // 8 faces, non-indexed
    g->apply_transform(Mat4::translation({0.0f, 2.0f, 0.0f}));
    CHECK(approx(g->bounding_box().center(), {0.0f, 2.0f, 0.0f}, 1e-3f));
}

// â”€â”€ OrbitControls â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST_CASE("e3d orbit: rotate drag orbits camera around target") {
    auto cam = std::make_shared<PerspectiveCamera>(38.0f, 1.5f, 0.1f, 200.0f);
    cam->position = {0.0f, 0.0f, 10.0f};
    OrbitControls orbit(cam);
    orbit.set_viewport(800.0f, 600.0f);
    orbit.target = {0.0f, 0.0f, 0.0f};
    orbit.update();

    orbit.pointer_down(400.0f, 300.0f, 0);
    CHECK(orbit.dragging());
    orbit.pointer_move(460.0f, 300.0f);  // horizontal drag
    orbit.pointer_up();
    CHECK(orbit.update());

    // Distance preserved, azimuth changed, camera still looks at target.
    CHECK(approx(orbit.distance(), 10.0f, 1e-3f));
    CHECK(std::abs(cam->position.x) > 1.0f);
    cam->update_world_matrix(true, false);
    const Vec3 fwd = -cam->matrix_world.basis_z();
    CHECK(approx(fwd, (orbit.target - cam->position).normalized(), 1e-3f));
}

TEST_CASE("e3d orbit: wheel dollies within min/max distance") {
    auto cam = std::make_shared<PerspectiveCamera>();
    cam->position = {0.0f, 0.0f, 10.0f};
    OrbitControls orbit(cam);
    orbit.set_viewport(800.0f, 600.0f);
    orbit.min_distance = 2.0f;
    orbit.max_distance = 12.0f;
    orbit.update();

    orbit.wheel(-120.0f);  // zoom in
    orbit.update();
    CHECK(orbit.distance() < 10.0f);
    for (int i = 0; i < 60; ++i) {
        orbit.wheel(-120.0f);
        orbit.update();
    }
    CHECK(approx(orbit.distance(), 2.0f, 1e-2f));  // clamped at min
    for (int i = 0; i < 90; ++i) {
        orbit.wheel(120.0f);
        orbit.update();
    }
    CHECK(approx(orbit.distance(), 12.0f, 1e-2f));  // clamped at max
}

TEST_CASE("e3d orbit: polar clamp keeps camera above the ground plane") {
    auto cam = std::make_shared<PerspectiveCamera>();
    cam->position = {0.0f, 5.0f, 10.0f};
    OrbitControls orbit(cam);
    orbit.set_viewport(800.0f, 600.0f);
    orbit.max_polar_angle = kPi / 2.0f;  // horizon
    orbit.update();

    orbit.pointer_down(400.0f, 300.0f, 0);
    orbit.pointer_move(400.0f, 1200.0f);  // huge downward drag
    orbit.pointer_up();
    orbit.update();
    CHECK(cam->position.y >= -1e-3f);
}

TEST_CASE("e3d orbit: pan moves the target") {
    auto cam = std::make_shared<PerspectiveCamera>();
    cam->position = {0.0f, 0.0f, 10.0f};
    OrbitControls orbit(cam);
    orbit.set_viewport(800.0f, 600.0f);
    orbit.update();

    orbit.pointer_down(400.0f, 300.0f, 2);  // right button = pan
    orbit.pointer_move(500.0f, 300.0f);
    orbit.pointer_up();
    orbit.update();
    CHECK(std::abs(orbit.target.x) > 0.1f);
    CHECK(approx(orbit.distance(), 10.0f, 1e-2f));
}

// â”€â”€ TransformControls â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

namespace {

struct GizmoRig {
    std::shared_ptr<Scene> scene;
    std::shared_ptr<PerspectiveCamera> camera;
    ObjectPtr cube;
    std::shared_ptr<TransformControls> tc;

    GizmoRig() {
        scene = std::make_shared<Scene>();
        camera =
            std::make_shared<PerspectiveCamera>(45.0f, 1.0f, 0.1f, 100.0f);
        camera->position = {0.0f, 0.0f, 8.0f};
        camera->look_at({0.0f, 0.0f, 0.0f});
        scene->add(camera);
        cube = std::make_shared<Mesh>(make_box(1.0f, 1.0f, 1.0f),
                                      Material::standard(0xffffff));
        scene->add(cube);
        tc = std::make_shared<TransformControls>(camera);
        scene->add(tc->helper());
        tc->attach(cube);
        scene->update_matrix_world();
        tc->update();
        scene->update_matrix_world();
    }
};

}  // namespace

TEST_CASE("e3d gizmo: helper visibility follows attach/detach") {
    GizmoRig rig;
    CHECK(rig.tc->helper()->visible);
    rig.tc->detach();
    CHECK(!rig.tc->helper()->visible);
}

TEST_CASE("e3d gizmo: hover finds the +X arrow picker") {
    GizmoRig rig;
    // The X axis extends to +X in screen space; hover a point to the
    // right of center. Gizmo scale at distance 8, fov 45 â‰ˆ 1.5 world
    // units for the whole handle, so probe ~0.35 world units out â€”
    // project (0.75, 0, 0) into NDC.
    const Vec3 probe =
        Vec3{0.75f, 0.0f, 0.0f}
            .applied(rig.camera->matrix_world.inverted())
            .applied(rig.camera->projection_matrix);
    CHECK(rig.tc->pointer_hover({probe.x, probe.y}));
    CHECK(rig.tc->axis() == "X");
    // Hovering far away clears it.
    CHECK(rig.tc->pointer_hover({0.9f, 0.9f}));
    CHECK(rig.tc->axis().empty());
}

TEST_CASE("e3d gizmo: X-axis drag translates the object in X only") {
    GizmoRig rig;
    const auto to_ndc = [&](const Vec3& world) {
        const Vec3 p = world.applied(rig.camera->matrix_world.inverted())
                           .applied(rig.camera->projection_matrix);
        return Vec2{p.x, p.y};
    };

    bool drag_events[2] = {false, false};
    rig.tc->on_dragging_changed = [&](bool d) {
        drag_events[d ? 0 : 1] = true;
    };
    int changes = 0;
    rig.tc->on_object_change = [&] { ++changes; };

    CHECK(rig.tc->pointer_hover(to_ndc({0.75f, 0.0f, 0.0f})));
    CHECK(rig.tc->pointer_down(to_ndc({0.75f, 0.0f, 0.0f})));
    CHECK(rig.tc->dragging());
    CHECK(drag_events[0]);

    // Drag one world unit to the right along the axis.
    CHECK(rig.tc->pointer_move(to_ndc({1.75f, 0.0f, 0.0f})));
    CHECK(changes > 0);
    CHECK(approx(rig.cube->position.x, 1.0f, 0.05f));
    CHECK(approx(rig.cube->position.y, 0.0f, 1e-3f));
    CHECK(approx(rig.cube->position.z, 0.0f, 1e-3f));

    CHECK(rig.tc->pointer_up());
    CHECK(!rig.tc->dragging());
    CHECK(drag_events[1]);
}

TEST_CASE("e3d gizmo: translation snap quantizes the drag") {
    GizmoRig rig;
    const auto to_ndc = [&](const Vec3& world) {
        const Vec3 p = world.applied(rig.camera->matrix_world.inverted())
                           .applied(rig.camera->projection_matrix);
        return Vec2{p.x, p.y};
    };
    rig.tc->translation_snap = 0.5f;
    REQUIRE(rig.tc->pointer_down(to_ndc({0.75f, 0.0f, 0.0f})));
    rig.tc->pointer_move(to_ndc({1.4f, 0.0f, 0.0f}));
    const float x = rig.cube->position.x;
    CHECK(approx(x / 0.5f, std::round(x / 0.5f), 1e-3f));
    rig.tc->pointer_up();
}

TEST_CASE("e3d gizmo: Z-ring drag rotates about Z") {
    GizmoRig rig;
    rig.tc->set_mode(TransformControls::Mode::Rotate);
    rig.scene->update_matrix_world();
    rig.tc->update();

    // The Z rotate ring faces the camera; grab it at its right edge.
    // Ring radius 0.5 x handle scale (~1.36 at distance 8, fov 45).
    const auto to_ndc = [&](const Vec3& world) {
        const Vec3 p = world.applied(rig.camera->matrix_world.inverted())
                           .applied(rig.camera->projection_matrix);
        return Vec2{p.x, p.y};
    };
    const float ring_r = 0.5f * 8.0f *
                         std::min(1.9f * std::tan(kPi * 45.0f / 360.0f),
                                  7.0f) /
                         4.0f;
    // Probe the ring's diagonal â€” the +X pole is shared with the
    // Y-ring, but (r/âˆš2, r/âˆš2, 0) lies on the Z-ring alone.
    const float d = ring_r / std::sqrt(2.0f);
    REQUIRE(rig.tc->pointer_hover(to_ndc({d, d, 0.0f})));
    REQUIRE(rig.tc->axis() == "Z");
    REQUIRE(rig.tc->pointer_down(to_ndc({d, d, 0.0f})));
    rig.tc->pointer_move(to_ndc({d * 0.7f, d * 1.25f, 0.0f}));
    rig.tc->pointer_up();

    const Euler e = rig.cube->rotation();
    CHECK(std::abs(e.z) > 0.05f);
    CHECK(approx(e.x, 0.0f, 1e-2f));
    CHECK(approx(e.y, 0.0f, 1e-2f));
}

TEST_CASE("e3d gizmo: uniform scale via center handle") {
    GizmoRig rig;
    rig.tc->set_mode(TransformControls::Mode::Scale);
    rig.scene->update_matrix_world();
    rig.tc->update();

    const auto to_ndc = [&](const Vec3& world) {
        const Vec3 p = world.applied(rig.camera->matrix_world.inverted())
                           .applied(rig.camera->projection_matrix);
        return Vec2{p.x, p.y};
    };
    // The X scale handle sits along +X like the translate arrow.
    REQUIRE(rig.tc->pointer_hover(to_ndc({0.75f, 0.0f, 0.0f})));
    REQUIRE(rig.tc->axis() == "X");
    REQUIRE(rig.tc->pointer_down(to_ndc({0.75f, 0.0f, 0.0f})));
    rig.tc->pointer_move(to_ndc({1.5f, 0.0f, 0.0f}));
    rig.tc->pointer_up();
    CHECK(rig.cube->scale.x > 1.5f);
    CHECK(approx(rig.cube->scale.y, 1.0f, 1e-3f));
}

TEST_CASE("e3d gizmo: mode switch changes visible gizmo group") {
    GizmoRig rig;
    rig.tc->set_mode(TransformControls::Mode::Rotate);
    rig.tc->update();
    // Count visible direct children of the helper root: 3 gizmo groups
    // + 3 picker groups; exactly one gizmo group visible.
    int visible_groups = 0;
    for (const auto& c : rig.tc->helper()->children()) {
        if (c->visible) ++visible_groups;
    }
    CHECK(visible_groups == 1);
}

// â”€â”€ Reflection â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

TEST_CASE("e3d reflection: Object3D property get/set round-trips") {
    static_assert(affineui::Reflectable<Object3D>);
    Object3D node;
    node.name = "hero";
    node.position = {1.0f, 2.0f, 3.0f};
    node.set_rotation({0.0f, deg_to_rad(90.0f), 0.0f});
    const affineui::ObjectClass& cls = get_class(node);
    CHECK(std::string(cls.name()) == "Object3D");

    // Reads: name string, per-axis doubles, rotation in degrees.
    CHECK(std::get<std::string>(cls.get(&node, "name")) == "hero");
    CHECK(approx(static_cast<float>(
                     std::get<double>(cls.get(&node, "position.y"))),
                 2.0f, 1e-6f));
    CHECK(approx(static_cast<float>(
                     std::get<double>(cls.get(&node, "rotation.y"))),
                 90.0f, 1e-3f));

    // Writes go through the same mediator; rotation converts back to
    // radians and re-syncs the quaternion.
    CHECK(cls.set(&node, "position.x", affineui::PropertyValue{5.5}));
    CHECK(approx(node.position.x, 5.5f, 1e-6f));
    CHECK(cls.set(&node, "rotation.z", affineui::PropertyValue{45.0}));
    CHECK(approx(node.rotation().z, deg_to_rad(45.0f), 1e-4f));
    CHECK(cls.set(&node, "visible", affineui::PropertyValue{false}));
    CHECK_FALSE(node.visible);

    // name is read-only; unknown properties are refused.
    CHECK_FALSE(cls.set(&node, "name", affineui::PropertyValue{std::string{"x"}}));
    CHECK_FALSE(cls.set(&node, "nope", affineui::PropertyValue{1.0}));
}

TEST_CASE("e3d gizmo: attached helper exposes visible drawable geometry") {
    // "Gizmo missing" regression guard at the scene layer: after attach +
    // update the helper subtree must contain visible meshes/lines with
    // real geometry and sane world matrices — exactly what the renderer
    // collects and draws.
    GizmoRig rig;
    rig.tc->attach(rig.cube);
    rig.tc->set_mode(TransformControls::Mode::Translate);
    rig.tc->update();
    rig.scene->update_matrix_world();

    int drawable = 0;
    rig.tc->helper()->traverse([&](Object3D& node) {
        if (!node.visible) return;
        for (Object3D* a = node.parent; a != nullptr; a = a->parent) {
            if (!a->visible) return;
        }
        const BufferGeometry* g = nullptr;
        if (node.kind() == ObjectKind::Mesh) {
            g = static_cast<Mesh&>(node).geometry.get();
        } else if (node.is_line()) {
            g = static_cast<Line&>(node).geometry.get();
        }
        if (g == nullptr || g->positions.empty()) return;
        const float sc = node.matrix_world.max_scale_on_axis();
        if (sc > 1e-5f) ++drawable;
    });
    // The translate gizmo has arrow shafts/heads and plane quads on
    // multiple axes — a healthy pose exposes a good handful.
    CHECK(drawable >= 6);

    // Detached again: the helper hides (nothing for the renderer).
    rig.tc->detach();
    rig.tc->update();
    CHECK_FALSE(rig.tc->helper()->visible);
}
