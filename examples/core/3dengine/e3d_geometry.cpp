// e3d_geometry.cpp — BufferGeometry bookkeeping and the primitive
// generators, ported from three.js r170 (src/geometries/*.js) without
// the UV channels.
#include "e3d_geometry.h"

#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace e3d {

namespace detail {
// Installed by the renderer so geometry destruction releases any GPU
// buffers uploaded for it. Null until a Renderer exists (pure-CPU use,
// e.g. unit tests, never touches the GPU).
GeometryReleaseFn geometry_release_hook = nullptr;
}  // namespace detail

BufferGeometry::~BufferGeometry() {
    if (detail::geometry_release_hook) detail::geometry_release_hook(this);
}

void BufferGeometry::apply_transform(const Mat4& m) {
    const Mat4 nmat = m.normal_matrix();
    for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
        const Vec3 p =
            Vec3{positions[i], positions[i + 1], positions[i + 2]}.applied(m);
        positions[i] = p.x;
        positions[i + 1] = p.y;
        positions[i + 2] = p.z;
    }
    for (std::size_t i = 0; i + 2 < normals.size(); i += 3) {
        const Vec3 n = Vec3{normals[i], normals[i + 1], normals[i + 2]}
                           .transformed_direction(nmat);
        normals[i] = n.x;
        normals[i + 1] = n.y;
        normals[i + 2] = n.z;
    }
    mark_changed();
}

void BufferGeometry::set_from_points(const Vec3* points, std::size_t count) {
    positions.resize(count * 3);
    for (std::size_t i = 0; i < count; ++i) {
        positions[i * 3] = points[i].x;
        positions[i * 3 + 1] = points[i].y;
        positions[i * 3 + 2] = points[i].z;
    }
    normals.clear();
    indices.clear();
    mark_changed();
}

void BufferGeometry::mark_changed() {
    bounds_valid_ = false;
    wireframe_valid_ = false;
    ++version_;
}

const Box3& BufferGeometry::bounding_box() const {
    if (!bounds_valid_) {
        bbox_.make_empty();
        for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
            bbox_.expand_by_point(
                {positions[i], positions[i + 1], positions[i + 2]});
        }
        // three.js computes the sphere from actual vertices; the
        // box-derived sphere is a slightly looser but valid bound and
        // is only used as a raycast pre-test.
        bsphere_ = bbox_.bounding_sphere();
        bounds_valid_ = true;
    }
    return bbox_;
}

const Sphere& BufferGeometry::bounding_sphere() const {
    bounding_box();
    return bsphere_;
}

const std::vector<std::uint32_t>& BufferGeometry::wireframe_indices() const {
    if (!wireframe_valid_) {
        wireframe_.clear();
        std::unordered_set<std::uint64_t> seen;
        const std::size_t tri_indices =
            indices.empty() ? vertex_count() : indices.size();
        for (std::size_t i = 0; i + 2 < tri_indices; i += 3) {
            for (int e = 0; e < 3; ++e) {
                std::uint32_t a = static_cast<std::uint32_t>(i + e);
                std::uint32_t b = static_cast<std::uint32_t>(i + (e + 1) % 3);
                if (!indices.empty()) {
                    a = indices[a];
                    b = indices[b];
                }
                const std::uint64_t key =
                    a < b ? (std::uint64_t(a) << 32 | b)
                          : (std::uint64_t(b) << 32 | a);
                if (seen.insert(key).second) {
                    wireframe_.push_back(a);
                    wireframe_.push_back(b);
                }
            }
        }
        wireframe_valid_ = true;
    }
    return wireframe_;
}

// ── Generator helpers ───────────────────────────────────────────────

namespace {

GeometryPtr new_geometry() { return std::make_shared<BufferGeometry>(); }

void push3(std::vector<float>& v, float x, float y, float z) {
    v.push_back(x);
    v.push_back(y);
    v.push_back(z);
}

// Flat normals for non-indexed triangle soup (three.js
// computeVertexNormals on non-indexed geometry).
void compute_flat_normals(BufferGeometry& g) {
    g.normals.assign(g.positions.size(), 0.0f);
    for (std::size_t i = 0; i + 8 < g.positions.size(); i += 9) {
        const Vec3 a{g.positions[i], g.positions[i + 1], g.positions[i + 2]};
        const Vec3 b{g.positions[i + 3], g.positions[i + 4],
                     g.positions[i + 5]};
        const Vec3 c{g.positions[i + 6], g.positions[i + 7],
                     g.positions[i + 8]};
        const Vec3 n = (c - b).cross(a - b).normalized();
        for (int v = 0; v < 3; ++v) {
            g.normals[i + v * 3] = n.x;
            g.normals[i + v * 3 + 1] = n.y;
            g.normals[i + v * 3 + 2] = n.z;
        }
    }
}

}  // namespace

// ── Box ─────────────────────────────────────────────────────────────

GeometryPtr make_box(float width, float height, float depth) {
    // Port of BoxGeometry with one segment per side: six faces, four
    // vertices each, per-face normals.
    auto g = new_geometry();
    const float hw = width / 2.0f, hh = height / 2.0f, hd = depth / 2.0f;

    struct Face {
        Vec3 normal, u_axis, v_axis;
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}},   // +X
        {{-1, 0, 0}, {0, 0, 1}, {0, -1, 0}},   // -X
        {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},     // +Y
        {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},   // -Y
        {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}},    // +Z
        {{0, 0, -1}, {-1, 0, 0}, {0, -1, 0}},  // -Z
    };
    const Vec3 half{hw, hh, hd};

    std::uint32_t base = 0;
    for (const Face& f : faces) {
        const Vec3 center = f.normal * half;
        const Vec3 u = f.u_axis * half;
        const Vec3 v = f.v_axis * half;
        const Vec3 corners[4] = {center - u - v, center + u - v,
                                 center - u + v, center + u + v};
        for (const Vec3& p : corners) {
            push3(g->positions, p.x, p.y, p.z);
            push3(g->normals, f.normal.x, f.normal.y, f.normal.z);
        }
        g->indices.insert(g->indices.end(),
                          {base, base + 2, base + 1, base + 2, base + 3,
                           base + 1});
        base += 4;
    }
    g->mark_changed();
    return g;
}

// ── Sphere ──────────────────────────────────────────────────────────

GeometryPtr make_sphere(float radius, int width_segments,
                        int height_segments) {
    auto g = new_geometry();
    width_segments = std::max(3, width_segments);
    height_segments = std::max(2, height_segments);

    std::vector<std::vector<std::uint32_t>> grid;
    std::uint32_t index = 0;
    for (int iy = 0; iy <= height_segments; ++iy) {
        std::vector<std::uint32_t> row;
        const float v = static_cast<float>(iy) / height_segments;
        // Pole offset trick from three.js is UV-only; skipped.
        for (int ix = 0; ix <= width_segments; ++ix) {
            const float u = static_cast<float>(ix) / width_segments;
            const float phi = u * 2.0f * kPi;
            const float theta = v * kPi;
            const Vec3 p{-radius * std::cos(phi) * std::sin(theta),
                         radius * std::cos(theta),
                         radius * std::sin(phi) * std::sin(theta)};
            push3(g->positions, p.x, p.y, p.z);
            const Vec3 n = p.normalized();
            push3(g->normals, n.x, n.y, n.z);
            row.push_back(index++);
        }
        grid.push_back(std::move(row));
    }

    for (int iy = 0; iy < height_segments; ++iy) {
        for (int ix = 0; ix < width_segments; ++ix) {
            const std::uint32_t a = grid[iy][ix + 1];
            const std::uint32_t b = grid[iy][ix];
            const std::uint32_t c = grid[iy + 1][ix];
            const std::uint32_t d = grid[iy + 1][ix + 1];
            if (iy != 0) g->indices.insert(g->indices.end(), {a, b, d});
            if (iy != height_segments - 1) {
                g->indices.insert(g->indices.end(), {b, c, d});
            }
        }
    }
    g->mark_changed();
    return g;
}

// ── Polyhedra (via PolyhedronGeometry subdivision) ──────────────────

namespace {

GeometryPtr make_polyhedron(const float* verts, const int* faces,
                            std::size_t face_index_count, float radius,
                            int detail) {
    detail = std::max(0, detail);  // negative would produce cols <= 0
    auto g = new_geometry();
    const auto vertex_at = [&](int i) {
        return Vec3{verts[i * 3], verts[i * 3 + 1], verts[i * 3 + 2]};
    };

    // PolyhedronGeometry.subdivideFace: split each face into a triangle
    // grid of (detail + 1)² sub-triangles, non-indexed.
    const int cols = detail + 1;
    for (std::size_t f = 0; f < face_index_count; f += 3) {
        const Vec3 a = vertex_at(faces[f]);
        const Vec3 b = vertex_at(faces[f + 1]);
        const Vec3 c = vertex_at(faces[f + 2]);

        std::vector<std::vector<Vec3>> v(cols + 1);
        for (int i = 0; i <= cols; ++i) {
            const Vec3 aj = a.lerp(c, static_cast<float>(i) / cols);
            const Vec3 bj = b.lerp(c, static_cast<float>(i) / cols);
            const int rows = cols - i;
            v[i].resize(rows + 1);
            for (int j = 0; j <= rows; ++j) {
                v[i][j] = (j == 0 && i == cols)
                              ? aj
                              : aj.lerp(bj, static_cast<float>(j) / rows);
            }
        }
        for (int i = 0; i < cols; ++i) {
            for (int j = 0; j < 2 * (cols - i) - 1; ++j) {
                const int k = j / 2;
                if (j % 2 == 0) {
                    push3(g->positions, v[i][k + 1].x, v[i][k + 1].y,
                          v[i][k + 1].z);
                    push3(g->positions, v[i + 1][k].x, v[i + 1][k].y,
                          v[i + 1][k].z);
                    push3(g->positions, v[i][k].x, v[i][k].y, v[i][k].z);
                } else {
                    push3(g->positions, v[i][k + 1].x, v[i][k + 1].y,
                          v[i][k + 1].z);
                    push3(g->positions, v[i + 1][k + 1].x, v[i + 1][k + 1].y,
                          v[i + 1][k + 1].z);
                    push3(g->positions, v[i + 1][k].x, v[i + 1][k].y,
                          v[i + 1][k].z);
                }
            }
        }
    }

    // Project onto the sphere.
    for (std::size_t i = 0; i + 2 < g->positions.size(); i += 3) {
        Vec3 p{g->positions[i], g->positions[i + 1], g->positions[i + 2]};
        p = p.normalized() * radius;
        g->positions[i] = p.x;
        g->positions[i + 1] = p.y;
        g->positions[i + 2] = p.z;
    }

    if (detail == 0) {
        compute_flat_normals(*g);
    } else {
        g->normals.resize(g->positions.size());
        for (std::size_t i = 0; i + 2 < g->positions.size(); i += 3) {
            const Vec3 n = Vec3{g->positions[i], g->positions[i + 1],
                                g->positions[i + 2]}
                               .normalized();
            g->normals[i] = n.x;
            g->normals[i + 1] = n.y;
            g->normals[i + 2] = n.z;
        }
    }
    g->mark_changed();
    return g;
}

}  // namespace

GeometryPtr make_icosahedron(float radius, int detail) {
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    const float verts[] = {-1, t,  0,  1, t, 0,  -1, -t, 0,  1, -t, 0,
                           0,  -1, t,  0, 1, t,  0,  -1, -t, 0, 1,  -t,
                           t,  0,  -1, t, 0, 1,  -t, 0,  -1, -t, 0, 1};
    const int faces[] = {0, 11, 5,  0, 5,  1,  0,  1,  7,  0,  7,  10,
                         0, 10, 11, 1, 5,  9,  5,  11, 4,  11, 10, 2,
                         10, 7, 6,  7, 1,  8,  3,  9,  4,  3,  4,  2,
                         3, 2,  6,  3, 6,  8,  3,  8,  9,  4,  9,  5,
                         2, 4,  11, 6, 2,  10, 8,  6,  7,  9,  8,  1};
    return make_polyhedron(verts, faces, std::size(faces), radius, detail);
}

GeometryPtr make_octahedron(float radius, int detail) {
    const float verts[] = {1, 0, 0,  -1, 0, 0, 0, 1, 0,
                           0, -1, 0, 0, 0, 1, 0, 0, -1};
    const int faces[] = {0, 2, 4, 0, 4, 3, 0, 3, 5, 0, 5, 2,
                         1, 2, 5, 1, 5, 3, 1, 3, 4, 1, 4, 2};
    return make_polyhedron(verts, faces, std::size(faces), radius, detail);
}

// ── Cylinder / Cone ─────────────────────────────────────────────────

GeometryPtr make_cylinder(float radius_top, float radius_bottom, float height,
                          int radial_segments, int height_segments,
                          bool open_ended, float theta_start,
                          float theta_length) {
    auto g = new_geometry();
    radial_segments = std::max(3, radial_segments);
    height_segments = std::max(1, height_segments);
    // A zero height would divide by zero in the torso slope below; a
    // degenerate cylinder isn't meaningful, so floor it to a tiny extent.
    if (std::abs(height) < 1e-6f) height = height < 0.0f ? -1e-6f : 1e-6f;
    const float half_height = height / 2.0f;
    std::uint32_t index = 0;

    // Torso.
    {
        std::vector<std::vector<std::uint32_t>> rows;
        const float slope = (radius_bottom - radius_top) / height;
        for (int y = 0; y <= height_segments; ++y) {
            std::vector<std::uint32_t> row;
            const float v = static_cast<float>(y) / height_segments;
            const float radius = v * (radius_bottom - radius_top) + radius_top;
            for (int x = 0; x <= radial_segments; ++x) {
                const float u = static_cast<float>(x) / radial_segments;
                const float theta = u * theta_length + theta_start;
                const float st = std::sin(theta), ct = std::cos(theta);
                push3(g->positions, radius * st, -v * height + half_height,
                      radius * ct);
                const Vec3 n = Vec3{st, slope, ct}.normalized();
                push3(g->normals, n.x, n.y, n.z);
                row.push_back(index++);
            }
            rows.push_back(std::move(row));
        }
        for (int x = 0; x < radial_segments; ++x) {
            for (int y = 0; y < height_segments; ++y) {
                const std::uint32_t a = rows[y][x];
                const std::uint32_t b = rows[y + 1][x];
                const std::uint32_t c = rows[y + 1][x + 1];
                const std::uint32_t d = rows[y][x + 1];
                if (radius_top > 0.0f || y != 0) {
                    g->indices.insert(g->indices.end(), {a, b, d});
                }
                if (radius_bottom > 0.0f || y != height_segments - 1) {
                    g->indices.insert(g->indices.end(), {b, c, d});
                }
            }
        }
    }

    // Caps.
    const auto generate_cap = [&](bool top) {
        const float radius = top ? radius_top : radius_bottom;
        const float sign = top ? 1.0f : -1.0f;
        const std::uint32_t center_start = index;
        // One center vertex per segment (mirrors three.js, which needs
        // it for UVs; harmless and keeps the port one-to-one).
        for (int x = 1; x <= radial_segments; ++x) {
            push3(g->positions, 0.0f, half_height * sign, 0.0f);
            push3(g->normals, 0.0f, sign, 0.0f);
            ++index;
        }
        const std::uint32_t center_end = index;
        for (int x = 0; x <= radial_segments; ++x) {
            const float u = static_cast<float>(x) / radial_segments;
            const float theta = u * theta_length + theta_start;
            push3(g->positions, radius * std::sin(theta), half_height * sign,
                  radius * std::cos(theta));
            push3(g->normals, 0.0f, sign, 0.0f);
            ++index;
        }
        for (int x = 0; x < radial_segments; ++x) {
            const std::uint32_t c = center_start + x;
            const std::uint32_t i = center_end + x;
            if (top) {
                g->indices.insert(g->indices.end(), {i, i + 1, c});
            } else {
                g->indices.insert(g->indices.end(), {i + 1, i, c});
            }
        }
    };
    if (!open_ended) {
        if (radius_top > 0.0f) generate_cap(true);
        if (radius_bottom > 0.0f) generate_cap(false);
    }
    g->mark_changed();
    return g;
}

GeometryPtr make_cone(float radius, float height, int radial_segments,
                      int height_segments, bool open_ended) {
    return make_cylinder(0.0f, radius, height, radial_segments,
                         height_segments, open_ended);
}

// ── Torus ───────────────────────────────────────────────────────────

GeometryPtr make_torus(float radius, float tube, int radial_segments,
                       int tubular_segments, float arc) {
    radial_segments = std::max(3, radial_segments);
    tubular_segments = std::max(3, tubular_segments);
    auto g = new_geometry();
    for (int j = 0; j <= radial_segments; ++j) {
        for (int i = 0; i <= tubular_segments; ++i) {
            const float u = static_cast<float>(i) / tubular_segments * arc;
            const float v =
                static_cast<float>(j) / radial_segments * 2.0f * kPi;
            const Vec3 p{(radius + tube * std::cos(v)) * std::cos(u),
                         (radius + tube * std::cos(v)) * std::sin(u),
                         tube * std::sin(v)};
            push3(g->positions, p.x, p.y, p.z);
            const Vec3 center{radius * std::cos(u), radius * std::sin(u),
                              0.0f};
            const Vec3 n = (p - center).normalized();
            push3(g->normals, n.x, n.y, n.z);
        }
    }
    for (int j = 1; j <= radial_segments; ++j) {
        for (int i = 1; i <= tubular_segments; ++i) {
            const std::uint32_t a = (tubular_segments + 1) * j + i - 1;
            const std::uint32_t b = (tubular_segments + 1) * (j - 1) + i - 1;
            const std::uint32_t c = (tubular_segments + 1) * (j - 1) + i;
            const std::uint32_t d = (tubular_segments + 1) * j + i;
            g->indices.insert(g->indices.end(), {a, b, d, b, c, d});
        }
    }
    g->mark_changed();
    return g;
}

// ── Plane ───────────────────────────────────────────────────────────

GeometryPtr make_plane(float width, float height) {
    auto g = new_geometry();
    const float hw = width / 2.0f, hh = height / 2.0f;
    const float xs[] = {-hw, hw};
    const float ys[] = {hh, -hh};
    for (float y : ys) {
        for (float x : xs) {
            push3(g->positions, x, y, 0.0f);
            push3(g->normals, 0.0f, 0.0f, 1.0f);
        }
    }
    g->indices = {0, 2, 1, 2, 3, 1};
    g->mark_changed();
    return g;
}

// ── Edges ───────────────────────────────────────────────────────────

namespace {

// Quantized vertex key (three.js rounds to 4 decimal places).
std::array<std::int64_t, 3> quantize(const Vec3& v) {
    constexpr float precision = 10000.0f;
    return {static_cast<std::int64_t>(std::lround(v.x * precision)),
            static_cast<std::int64_t>(std::lround(v.y * precision)),
            static_cast<std::int64_t>(std::lround(v.z * precision))};
}

struct EdgeKey {
    std::array<std::int64_t, 3> v0, v1;
    bool operator==(const EdgeKey&) const = default;
};

struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& k) const {
        std::size_t h = 1469598103934665603ull;
        const auto mix = [&h](std::int64_t v) {
            h ^= static_cast<std::size_t>(v);
            h *= 1099511628211ull;
        };
        for (auto v : k.v0) mix(v);
        for (auto v : k.v1) mix(v);
        return h;
    }
};

}  // namespace

GeometryPtr make_edges(const BufferGeometry& source,
                       float threshold_angle_deg) {
    auto g = new_geometry();
    const float threshold_dot = std::cos(deg_to_rad(threshold_angle_deg));

    struct EdgeEntry {
        Vec3 a, b;
        Vec3 normal;
        bool live{true};
    };
    std::unordered_map<EdgeKey, EdgeEntry, EdgeKeyHash> edges;

    const std::size_t tri_indices = source.indices.empty()
                                        ? source.vertex_count()
                                        : source.indices.size();
    for (std::size_t i = 0; i + 2 < tri_indices; i += 3) {
        Vec3 tri[3];
        std::array<std::int64_t, 3> keys[3];
        for (int v = 0; v < 3; ++v) {
            const std::size_t idx = source.indices.empty()
                                        ? i + v
                                        : source.indices[i + v];
            tri[v] = source.position_at(idx);
            keys[v] = quantize(tri[v]);
        }
        if (keys[0] == keys[1] || keys[1] == keys[2] || keys[2] == keys[0]) {
            continue;  // degenerate
        }
        const Vec3 normal = (tri[2] - tri[1]).cross(tri[0] - tri[1]).normalized();

        for (int e = 0; e < 3; ++e) {
            const int en = (e + 1) % 3;
            const EdgeKey forward{keys[e], keys[en]};
            const EdgeKey reverse{keys[en], keys[e]};
            if (auto it = edges.find(reverse);
                it != edges.end() && it->second.live) {
                // Sibling face found: keep the edge only if the crease
                // angle crosses the threshold.
                if (normal.dot(it->second.normal) <= threshold_dot) {
                    push3(g->positions, tri[e].x, tri[e].y, tri[e].z);
                    push3(g->positions, tri[en].x, tri[en].y, tri[en].z);
                }
                it->second.live = false;
            } else if (!edges.contains(forward)) {
                edges.emplace(forward, EdgeEntry{tri[e], tri[en], normal});
            }
        }
    }
    // Unmatched (boundary) edges are always kept.
    for (const auto& [key, entry] : edges) {
        if (entry.live) {
            push3(g->positions, entry.a.x, entry.a.y, entry.a.z);
            push3(g->positions, entry.b.x, entry.b.y, entry.b.z);
        }
    }
    g->mark_changed();
    return g;
}

GeometryPtr make_from_points(const Vec3* points, std::size_t count) {
    auto g = new_geometry();
    g->set_from_points(points, count);
    return g;
}

}  // namespace e3d
