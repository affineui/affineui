#include "dender_viewport.h"

#include <affineui/painter.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace dender {

namespace {

using affineui::Color;
using affineui::PathPaint;
using affineui::Rect;

constexpr double kPi = 3.14159265358979323846;

// Web viewport.js camera / OrbitControls parameters.
constexpr double kFovY = 38.0 * kPi / 180.0;
constexpr double kNear = 0.1;
constexpr double kMinDist = 1.8;
constexpr double kMaxDist = 50.0;
constexpr double kRotateSpeed = 0.85;
constexpr double kPanSpeed = 0.8;
constexpr double kZoomSpeed = 0.9;
// maxPolarAngle = pi * 0.495 keeps the eye just above the ground plane.
// Pitch here is elevation above the horizon (pi/2 - polar angle).
constexpr double kPitchMin = kPi * 0.005;
constexpr double kPitchMax = kPi * 0.5 - 1e-3;

// Click-vs-drag discrimination (web click handler).
constexpr double kClickSlopPx = 4.0;
constexpr auto kClickMaxHold = std::chrono::milliseconds(600);
constexpr auto kPostDragSuppress = std::chrono::milliseconds(250);

// Fallback canvas height for orbit/pan speed before the first paint has
// resolved the real rect.
constexpr double kFallbackH = 640.0;

constexpr const char* kSceneLayerName = "dn-vp-scenelayer";

// TEMP (user directive 2026-07-03): viewport painting is DISABLED while UI
// performance is being debugged — the scene/gizmo canvases stay declared
// (layout and hit-testing unchanged) but nothing registers or repaints, so
// the viewport contributes zero per-frame and per-interaction cost.
constexpr bool kViewportPaintEnabled = false;

// Scene palette (web viewport.js colors).
constexpr Color kBaseColor{0x9a, 0xa1, 0xad, 0xff};   // mesh base / wireframe
constexpr Color kSelectColor{0xe8, 0x94, 0x3c, 0xff};
constexpr Color kAxisColor[3] = {Color{0xd8, 0x47, 0x5a, 0xff},
                                 Color{0x6f, 0xb7, 0x4a, 0xff},
                                 Color{0x3f, 0x7a, 0xd0, 0xff}};
constexpr Color kGridColor{0x2a, 0x2d, 0x34, 140};        // opacity .55
constexpr Color kGridCenterColor{0x40, 0x44, 0x4c, 140};  // opacity .55

Color with_alpha(Color c, double a01) {
    c.a = static_cast<std::uint8_t>(
        std::clamp(std::lround(a01 * 255.0), 0L, 255L));
    return c;
}

// Parse "#rrggbb" (the HelperArt color strings).
Color hex_color(const char* s) {
    auto nib = [](char ch) -> unsigned {
        if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<unsigned>(ch - 'a') + 10u;
        if (ch >= 'A' && ch <= 'F') return static_cast<unsigned>(ch - 'A') + 10u;
        return 0u;
    };
    if (s == nullptr || s[0] != '#') return Color{0xff, 0x00, 0xff, 0xff};
    return Color{static_cast<std::uint8_t>((nib(s[1]) << 4) | nib(s[2])),
                 static_cast<std::uint8_t>((nib(s[3]) << 4) | nib(s[4])),
                 static_cast<std::uint8_t>((nib(s[5]) << 4) | nib(s[6])),
                 0xff};
}

// ── Vec3 / Mat3 math (Y-up right-handed, matching three.js) ────────────────

Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(const Vec3& a) {
    const double len = length(a);
    return len > 1e-12 ? mul(a, 1.0 / len) : Vec3{0.0, 0.0, 1.0};
}

struct Mat3 {
    Vec3 cx{1.0, 0.0, 0.0};  // columns
    Vec3 cy{0.0, 1.0, 0.0};
    Vec3 cz{0.0, 0.0, 1.0};
    [[nodiscard]] Vec3 apply(const Vec3& v) const {
        return add(add(mul(cx, v.x), mul(cy, v.y)), mul(cz, v.z));
    }
};
Mat3 mat_mul(const Mat3& a, const Mat3& b) {
    return {a.apply(b.cx), a.apply(b.cy), a.apply(b.cz)};
}

// three.js Euler order 'XYZ' (Matrix4.makeRotationFromEuler).
Mat3 rotation_xyz(const Vec3& e) {
    const double c1 = std::cos(e.x), s1 = std::sin(e.x);
    const double c2 = std::cos(e.y), s2 = std::sin(e.y);
    const double c3 = std::cos(e.z), s3 = std::sin(e.z);
    Mat3 m;
    m.cx = {c2 * c3, c1 * s3 + s1 * s2 * c3, s1 * s3 - c1 * s2 * c3};
    m.cy = {-c2 * s3, c1 * c3 - s1 * s2 * s3, s1 * c3 + c1 * s2 * s3};
    m.cz = {s2, -s1 * c2, c1 * c2};
    return m;
}

// Orientation with -Z facing (0, 0.6, 0) — the web bakes this lookAt into
// the camera helper at spawn; deriving it from the current position keeps
// the document's TRS authoritative (rotation edits compose on top).
Mat3 look_rotation(const Vec3& pos) {
    Vec3 z = sub(pos, Vec3{0.0, 0.6, 0.0});
    if (length(z) < 1e-9) return Mat3{};
    z = normalize(z);
    Vec3 x = cross(Vec3{0.0, 1.0, 0.0}, z);
    x = length(x) > 1e-6 ? normalize(x) : Vec3{1.0, 0.0, 0.0};
    const Vec3 y = cross(z, x);
    return {x, y, z};
}

struct Xform {
    Mat3 r;
    Vec3 t{};
    Vec3 s{1.0, 1.0, 1.0};
    [[nodiscard]] Vec3 apply(const Vec3& p) const {
        return add(t, r.apply(Vec3{p.x * s.x, p.y * s.y, p.z * s.z}));
    }
};

struct CamBasis {
    Vec3 eye, right, up, fwd;
};
CamBasis camera_basis(double yaw, double pitch, double dist,
                      const Vec3& target) {
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const Vec3 offset{cp * std::sin(yaw), sp, cp * std::cos(yaw)};
    CamBasis b;
    b.eye = add(target, mul(offset, dist));
    b.fwd = normalize(sub(target, b.eye));
    b.right = normalize(cross(b.fwd, Vec3{0.0, 1.0, 0.0}));
    b.up = cross(b.right, b.fwd);
    return b;
}

// Clip a view-space segment against the near plane (z = kNear, +z in front).
bool clip_near(Vec3& a, Vec3& b) {
    if (a.z <= kNear && b.z <= kNear) return false;
    if (a.z < kNear) {
        const double t = (kNear - a.z) / (b.z - a.z);
        a = add(a, mul(sub(b, a), t));
    } else if (b.z < kNear) {
        const double t = (kNear - b.z) / (a.z - b.z);
        b = add(b, mul(sub(a, b), t));
    }
    return true;
}

// ── Mesh tables ─────────────────────────────────────────────────────────────
// Tessellation constants chosen SVG-friendly (default scene stays well under
// ~2500 faces even with every primitive spawned once).

struct MeshData {
    std::vector<Vec3> verts;
    std::vector<std::array<int, 3>> tris;
    std::vector<std::array<int, 2>> edges;  // deduped wireframe edges
    Vec3 bb_min{}, bb_max{};
    MeshStats stats{};
    bool double_sided{false};
};

int add_vert(MeshData& m, double x, double y, double z) {
    m.verts.push_back({x, y, z});
    return static_cast<int>(m.verts.size()) - 1;
}
void add_tri(MeshData& m, int a, int b, int c) {
    m.tris.push_back({a, b, c});
    ++m.stats.faces;
    ++m.stats.tris;
}
void add_quad(MeshData& m, int a, int b, int c, int d) {
    m.tris.push_back({a, b, c});
    m.tris.push_back({a, c, d});
    ++m.stats.faces;
    m.stats.tris += 2;
}

void finalize(MeshData& m) {
    m.stats.verts = static_cast<int>(m.verts.size());
    // Wireframe edge set: every triangle edge, deduped (three.js
    // material.wireframe shows the triangulation diagonals too).
    std::vector<std::uint64_t> keys;
    keys.reserve(m.tris.size() * 3);
    for (const auto& t : m.tris) {
        for (int e = 0; e < 3; ++e) {
            const int a = t[static_cast<std::size_t>(e)];
            const int b = t[static_cast<std::size_t>((e + 1) % 3)];
            const auto lo = static_cast<std::uint64_t>(std::min(a, b));
            const auto hi = static_cast<std::uint64_t>(std::max(a, b));
            keys.push_back((lo << 32) | hi);
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    m.edges.reserve(keys.size());
    for (const std::uint64_t k : keys) {
        m.edges.push_back({static_cast<int>(k >> 32),
                           static_cast<int>(k & 0xffffffffULL)});
    }
    Vec3 mn{1e30, 1e30, 1e30}, mx{-1e30, -1e30, -1e30};
    for (const auto& v : m.verts) {
        mn = {std::min(mn.x, v.x), std::min(mn.y, v.y), std::min(mn.z, v.z)};
        mx = {std::max(mx.x, v.x), std::max(mx.y, v.y), std::max(mx.z, v.z)};
    }
    m.bb_min = mn;
    m.bb_max = mx;
}

MeshData make_cube() {
    MeshData m;
    const double h = 0.8;  // 1.6^3
    const int v[8] = {
        add_vert(m, -h, -h, -h), add_vert(m, h, -h, -h),
        add_vert(m, h, h, -h),   add_vert(m, -h, h, -h),
        add_vert(m, -h, -h, h),  add_vert(m, h, -h, h),
        add_vert(m, h, h, h),    add_vert(m, -h, h, h),
    };
    add_quad(m, v[1], v[2], v[6], v[5]);  // +X
    add_quad(m, v[4], v[7], v[3], v[0]);  // -X
    add_quad(m, v[3], v[7], v[6], v[2]);  // +Y
    add_quad(m, v[4], v[0], v[1], v[5]);  // -Y
    add_quad(m, v[4], v[5], v[6], v[7]);  // +Z
    add_quad(m, v[1], v[0], v[3], v[2]);  // -Z
    finalize(m);
    return m;
}

MeshData make_uv_sphere() {
    MeshData m;
    const double r = 0.9;
    const int seg = 16, rings = 12;
    const int top = add_vert(m, 0.0, r, 0.0);
    for (int ri = 1; ri < rings; ++ri) {
        const double phi = kPi * ri / rings;
        const double y = r * std::cos(phi);
        const double rad = r * std::sin(phi);
        for (int si = 0; si < seg; ++si) {
            const double th = 2.0 * kPi * si / seg;
            add_vert(m, rad * std::sin(th), y, rad * std::cos(th));
        }
    }
    const int bottom = add_vert(m, 0.0, -r, 0.0);
    auto ring_vert = [&](int ri, int si) {
        return 1 + (ri - 1) * seg + (si % seg);
    };
    for (int si = 0; si < seg; ++si) {
        add_tri(m, top, ring_vert(1, si), ring_vert(1, si + 1));
    }
    for (int ri = 1; ri < rings - 1; ++ri) {
        for (int si = 0; si < seg; ++si) {
            add_quad(m, ring_vert(ri, si), ring_vert(ri + 1, si),
                     ring_vert(ri + 1, si + 1), ring_vert(ri, si + 1));
        }
    }
    for (int si = 0; si < seg; ++si) {
        add_tri(m, ring_vert(rings - 1, si), bottom, ring_vert(rings - 1, si + 1));
    }
    finalize(m);
    return m;
}

struct IcoData {
    std::vector<Vec3> verts;
    std::vector<std::array<int, 3>> faces;
};
const IcoData& icosahedron() {
    static const IcoData ico = [] {
        IcoData d;
        const double t = (1.0 + std::sqrt(5.0)) * 0.5;
        const double raw[12][3] = {
            {-1, t, 0}, {1, t, 0},  {-1, -t, 0}, {1, -t, 0},
            {0, -1, t}, {0, 1, t},  {0, -1, -t}, {0, 1, -t},
            {t, 0, -1}, {t, 0, 1},  {-t, 0, -1}, {-t, 0, 1},
        };
        for (const auto& v : raw) {
            d.verts.push_back(normalize(Vec3{v[0], v[1], v[2]}));
        }
        d.faces = {
            {0, 11, 5}, {0, 5, 1},   {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
            {1, 5, 9},  {5, 11, 4},  {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
            {3, 9, 4},  {3, 4, 2},   {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
            {4, 9, 5},  {2, 4, 11},  {6, 2, 10},  {8, 6, 7},  {9, 8, 1},
        };
        return d;
    }();
    return ico;
}

MeshData make_icosphere() {
    MeshData m;
    const double r = 0.9;
    const IcoData& ico = icosahedron();
    for (const auto& v : ico.verts) {
        m.verts.push_back(mul(v, r));
    }
    // One midpoint subdivision (80 tris), midpoints projected to the sphere.
    std::map<std::uint64_t, int> mids;
    auto midpoint = [&](int a, int b) {
        const auto lo = static_cast<std::uint64_t>(std::min(a, b));
        const auto hi = static_cast<std::uint64_t>(std::max(a, b));
        const std::uint64_t key = (lo << 32) | hi;
        const auto it = mids.find(key);
        if (it != mids.end()) return it->second;
        const Vec3 p = normalize(add(m.verts[static_cast<std::size_t>(a)],
                                     m.verts[static_cast<std::size_t>(b)]));
        m.verts.push_back(mul(p, r));
        const int idx = static_cast<int>(m.verts.size()) - 1;
        mids.emplace(key, idx);
        return idx;
    };
    for (const auto& f : ico.faces) {
        const int ab = midpoint(f[0], f[1]);
        const int bc = midpoint(f[1], f[2]);
        const int ca = midpoint(f[2], f[0]);
        add_tri(m, f[0], ab, ca);
        add_tri(m, f[1], bc, ab);
        add_tri(m, f[2], ca, bc);
        add_tri(m, ab, bc, ca);
    }
    finalize(m);
    return m;
}

MeshData make_cylinder() {
    MeshData m;
    const double r = 0.7, h = 0.8;  // r 0.7, height 1.6
    const int seg = 16;
    for (int si = 0; si < seg; ++si) {
        const double th = 2.0 * kPi * si / seg;
        add_vert(m, r * std::sin(th), h, r * std::cos(th));
    }
    for (int si = 0; si < seg; ++si) {
        const double th = 2.0 * kPi * si / seg;
        add_vert(m, r * std::sin(th), -h, r * std::cos(th));
    }
    const int ct = add_vert(m, 0.0, h, 0.0);
    const int cb = add_vert(m, 0.0, -h, 0.0);
    auto top = [&](int si) { return si % seg; };
    auto bot = [&](int si) { return seg + (si % seg); };
    for (int si = 0; si < seg; ++si) {
        add_quad(m, top(si), bot(si), bot(si + 1), top(si + 1));
        add_tri(m, ct, top(si), top(si + 1));
        add_tri(m, cb, bot(si + 1), bot(si));
    }
    finalize(m);
    return m;
}

MeshData make_cone() {
    MeshData m;
    const double r = 0.85, h = 0.8;  // r 0.85, height 1.6
    const int seg = 16;
    const int apex = add_vert(m, 0.0, h, 0.0);
    for (int si = 0; si < seg; ++si) {
        const double th = 2.0 * kPi * si / seg;
        add_vert(m, r * std::sin(th), -h, r * std::cos(th));
    }
    const int cb = add_vert(m, 0.0, -h, 0.0);
    auto base = [&](int si) { return 1 + (si % seg); };
    for (int si = 0; si < seg; ++si) {
        add_tri(m, apex, base(si), base(si + 1));
        add_tri(m, cb, base(si + 1), base(si));
    }
    finalize(m);
    return m;
}

MeshData make_torus() {
    MeshData m;
    const double R = 0.8, r = 0.25;
    const int radial = 12, tubular = 20;
    // Generated lying flat (ring around the Y axis) — the web spawns the
    // three.js torus rotated flat, so the table bakes that in.
    for (int i = 0; i < tubular; ++i) {
        const double u = 2.0 * kPi * i / tubular;
        for (int j = 0; j < radial; ++j) {
            const double v = 2.0 * kPi * j / radial;
            add_vert(m, (R + r * std::cos(v)) * std::sin(u), r * std::sin(v),
                     (R + r * std::cos(v)) * std::cos(u));
        }
    }
    auto at = [&](int i, int j) {
        return (i % tubular) * radial + (j % radial);
    };
    for (int i = 0; i < tubular; ++i) {
        for (int j = 0; j < radial; ++j) {
            add_quad(m, at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1));
        }
    }
    finalize(m);
    return m;
}

MeshData make_plane() {
    MeshData m;
    // 2x2, generated flat (web spawns it flat at y = 0.001); visible from
    // both sides.
    const int a = add_vert(m, -1.0, 0.0, -1.0);
    const int b = add_vert(m, -1.0, 0.0, 1.0);
    const int c = add_vert(m, 1.0, 0.0, 1.0);
    const int d = add_vert(m, 1.0, 0.0, -1.0);
    add_quad(m, a, b, c, d);
    m.double_sided = true;
    finalize(m);
    return m;
}

const MeshData* mesh_for(std::string_view type) {
    static const MeshData cube = make_cube();
    static const MeshData sphere = make_uv_sphere();
    static const MeshData icosphere = make_icosphere();
    static const MeshData cylinder = make_cylinder();
    static const MeshData cone = make_cone();
    static const MeshData torus = make_torus();
    static const MeshData plane = make_plane();
    if (type == "Cube") return &cube;
    if (type == "UV Sphere") return &sphere;
    if (type == "Icosphere") return &icosphere;
    if (type == "Cylinder") return &cylinder;
    if (type == "Cone") return &cone;
    if (type == "Torus") return &torus;
    if (type == "Plane") return &plane;
    return nullptr;
}

// ── Helper art (lights / camera / empty as stroked polylines) ───────────────

struct HelperArt {
    std::vector<std::array<Vec3, 2>> segments;  // object-space line segments
    const char* color{"#a0a4ad"};
    double halo_radius{0.0};  // >0: screen-billboard circle (world units)
    int spokes{0};            // sun ray count (screen billboard)
    double spoke_r0{0.0};
    double spoke_r1{0.0};
    Vec3 bb_min{}, bb_max{};
    bool aim_at_focus{false};  // camera helper lookAt orientation
};

void helper_bounds(HelperArt& a) {
    Vec3 mn{1e30, 1e30, 1e30}, mx{-1e30, -1e30, -1e30};
    for (const auto& s : a.segments) {
        for (const auto& p : s) {
            mn = {std::min(mn.x, p.x), std::min(mn.y, p.y),
                  std::min(mn.z, p.z)};
            mx = {std::max(mx.x, p.x), std::max(mx.y, p.y),
                  std::max(mx.z, p.z)};
        }
    }
    const double r = std::max(a.halo_radius,
                              a.spokes > 0 ? a.spoke_r1 : 0.0);
    if (a.segments.empty()) {
        mn = {-r, -r, -r};
        mx = {r, r, r};
    } else if (r > 0.0) {
        mn = {std::min(mn.x, -r), std::min(mn.y, -r), std::min(mn.z, -r)};
        mx = {std::max(mx.x, r), std::max(mx.y, r), std::max(mx.z, r)};
    }
    a.bb_min = mn;
    a.bb_max = mx;
}

HelperArt make_point_light() {
    HelperArt a;
    a.color = "#ffd060";
    a.halo_radius = 0.34;
    // Small icosahedron wire (the web light helper's edge sphere).
    const IcoData& ico = icosahedron();
    std::vector<std::uint64_t> keys;
    for (const auto& f : ico.faces) {
        for (int e = 0; e < 3; ++e) {
            const int p = f[static_cast<std::size_t>(e)];
            const int q = f[static_cast<std::size_t>((e + 1) % 3)];
            const auto lo = static_cast<std::uint64_t>(std::min(p, q));
            const auto hi = static_cast<std::uint64_t>(std::max(p, q));
            keys.push_back((lo << 32) | hi);
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (const std::uint64_t k : keys) {
        const auto p = ico.verts[static_cast<std::size_t>(k >> 32)];
        const auto q = ico.verts[static_cast<std::size_t>(k & 0xffffffffULL)];
        a.segments.push_back({mul(p, 0.22), mul(q, 0.22)});
    }
    helper_bounds(a);
    return a;
}

HelperArt make_sun() {
    HelperArt a;
    a.color = "#ffe2b5";
    a.halo_radius = 0.28;
    a.spokes = 8;
    a.spoke_r0 = 0.4;
    a.spoke_r1 = 0.62;
    helper_bounds(a);
    return a;
}

HelperArt make_spot() {
    HelperArt a;
    a.color = "#fff0c0";
    // Cone edges from the apex toward -Y (web target (0, -1, 0)):
    // half-angle pi/6 over length 1.2.
    const double len = 1.2;
    const double rim = std::tan(kPi / 6.0) * len;
    const int seg = 16;
    Vec3 prev{rim * std::sin(0.0), -len, rim * std::cos(0.0)};
    for (int i = 1; i <= seg; ++i) {
        const double th = 2.0 * kPi * i / seg;
        const Vec3 p{rim * std::sin(th), -len, rim * std::cos(th)};
        a.segments.push_back({prev, p});
        prev = p;
    }
    for (int i = 0; i < 4; ++i) {
        const double th = kPi * 0.5 * i;
        a.segments.push_back(
            {Vec3{0.0, 0.0, 0.0},
             Vec3{rim * std::sin(th), -len, rim * std::cos(th)}});
    }
    helper_bounds(a);
    return a;
}

HelperArt make_camera() {
    HelperArt a;
    a.color = "#b6bcc7";
    a.aim_at_focus = true;
    // Wire body box behind the "lens" plus frustum lines toward -Z.
    const double bx = 0.25, by = 0.18, bz0 = 0.1, bz1 = 0.6;
    const Vec3 c[8] = {
        {-bx, -by, bz0}, {bx, -by, bz0}, {bx, by, bz0}, {-bx, by, bz0},
        {-bx, -by, bz1}, {bx, -by, bz1}, {bx, by, bz1}, {-bx, by, bz1},
    };
    const int be[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                           {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : be) a.segments.push_back({c[e[0]], c[e[1]]});
    const double fx = 0.34, fy = 0.24, fz = -0.5;
    const Vec3 f[4] = {
        {-fx, -fy, fz}, {fx, -fy, fz}, {fx, fy, fz}, {-fx, fy, fz}};
    for (int i = 0; i < 4; ++i) {
        a.segments.push_back({c[i], f[i]});
        a.segments.push_back({f[i], f[(i + 1) % 4]});
    }
    helper_bounds(a);
    return a;
}

HelperArt make_empty() {
    HelperArt a;
    a.color = "#a0a4ad";
    for (int axis = 0; axis < 3; ++axis) {
        Vec3 d{axis == 0 ? 0.5 : 0.0, axis == 1 ? 0.5 : 0.0,
               axis == 2 ? 0.5 : 0.0};
        a.segments.push_back({mul(d, -1.0), d});
    }
    helper_bounds(a);
    return a;
}

const HelperArt* helper_for(std::string_view type) {
    static const HelperArt point_light = make_point_light();
    static const HelperArt sun = make_sun();
    static const HelperArt spot = make_spot();
    static const HelperArt camera = make_camera();
    static const HelperArt empty = make_empty();
    if (type == "Point Light") return &point_light;
    if (type == "Sun") return &sun;
    if (type == "Spot") return &spot;
    if (type == "Camera") return &camera;
    if (type == "Empty") return &empty;
    return nullptr;
}

// ── Shading / formatting ────────────────────────────────────────────────────

struct Rgb {
    double r, g, b;
};

// Flat shading: key light normalize(5, 7, 4) at ~1.0 diffuse weight plus a
// hemisphere ambient (sky 0xe6efff * 0.25 up, ground 0x1a1c20 down), clamped.
Rgb shade_face(const Vec3& n) {
    static const Vec3 kKeyDir = normalize(Vec3{5.0, 7.0, 4.0});
    const double diff = std::max(0.0, dot(n, kKeyDir));
    const double t = 0.5 * (n.y + 1.0);
    const Rgb sky{0.902 * 0.25, 0.937 * 0.25, 1.0 * 0.25};
    const Rgb ground{0.102, 0.110, 0.125};
    const Rgb amb{sky.r * t + ground.r * (1.0 - t),
                  sky.g * t + ground.g * (1.0 - t),
                  sky.b * t + ground.b * (1.0 - t)};
    const Rgb base{154.0 / 255.0, 161.0 / 255.0, 173.0 / 255.0};  // 0x9aa1ad
    auto ch = [&](double b, double a) {
        return std::clamp(b * (diff + a), 0.0, 1.0);
    };
    return {ch(base.r, amb.r), ch(base.g, amb.g), ch(base.b, amb.b)};
}

Color rgb_color(const Rgb& c) {
    auto ch = [](double v) {
        return static_cast<std::uint8_t>(
            std::clamp(std::lround(v * 255.0), 0L, 255L));
    };
    return Color{ch(c.r), ch(c.g), ch(c.b), 0xff};
}

bool point_in_tri(double px, double py, double x0, double y0, double x1,
                  double y1, double x2, double y2) {
    auto side = [](double ax, double ay, double bx, double by, double cx,
                   double cy) {
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    };
    const double d0 = side(x0, y0, x1, y1, px, py);
    const double d1 = side(x1, y1, x2, y2, px, py);
    const double d2 = side(x2, y2, x0, y0, px, py);
    const bool neg = d0 < 0.0 || d1 < 0.0 || d2 < 0.0;
    const bool pos = d0 > 0.0 || d1 > 0.0 || d2 > 0.0;
    return !(neg && pos);
}

double dist_to_segment(double px, double py, double x0, double y0, double x1,
                       double y1) {
    const double dx = x1 - x0, dy = y1 - y0;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 1e-12 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double cx = x0 + dx * t, cy = y0 + dy * t;
    return std::hypot(px - cx, py - cy);
}

bool chain_entry_has(const affineui::Document::HoverInfo& info,
                     std::string_view cls) {
    for (const auto& c : info.classes) {
        if (c == cls) return true;
    }
    return false;
}

}  // namespace

// ── Public stats helpers ────────────────────────────────────────────────────

const MeshStats* mesh_stats_for(std::string_view type) {
    const MeshData* m = mesh_for(type);
    return m != nullptr ? &m->stats : nullptr;
}

SceneStats scene_stats(const DenderDocument& doc) {
    SceneStats st;
    st.total = static_cast<int>(doc.object_count());
    for (const auto& obj : doc.objects()) {
        if (doc.is_selected(obj.id)) ++st.selected;
    }
    const SceneObject* active = doc.active();
    if (const MeshStats* ms =
            active != nullptr ? mesh_stats_for(active->type) : nullptr) {
        st.verts = ms->verts;
        st.faces = ms->faces;
        st.tris = ms->tris;
        return st;
    }
    for (const auto& obj : doc.objects()) {  // fall back to scene totals
        if (const MeshStats* ms = mesh_stats_for(obj.type)) {
            st.verts += ms->verts;
            st.faces += ms->faces;
            st.tris += ms->tris;
        }
    }
    return st;
}

// ── DenderViewport ──────────────────────────────────────────────────────────

DenderViewport::DenderViewport(const DenderDocument& doc) : doc_(doc) {
    // Derive yaw/pitch/dist from the web's initial eye (5.2, 3.4, 6.4).
    const Vec3 off = sub(Vec3{5.2, 3.4, 6.4}, target_);
    dist_ = length(off);
    pitch_ = std::asin(off.y / dist_);
    yaw_ = std::atan2(off.x, off.z);
}

void DenderViewport::attach(affineui::App& app) {
    app_ = &app;
    app.on_event([this](const affineui::Event& ev,
                        const std::vector<affineui::Document::HoverInfo>&
                            chain) { return handle_event(ev, chain); });
    if (kViewportPaintEnabled) {
        app.set_custom_paint(kScenePaintName,
                             [this](affineui::Painter& p, const Rect& r) {
                                 paint_scene(p, r);
                             });
        app.set_custom_paint(kGizmoPaintName,
                             [this](affineui::Painter& p, const Rect& r) {
                                 paint_gizmo(p, r);
                             });
    }
}

void DenderViewport::camera_changed() {
    pitch_ = std::clamp(pitch_, kPitchMin, kPitchMax);
    dist_ = std::clamp(dist_, kMinDist, kMaxDist);
    // Camera moves are geometry-only: repaint the two canvas rects. No
    // View rebuild, no reconcile, no restyle — the paint handlers read
    // the fresh camera when the renderer records the dirty rects.
    if (!kViewportPaintEnabled) return;  // TEMP: viewport painting disabled
    if (app_ != nullptr) {
        app_->request_custom_repaint(kScenePaintName);
        app_->request_custom_repaint(kGizmoPaintName);
    } else if (request_render) {
        request_render();  // headless fallback (no live document blocks)
    }
}

void DenderViewport::dolly(double factor) {
    dist_ *= factor;
    camera_changed();
}

void DenderViewport::toggle_pan_mode() {
    pan_mode_ = !pan_mode_;
    if (request_render) request_render();  // nav button aria-pressed
}

void DenderViewport::orbit(double dx, double dy) {
    // OrbitControls rotate: 2*pi per client-height pixels, both axes.
    const double h = canvas_rect_.h > 0 ? canvas_rect_.h : kFallbackH;
    yaw_ -= 2.0 * kPi * dx / h * kRotateSpeed;
    pitch_ += 2.0 * kPi * dy / h * kRotateSpeed;
    camera_changed();
}

void DenderViewport::pan(double dx, double dy) {
    // OrbitControls pan: the scene follows the cursor in the view plane.
    const double h = canvas_rect_.h > 0 ? canvas_rect_.h : kFallbackH;
    const double wpp = 2.0 * dist_ * std::tan(kFovY * 0.5) / h * kPanSpeed;
    const CamBasis b = camera_basis(yaw_, pitch_, dist_, target_);
    target_ = add(target_,
                  add(mul(b.right, -dx * wpp), mul(b.up, dy * wpp)));
    camera_changed();
}

void DenderViewport::snap_to_axis(int axis) {
    static const Vec3 kDirs[6] = {{1, 0, 0},  {0, 1, 0},  {0, 0, 1},
                                  {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const Vec3 d = kDirs[axis];
    if (std::abs(d.y) > 0.999) {
        // Top keeps the current heading; bottom clamps to the polar limit
        // (the web's controls.update() does the same clamp).
        pitch_ = d.y > 0.0 ? kPitchMax : kPitchMin;
    } else {
        pitch_ = kPitchMin;  // horizon-level, clamped above the ground
        yaw_ = std::atan2(d.x, d.z);
    }
    camera_changed();
}

void DenderViewport::frame_selected() {
    // Fit the selection, else the whole scene (web frameSelected).
    Vec3 mn{1e30, 1e30, 1e30}, mx{-1e30, -1e30, -1e30};
    bool any = false;
    auto extend = [&](const SceneObject& obj) {
        Vec3 lo, hi;
        const HelperArt* art = helper_for(obj.type);
        if (const MeshData* mesh = mesh_for(obj.type)) {
            lo = mesh->bb_min;
            hi = mesh->bb_max;
        } else if (art != nullptr) {
            lo = art->bb_min;
            hi = art->bb_max;
        } else {
            return;
        }
        Xform x;
        x.t = obj.position;
        x.s = obj.scale;
        x.r = rotation_xyz(obj.rotation);
        if (art != nullptr && art->aim_at_focus) {
            x.r = mat_mul(look_rotation(obj.position), x.r);
        }
        for (int i = 0; i < 8; ++i) {
            const Vec3 corner{(i & 1) != 0 ? hi.x : lo.x,
                              (i & 2) != 0 ? hi.y : lo.y,
                              (i & 4) != 0 ? hi.z : lo.z};
            const Vec3 wp = x.apply(corner);
            mn = {std::min(mn.x, wp.x), std::min(mn.y, wp.y),
                  std::min(mn.z, wp.z)};
            mx = {std::max(mx.x, wp.x), std::max(mx.y, wp.y),
                  std::max(mx.z, wp.z)};
            any = true;
        }
    };
    bool have_selection = false;
    for (const auto& obj : doc_.objects()) {
        if (doc_.is_selected(obj.id)) {
            extend(obj);
            have_selection = true;
        }
    }
    if (!have_selection) {
        for (const auto& obj : doc_.objects()) extend(obj);
    }
    if (any) {
        target_ = mul(add(mn, mx), 0.5);
        dist_ = std::max(2.0, length(sub(mx, mn)) * 1.4);
    } else {
        target_ = {0.0, 0.6, 0.0};
    }
    camera_changed();
}

// ── Declaration side ────────────────────────────────────────────────────────

void DenderViewport::scene_layer(affineui::View& v) {
    // The declaration is camera-independent: a wrapper for stacking order
    // plus the custom-paint canvas block. All drawing happens in
    // paint_scene at repaint time (headless dumps show an empty region,
    // matching the web demo's runtime-drawn three.js canvas).
    auto wrap = v.element("div", "dn-vp-scenelayer", "vp-scene-layer");
    wrap.attr("id", "vp-scene");
    wrap.attr("data-aui-name", kSceneLayerName);
    auto canvas = v.canvas(kScenePaintName, {}, "vp-scene-canvas");
    canvas.attr("style", "position:absolute;inset:0");
}

void DenderViewport::paint_scene(affineui::Painter& p, const Rect& r) {
    canvas_rect_ = r;  // pick coordinates convert through the live rect
    pick_faces_.clear();
    pick_lines_.clear();
    pick_ids_.clear();

    const double w = static_cast<double>(r.w);
    const double h = static_cast<double>(r.h);
    if (w <= 0.0 || h <= 0.0) return;
    const float ox = static_cast<float>(r.x);
    const float oy = static_cast<float>(r.y);
    p.push_clip(r);

    const CamBasis cb = camera_basis(yaw_, pitch_, dist_, target_);
    const double fl = (h * 0.5) / std::tan(kFovY * 0.5);
    const double cx = w * 0.5, cy = h * 0.5;
    auto view_of = [&](const Vec3& pt) {
        const Vec3 d = sub(pt, cb.eye);
        return Vec3{dot(d, cb.right), dot(d, cb.up), dot(d, cb.fwd)};
    };
    auto screen_x = [&](const Vec3& vv) { return cx + vv.x * fl / vv.z; };
    auto screen_y = [&](const Vec3& vv) { return cy - vv.y * fl / vv.z; };
    // Append a near-clipped world segment into a Painter path-command
    // buffer (screen coords are canvas-local; the ox/oy offset places
    // them in document space).
    auto put_seg = [&](std::vector<float>& d, const Vec3& wa, const Vec3& wb,
                       double* out_depth = nullptr) {
        Vec3 va = view_of(wa), vb = view_of(wb);
        if (!clip_near(va, vb)) return false;
        d.push_back(affineui::kPathMove);
        d.push_back(ox + static_cast<float>(screen_x(va)));
        d.push_back(oy + static_cast<float>(screen_y(va)));
        d.push_back(affineui::kPathLine);
        d.push_back(ox + static_cast<float>(screen_x(vb)));
        d.push_back(oy + static_cast<float>(screen_y(vb)));
        if (out_depth != nullptr) *out_depth = (va.z + vb.z) * 0.5;
        return true;
    };
    auto stroke_cmds = [&](const std::vector<float>& d, Color c,
                           float width) {
        if (!d.empty()) {
            p.stroke_path(d.data(), d.size(), PathPaint::solid(c), width);
        }
    };

    // ── Ground grid (GridHelper 20x20: center lines 0x40444c, others
    //    0x2a2d34, opacity .55) — two batched strokes. ──
    {
        std::vector<float> center_d, grid_d;
        for (int i = -10; i <= 10; ++i) {
            std::vector<float>& d = i == 0 ? center_d : grid_d;
            const double fi = static_cast<double>(i);
            put_seg(d, Vec3{fi, 0.0, -10.0}, Vec3{fi, 0.0, 10.0});
            put_seg(d, Vec3{-10.0, 0.0, fi}, Vec3{10.0, 0.0, fi});
        }
        stroke_cmds(grid_d, kGridColor, 1.0f);
        stroke_cmds(center_d, kGridCenterColor, 1.0f);
    }

    // ── Scene objects: gather faces (all objects, one global depth sort);
    //    wire/helper strokes and selection boxes collect into layers drawn
    //    above the fills, mirroring the old SVG stacking order. ──
    const bool wire = doc_.wireframe();
    struct RenderFace {
        double depth;
        double x[3], y[3];
        Color col;
    };
    std::vector<RenderFace> faces;
    faces.reserve(512);
    struct StrokeBatch {
        std::vector<float> d;
        Color c;
        float w;
    };
    std::vector<StrokeBatch> line_batches;
    struct CircleItem {
        float x, y, r;
        Color c;
        float w;
    };
    std::vector<CircleItem> circles;
    std::vector<StrokeBatch> select_batches;

    std::vector<Vec3> view_pts;
    std::vector<double> sx_pts, sy_pts;

    for (const auto& obj : doc_.objects()) {
        pick_ids_.push_back(obj.id);
        const int oi = static_cast<int>(pick_ids_.size()) - 1;
        const HelperArt* art = helper_for(obj.type);
        Xform x;
        x.t = obj.position;
        x.s = obj.scale;
        x.r = rotation_xyz(obj.rotation);
        if (art != nullptr && art->aim_at_focus) {
            x.r = mat_mul(look_rotation(obj.position), x.r);
        }

        if (const MeshData* mesh = mesh_for(obj.type)) {
            const std::size_t n = mesh->verts.size();
            view_pts.resize(n);
            sx_pts.resize(n);
            sy_pts.resize(n);
            std::vector<Vec3> world_pts(n);
            for (std::size_t i = 0; i < n; ++i) {
                world_pts[i] = x.apply(mesh->verts[i]);
                view_pts[i] = view_of(world_pts[i]);
                if (view_pts[i].z > kNear) {
                    sx_pts[i] = screen_x(view_pts[i]);
                    sy_pts[i] = screen_y(view_pts[i]);
                }
            }
            for (const auto& t : mesh->tris) {
                const auto a = static_cast<std::size_t>(t[0]);
                const auto b = static_cast<std::size_t>(t[1]);
                const auto c = static_cast<std::size_t>(t[2]);
                if (view_pts[a].z <= kNear || view_pts[b].z <= kNear ||
                    view_pts[c].z <= kNear) {
                    continue;  // whole-face near cull (scene sits far away)
                }
                Vec3 nrm = normalize(cross(sub(world_pts[b], world_pts[a]),
                                           sub(world_pts[c], world_pts[a])));
                const Vec3 centroid = mul(
                    add(add(world_pts[a], world_pts[b]), world_pts[c]),
                    1.0 / 3.0);
                if (dot(nrm, sub(cb.eye, centroid)) <= 0.0) {
                    if (!mesh->double_sided) continue;  // backface cull
                    nrm = mul(nrm, -1.0);
                }
                const double depth =
                    (view_pts[a].z + view_pts[b].z + view_pts[c].z) / 3.0;
                pick_faces_.push_back({sx_pts[a], sy_pts[a], sx_pts[b],
                                       sy_pts[b], sx_pts[c], sy_pts[c], depth,
                                       oi});
                if (!wire) {
                    RenderFace rf;
                    rf.depth = depth;
                    rf.x[0] = sx_pts[a];
                    rf.y[0] = sy_pts[a];
                    rf.x[1] = sx_pts[b];
                    rf.y[1] = sy_pts[b];
                    rf.x[2] = sx_pts[c];
                    rf.y[2] = sy_pts[c];
                    rf.col = rgb_color(shade_face(nrm));
                    faces.push_back(rf);
                }
            }
            if (wire) {
                std::vector<float> d;
                d.reserve(mesh->edges.size() * 6);
                for (const auto& e : mesh->edges) {
                    put_seg(d, world_pts[static_cast<std::size_t>(e[0])],
                            world_pts[static_cast<std::size_t>(e[1])]);
                }
                if (!d.empty()) {
                    line_batches.push_back({std::move(d), kBaseColor, 1.0f});
                }
            }
        } else if (art != nullptr) {
            // Stroked polyline helper (screen-constant ~1.2px strokes).
            const Color art_color = hex_color(art->color);
            std::vector<float> d;
            for (const auto& seg : art->segments) {
                const Vec3 wa = x.apply(seg[0]);
                const Vec3 wb = x.apply(seg[1]);
                double depth = 0.0;
                Vec3 va = view_of(wa), vb = view_of(wb);
                if (!clip_near(va, vb)) continue;
                depth = (va.z + vb.z) * 0.5;
                const double ax = screen_x(va), ay = screen_y(va);
                const double bx = screen_x(vb), by = screen_y(vb);
                d.push_back(affineui::kPathMove);
                d.push_back(ox + static_cast<float>(ax));
                d.push_back(oy + static_cast<float>(ay));
                d.push_back(affineui::kPathLine);
                d.push_back(ox + static_cast<float>(bx));
                d.push_back(oy + static_cast<float>(by));
                pick_lines_.push_back({ax, ay, bx, by, depth, 4.0, oi});
            }
            const Vec3 vo = view_of(x.t);
            if (vo.z > kNear) {
                const double hox = screen_x(vo), hoy = screen_y(vo);
                const double scale = fl / vo.z;
                if (art->halo_radius > 0.0) {
                    const double hr = art->halo_radius * scale;
                    circles.push_back({ox + static_cast<float>(hox),
                                       oy + static_cast<float>(hoy),
                                       static_cast<float>(hr), art_color,
                                       1.2f});
                    pick_lines_.push_back(
                        {hox, hoy, hox, hoy, vo.z, hr + 3.0, oi});
                }
                for (int k = 0; k < art->spokes; ++k) {
                    const double th = 2.0 * kPi * k / art->spokes;
                    const double dx = std::cos(th), dy = std::sin(th);
                    d.push_back(affineui::kPathMove);
                    d.push_back(ox + static_cast<float>(
                                         hox + dx * art->spoke_r0 * scale));
                    d.push_back(oy + static_cast<float>(
                                         hoy + dy * art->spoke_r0 * scale));
                    d.push_back(affineui::kPathLine);
                    d.push_back(ox + static_cast<float>(
                                         hox + dx * art->spoke_r1 * scale));
                    d.push_back(oy + static_cast<float>(
                                         hoy + dy * art->spoke_r1 * scale));
                }
            }
            if (!d.empty()) {
                line_batches.push_back({std::move(d), art_color, 1.2f});
            }
        }

        // Selection box: object-oriented bounds (rotates/scales with the
        // object), active bright, extra multi-select members dimmed.
        if (doc_.is_selected(obj.id)) {
            Vec3 lo{}, hi{};
            bool have = false;
            if (const MeshData* mesh = mesh_for(obj.type)) {
                lo = mesh->bb_min;
                hi = mesh->bb_max;
                have = true;
            } else if (art != nullptr) {
                lo = art->bb_min;
                hi = art->bb_max;
                have = true;
            }
            if (have) {
                Vec3 corners[8];
                for (int i = 0; i < 8; ++i) {
                    corners[i] = x.apply(Vec3{(i & 1) != 0 ? hi.x : lo.x,
                                              (i & 2) != 0 ? hi.y : lo.y,
                                              (i & 4) != 0 ? hi.z : lo.z});
                }
                std::vector<float> d;
                for (int i = 0; i < 8; ++i) {
                    for (int bit = 0; bit < 3; ++bit) {
                        if ((i & (1 << bit)) != 0) continue;
                        put_seg(d, corners[i], corners[i | (1 << bit)]);
                    }
                }
                if (!d.empty()) {
                    const bool active = doc_.active_id() == obj.id;
                    select_batches.push_back(
                        {std::move(d),
                         active ? kSelectColor
                                : with_alpha(kSelectColor, 0.45),
                         1.2f});
                }
            }
        }
    }

    // Painter's algorithm across ALL objects: far faces first.
    std::sort(faces.begin(), faces.end(),
              [](const RenderFace& a, const RenderFace& b) {
                  return a.depth > b.depth;
              });
    for (const auto& fce : faces) {
        const float tri[] = {
            affineui::kPathMove,
            ox + static_cast<float>(fce.x[0]),
            oy + static_cast<float>(fce.y[0]),
            affineui::kPathLine,
            ox + static_cast<float>(fce.x[1]),
            oy + static_cast<float>(fce.y[1]),
            affineui::kPathLine,
            ox + static_cast<float>(fce.x[2]),
            oy + static_cast<float>(fce.y[2]),
            affineui::kPathClose,
        };
        const PathPaint paint = PathPaint::solid(fce.col);
        p.fill_path(tri, std::size(tri), paint);
        // Tiny same-color stroke hides antialiasing seams between fills.
        p.stroke_path(tri, std::size(tri), paint, 0.25f);
    }
    for (const auto& b : line_batches) stroke_cmds(b.d, b.c, b.w);
    for (const auto& c : circles) {
        p.stroke_arc(c.x, c.y, c.r, 0.0f, 360.0f, c.c, c.w);
    }
    for (const auto& b : select_batches) stroke_cmds(b.d, b.c, b.w);

    // ── World-axes gnomon: drawn last (renders over the scene). ──
    for (int axis = 0; axis < 3; ++axis) {
        const Vec3 dir{axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0,
                       axis == 2 ? 1.0 : 0.0};
        std::vector<float> d;
        if (put_seg(d, Vec3{0.0, 0.0, 0.0}, mul(dir, 1.06))) {
            stroke_cmds(d, kAxisColor[axis], 2.0f);
        }
        Vec3 vb0 = view_of(mul(dir, 1.06));
        Vec3 vt = view_of(mul(dir, 1.4));
        if (vb0.z > kNear && vt.z > kNear) {
            const double bx = screen_x(vb0), by = screen_y(vb0);
            const double tx = screen_x(vt), ty = screen_y(vt);
            const double hx = tx - bx, hy = ty - by;
            const double hl = std::hypot(hx, hy);
            if (hl > 1e-6) {
                const double px = -hy / hl * 3.5, py = hx / hl * 3.5;
                const float head[] = {
                    affineui::kPathMove,
                    ox + static_cast<float>(tx),
                    oy + static_cast<float>(ty),
                    affineui::kPathLine,
                    ox + static_cast<float>(bx + px),
                    oy + static_cast<float>(by + py),
                    affineui::kPathLine,
                    ox + static_cast<float>(bx - px),
                    oy + static_cast<float>(by - py),
                    affineui::kPathClose,
                };
                p.fill_path(head, std::size(head),
                            PathPaint::solid(kAxisColor[axis]));
            }
        }
    }

    p.pop_clip();
}

void DenderViewport::paint_gizmo(affineui::Painter& p, const Rect& r) {
    if (r.w <= 0 || r.h <= 0) return;
    // Drawn in the web gizmo's 72x72 local space, scaled to the block.
    const double s = std::min(r.w, r.h) / 72.0;
    auto gx = [&](double v) {
        return static_cast<float>(static_cast<double>(r.x) + v * s);
    };
    auto gy = [&](double v) {
        return static_cast<float>(static_cast<double>(r.y) + v * s);
    };
    const auto sw = [&](double v) { return static_cast<float>(v * s); };

    const CamBasis b = camera_basis(yaw_, pitch_, dist_, target_);
    // Direction from target toward the eye — nubs pointing this way face
    // the viewer; the web dims back-facing nubs at depth < -0.05.
    const Vec3 back = mul(b.fwd, -1.0);

    struct Nub {
        double x, y, depth;
        int axis;
    };
    std::array<Nub, 6> nubs{};
    static const Vec3 kDirs[6] = {{1, 0, 0},  {0, 1, 0},  {0, 0, 1},
                                  {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    for (int i = 0; i < 6; ++i) {
        const Vec3& d = kDirs[i];
        const double px = dot(d, b.right);
        const double py = dot(d, b.up);
        nubs[static_cast<std::size_t>(i)] = {36.0 + px * 26.0,
                                             36.0 - py * 26.0,
                                             dot(d, back), i};
        // Click hit-testing stays in 72-space (handle_event scales the
        // gizmo bounds back to it).
        nubs_[static_cast<std::size_t>(i)] = {36.0 + px * 26.0,
                                              36.0 - py * 26.0, i};
    }
    std::sort(nubs.begin(), nubs.end(),
              [](const Nub& a, const Nub& n) { return a.depth < n.depth; });

    static const Color kLetterColor{0x10, 0x13, 0x1a, 0xff};
    static const Color kRingFill{0x22, 0x26, 0x2d, 0xff};
    for (const auto& n : nubs) {
        const int color_axis = n.axis % 3;
        const bool positive = n.axis < 3;
        const double alpha = n.depth < -0.05 ? 0.55 : 1.0;
        const Color axis_col = with_alpha(kAxisColor[color_axis], alpha);
        if (positive) {
            // Axis line from the ball center to the nub, then the filled
            // nub with its letter (drawn as strokes, like the web's).
            p.stroke_line(gx(36.0), gy(36.0), gx(n.x), gy(n.y), axis_col,
                          sw(2.0));
            p.fill_circle(gx(n.x), gy(n.y), sw(7.5), axis_col);
            std::vector<float> d;
            auto rel = [&](double dx, double dy, bool move) {
                d.push_back(move ? affineui::kPathMove : affineui::kPathLine);
                d.push_back(gx(n.x + dx));
                d.push_back(gy(n.y + dy));
            };
            if (color_axis == 0) {  // X
                rel(-2.3, -2.7, true);
                rel(2.3, 2.7, false);
                rel(2.3, -2.7, true);
                rel(-2.3, 2.7, false);
            } else if (color_axis == 1) {  // Y
                rel(-2.3, -2.7, true);
                rel(0.0, 0.0, false);
                rel(2.3, -2.7, true);
                rel(0.0, 0.0, false);
                rel(0.0, 2.7, false);
            } else {  // Z
                rel(-2.2, -2.6, true);
                rel(2.2, -2.6, false);
                rel(-2.2, 2.6, false);
                rel(2.2, 2.6, false);
            }
            p.stroke_path(d.data(), d.size(),
                          PathPaint::solid(with_alpha(kLetterColor, alpha)),
                          sw(1.3));
        } else {
            // Negative axes: hollow ring.
            p.fill_circle(gx(n.x), gy(n.y), sw(6.0),
                          with_alpha(kRingFill, alpha));
            p.stroke_arc(gx(n.x), gy(n.y), sw(6.0), 0.0f, 360.0f, axis_col,
                         sw(1.5));
        }
    }
}

// ── Runtime side ────────────────────────────────────────────────────────────

void DenderViewport::pick_at(double local_x, double local_y,
                             bool shift) const {
    double best = std::numeric_limits<double>::infinity();
    int best_obj = -1;
    // Faces and helper lines compete on view depth (nearest hit wins) —
    // the screen-space analog of the web's raycast.
    for (const auto& fc : pick_faces_) {
        if (fc.depth < best && point_in_tri(local_x, local_y, fc.x0, fc.y0,
                                            fc.x1, fc.y1, fc.x2, fc.y2)) {
            best = fc.depth;
            best_obj = fc.obj;
        }
    }
    for (const auto& ln : pick_lines_) {
        if (ln.depth < best && dist_to_segment(local_x, local_y, ln.x0, ln.y0,
                                               ln.x1, ln.y1) <= ln.tol) {
            best = ln.depth;
            best_obj = ln.obj;
        }
    }
    if (on_pick) {
        on_pick(best_obj >= 0
                    ? std::string_view{pick_ids_[static_cast<std::size_t>(
                          best_obj)]}
                    : std::string_view{},
                shift);
    }
}

bool DenderViewport::handle_event(
    const affineui::Event& ev,
    const std::vector<affineui::Document::HoverInfo>& chain) {
    using ET = affineui::EventType;

    // Track modifiers: on_click callbacks carry none, so the nav Zoom
    // button reads the last-seen shift state.
    if (ev.type == ET::MouseMove || ev.type == ET::MouseDown ||
        ev.type == ET::MouseUp || ev.type == ET::KeyDown ||
        ev.type == ET::KeyUp) {
        shift_down_ = ev.shift;
    }

    const double mx = static_cast<double>(ev.pos.x);
    const double my = static_cast<double>(ev.pos.y);

    if (dragging_) {
        if (ev.type == ET::MouseMove) {
            const double dx = mx - last_x_;
            const double dy = my - last_y_;
            last_x_ = mx;
            last_y_ = my;
            if (std::abs(mx - down_x_) > kClickSlopPx ||
                std::abs(my - down_y_) > kClickSlopPx) {
                drag_moved_ = true;
            }
            if (drag_pan_) pan(dx, dy);
            else orbit(dx, dy);
            return true;
        }
        if (ev.type == ET::MouseUp) {
            dragging_ = false;
            if (app_ != nullptr) app_->release_pointer();
            const auto now = Clock::now();
            const bool click = drag_left_ && !drag_moved_ &&
                               (now - down_time_) <= kClickMaxHold &&
                               now >= suppress_until_;
            if (drag_moved_) suppress_until_ = now + kPostDragSuppress;
            if (click) {
                pick_at(mx - static_cast<double>(canvas_rect_.x),
                        my - static_cast<double>(canvas_rect_.y), ev.shift);
            }
            return true;
        }
        return false;
    }

    // Route by the hover chain, deepest first: the stats/corner text is
    // click-through, the scene layer (or bare canvas) claims the event, any
    // interactive overlay above it declines.
    const affineui::Document::HoverInfo* scene_entry = nullptr;
    const affineui::Document::HoverInfo* gizmo_entry = nullptr;
    bool over_scene = false;
    for (const auto& info : chain) {
        if (chain_entry_has(info, "dn-vp-stats") ||
            chain_entry_has(info, "dn-vp-corner")) {
            continue;
        }
        if (chain_entry_has(info, "dn-gizmo")) {
            gizmo_entry = &info;
            break;
        }
        if (chain_entry_has(info, "dn-vp-scenelayer") ||
            chain_entry_has(info, "dn-vp-canvas")) {
            scene_entry = &info;
            over_scene = true;
            break;
        }
        if (chain_entry_has(info, "dn-toolrail") ||
            chain_entry_has(info, "dn-navcluster") ||
            chain_entry_has(info, "dcs-btn") ||
            chain_entry_has(info, "dcs-panel") ||
            chain_entry_has(info, "dcs-toolbar")) {
            break;
        }
    }

    if (gizmo_entry != nullptr && ev.type == ET::MouseDown &&
        ev.button == affineui::MouseButton::Left) {
        // Click a nav-gizmo nub → snap the camera along that axis.
        const Rect gb = gizmo_entry->bounds;
        if (gb.w > 0 && gb.h > 0) {
            const double gx = (mx - gb.x) * 72.0 / gb.w;
            const double gy = (my - gb.y) * 72.0 / gb.h;
            for (const auto& nub : nubs_) {
                if (std::hypot(gx - nub.x, gy - nub.y) <= 10.0) {
                    snap_to_axis(nub.axis);
                    break;
                }
            }
        }
        return true;
    }

    if (!over_scene) return false;

    if (ev.type == ET::MouseWheel) {
        // Dolly toward the view direction, x1.1 per notch (zoomSpeed 0.9).
        dolly(std::pow(1.1, -static_cast<double>(ev.wheel_dy) * kZoomSpeed));
        return true;
    }

    if (ev.type == ET::MouseDown &&
        (ev.button == affineui::MouseButton::Left ||
         ev.button == affineui::MouseButton::Right)) {
        if (scene_entry != nullptr &&
            chain_entry_has(*scene_entry, "dn-vp-scenelayer")) {
            canvas_rect_ = scene_entry->bounds;  // freshest geometry
        }
        dragging_ = true;
        drag_left_ = ev.button == affineui::MouseButton::Left;
        // LMB orbits (or pans while Move View is toggled); RMB always pans.
        drag_pan_ = !drag_left_ || pan_mode_;
        drag_moved_ = false;
        down_x_ = last_x_ = mx;
        down_y_ = last_y_ = my;
        down_time_ = Clock::now();
        if (app_ != nullptr) app_->capture_pointer();
        return true;
    }

    return false;
}

}  // namespace dender
