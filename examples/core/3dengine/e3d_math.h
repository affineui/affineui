// e3d_math.h — 3D math value types for the example 3D engine.
//
// A compact C++ port of the three.js (r170) math classes the engine
// needs: Vec2/Vec3, Quat, Euler, Mat4, Plane, Ray, Sphere, Box3,
// Spherical and Color. Semantics follow three.js — column-major
// matrices, right-handed coordinates, Y up — so code translated from
// the JS samples (and from three's own addons) reads one-to-one.
//
// Everything here is a plain value type; no allocation, no virtuals.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace e3d {

inline constexpr float kPi = 3.14159265358979323846f;

inline float deg_to_rad(float deg) { return deg * (kPi / 180.0f); }
inline float rad_to_deg(float rad) { return rad * (180.0f / kPi); }
inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ── Vec2 ────────────────────────────────────────────────────────────

struct Vec2 {
    float x{0.0f};
    float y{0.0f};

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    Vec2  operator+(const Vec2& v) const { return {x + v.x, y + v.y}; }
    Vec2  operator-(const Vec2& v) const { return {x - v.x, y - v.y}; }
    Vec2  operator*(float s) const { return {x * s, y * s}; }
    float length() const { return std::sqrt(x * x + y * y); }
};

// ── Vec3 ────────────────────────────────────────────────────────────

struct Quat;
struct Mat4;

struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3& set(float x_, float y_, float z_) {
        x = x_; y = y_; z = z_;
        return *this;
    }

    Vec3  operator-() const { return {-x, -y, -z}; }
    Vec3  operator+(const Vec3& v) const { return {x + v.x, y + v.y, z + v.z}; }
    Vec3  operator-(const Vec3& v) const { return {x - v.x, y - v.y, z - v.z}; }
    Vec3  operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3  operator*(const Vec3& v) const { return {x * v.x, y * v.y, z * v.z}; }
    Vec3  operator/(float s) const { return *this * (1.0f / s); }
    Vec3& operator+=(const Vec3& v) { x += v.x; y += v.y; z += v.z; return *this; }
    Vec3& operator-=(const Vec3& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    bool  operator==(const Vec3& v) const { return x == v.x && y == v.y && z == v.z; }

    float dot(const Vec3& v) const { return x * v.x + y * v.y + z * v.z; }
    Vec3  cross(const Vec3& v) const {
        return {y * v.z - z * v.y, z * v.x - x * v.z, x * v.y - y * v.x};
    }
    float length_sq() const { return dot(*this); }
    float length() const { return std::sqrt(length_sq()); }
    float distance_to(const Vec3& v) const { return (*this - v).length(); }
    float distance_to_sq(const Vec3& v) const { return (*this - v).length_sq(); }

    Vec3 normalized() const {
        const float len = length();
        return len > 0.0f ? *this / len : Vec3{};
    }
    Vec3& normalize() { return *this = normalized(); }

    Vec3& add_scaled(const Vec3& v, float s) {
        x += v.x * s; y += v.y * s; z += v.z * s;
        return *this;
    }
    Vec3 lerp(const Vec3& v, float t) const {
        return {x + (v.x - x) * t, y + (v.y - y) * t, z + (v.z - z) * t};
    }
    Vec3 min(const Vec3& v) const {
        return {std::min(x, v.x), std::min(y, v.y), std::min(z, v.z)};
    }
    Vec3 max(const Vec3& v) const {
        return {std::max(x, v.x), std::max(y, v.y), std::max(z, v.z)};
    }
    float angle_to(const Vec3& v) const {
        const float d = std::sqrt(length_sq() * v.length_sq());
        if (d == 0.0f) return kPi / 2.0f;
        return std::acos(clampf(dot(v) / d, -1.0f, 1.0f));
    }

    // Defined after Quat / Mat4.
    Vec3  applied(const Quat& q) const;
    Vec3& apply(const Quat& q) { return *this = applied(q); }
    Vec3  applied(const Mat4& m) const;        // full point transform (w divide)
    Vec3& apply(const Mat4& m) { return *this = applied(m); }
    Vec3  transformed_direction(const Mat4& m) const;  // rotation part only, normalized

    static Vec3 unit_x() { return {1.0f, 0.0f, 0.0f}; }
    static Vec3 unit_y() { return {0.0f, 1.0f, 0.0f}; }
    static Vec3 unit_z() { return {0.0f, 0.0f, 1.0f}; }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }

// ── Quat ────────────────────────────────────────────────────────────

struct Quat {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};

    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_)
        : x(x_), y(y_), z(z_), w(w_) {}

    Quat& identity() { return *this = Quat{}; }

    // Hamilton product, matches three.js multiplyQuaternions(a, b) = a ⨯ b.
    Quat operator*(const Quat& b) const {
        return {w * b.x + x * b.w + y * b.z - z * b.y,
                w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w,
                w * b.w - x * b.x - y * b.y - z * b.z};
    }
    Quat& premultiply(const Quat& a) { return *this = a * *this; }

    float length() const { return std::sqrt(x * x + y * y + z * z + w * w); }
    Quat& normalize() {
        const float len = length();
        if (len == 0.0f) return identity();
        const float inv = 1.0f / len;
        x *= inv; y *= inv; z *= inv; w *= inv;
        return *this;
    }
    // Inverse of a unit quaternion.
    Quat inverted() const { return {-x, -y, -z, w}; }

    Quat& set_from_axis_angle(const Vec3& axis, float angle) {
        // Axis is assumed normalized.
        const float half = angle / 2.0f;
        const float s = std::sin(half);
        x = axis.x * s; y = axis.y * s; z = axis.z * s;
        w = std::cos(half);
        return *this;
    }

    // Shortest arc rotating unit vector `from` onto unit vector `to`.
    // Port of three.js Quaternion.setFromUnitVectors.
    Quat& set_from_unit_vectors(const Vec3& from, const Vec3& to) {
        float r = from.dot(to) + 1.0f;
        if (r < std::numeric_limits<float>::epsilon()) {
            // Opposite directions: pick the most orthogonal axis.
            r = 0.0f;
            if (std::abs(from.x) > std::abs(from.z)) {
                x = -from.y; y = from.x; z = 0.0f; w = r;
            } else {
                x = 0.0f; y = -from.z; z = from.y; w = r;
            }
        } else {
            const Vec3 c = from.cross(to);
            x = c.x; y = c.y; z = c.z; w = r;
        }
        return normalize();
    }

    // Port of three.js Quaternion.setFromRotationMatrix (m: pure rotation).
    Quat& set_from_rotation_matrix(const Mat4& m);

    float angle_to(const Quat& q) const {
        const float d = clampf(x * q.x + y * q.y + z * q.z + w * q.w, -1.0f, 1.0f);
        return 2.0f * std::acos(std::abs(d));
    }
};

// ── Euler (XYZ order, three.js default) ─────────────────────────────

struct Euler {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    constexpr Euler() = default;
    constexpr Euler(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Euler& set(float x_, float y_, float z_) {
        x = x_; y = y_; z = z_;
        return *this;
    }

    // Port of Quaternion.setFromEuler / Euler.setFromQuaternion for the
    // XYZ order (the only order the engine supports).
    Quat to_quat() const {
        const float c1 = std::cos(x / 2.0f), s1 = std::sin(x / 2.0f);
        const float c2 = std::cos(y / 2.0f), s2 = std::sin(y / 2.0f);
        const float c3 = std::cos(z / 2.0f), s3 = std::sin(z / 2.0f);
        return {s1 * c2 * c3 + c1 * s2 * s3,
                c1 * s2 * c3 - s1 * c2 * s3,
                c1 * c2 * s3 + s1 * s2 * c3,
                c1 * c2 * c3 - s1 * s2 * s3};
    }
    static Euler from_quat(const Quat& q);  // defined after Mat4
};

// ── Mat4 (column-major, like three.js and GLSL) ─────────────────────

struct Mat4 {
    // e[column * 4 + row], identical layout to three.js `elements`.
    float e[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 identity() { return Mat4{}; }

    float& at(int row, int col) { return e[col * 4 + row]; }
    float  at(int row, int col) const { return e[col * 4 + row]; }

    // this ⨯ m (applies m first, then this), matching three.js multiply.
    Mat4 operator*(const Mat4& m) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            for (int rw = 0; rw < 4; ++rw) {
                r.e[c * 4 + rw] = e[0 * 4 + rw] * m.e[c * 4 + 0] +
                                  e[1 * 4 + rw] * m.e[c * 4 + 1] +
                                  e[2 * 4 + rw] * m.e[c * 4 + 2] +
                                  e[3 * 4 + rw] * m.e[c * 4 + 3];
            }
        }
        return r;
    }

    Vec3 origin() const { return {e[12], e[13], e[14]}; }
    Vec3 basis_x() const { return {e[0], e[1], e[2]}; }
    Vec3 basis_y() const { return {e[4], e[5], e[6]}; }
    Vec3 basis_z() const { return {e[8], e[9], e[10]}; }

    Vec3 scale_of() const {
        return {basis_x().length(), basis_y().length(), basis_z().length()};
    }
    float max_scale_on_axis() const {
        const Vec3 s{basis_x().length_sq(), basis_y().length_sq(),
                     basis_z().length_sq()};
        return std::sqrt(std::max(s.x, std::max(s.y, s.z)));
    }

    static Mat4 translation(const Vec3& t) {
        Mat4 m;
        m.e[12] = t.x; m.e[13] = t.y; m.e[14] = t.z;
        return m;
    }
    static Mat4 scaling(const Vec3& s) {
        Mat4 m;
        m.e[0] = s.x; m.e[5] = s.y; m.e[10] = s.z;
        return m;
    }
    static Mat4 rotation(const Quat& q) {
        return compose({0, 0, 0}, q, {1, 1, 1});
    }

    // Port of three.js Matrix4.compose(position, quaternion, scale).
    static Mat4 compose(const Vec3& p, const Quat& q, const Vec3& s) {
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        const float x2 = x + x, y2 = y + y, z2 = z + z;
        const float xx = x * x2, xy = x * y2, xz = x * z2;
        const float yy = y * y2, yz = y * z2, zz = z * z2;
        const float wx = w * x2, wy = w * y2, wz = w * z2;

        Mat4 m;
        m.e[0] = (1 - (yy + zz)) * s.x;
        m.e[1] = (xy + wz) * s.x;
        m.e[2] = (xz - wy) * s.x;
        m.e[3] = 0;
        m.e[4] = (xy - wz) * s.y;
        m.e[5] = (1 - (xx + zz)) * s.y;
        m.e[6] = (yz + wx) * s.y;
        m.e[7] = 0;
        m.e[8] = (xz + wy) * s.z;
        m.e[9] = (yz - wx) * s.z;
        m.e[10] = (1 - (xx + yy)) * s.z;
        m.e[11] = 0;
        m.e[12] = p.x; m.e[13] = p.y; m.e[14] = p.z; m.e[15] = 1;
        return m;
    }

    // Port of three.js Matrix4.decompose.
    void decompose(Vec3& p, Quat& q, Vec3& s) const {
        s = scale_of();
        // Negative determinant means one axis is mirrored.
        if (determinant() < 0.0f) s.x = -s.x;
        p = origin();

        Mat4 rot = *this;
        const float ix = 1.0f / s.x, iy = 1.0f / s.y, iz = 1.0f / s.z;
        rot.e[0] *= ix; rot.e[1] *= ix; rot.e[2] *= ix;
        rot.e[4] *= iy; rot.e[5] *= iy; rot.e[6] *= iy;
        rot.e[8] *= iz; rot.e[9] *= iz; rot.e[10] *= iz;
        rot.e[12] = rot.e[13] = rot.e[14] = 0.0f;
        q.set_from_rotation_matrix(rot);
    }

    float determinant() const {
        const float n11 = e[0], n21 = e[1], n31 = e[2], n41 = e[3];
        const float n12 = e[4], n22 = e[5], n32 = e[6], n42 = e[7];
        const float n13 = e[8], n23 = e[9], n33 = e[10], n43 = e[11];
        const float n14 = e[12], n24 = e[13], n34 = e[14], n44 = e[15];
        return n41 * (+n14 * n23 * n32 - n13 * n24 * n32 - n14 * n22 * n33 +
                      n12 * n24 * n33 + n13 * n22 * n34 - n12 * n23 * n34) +
               n42 * (+n11 * n23 * n34 - n11 * n24 * n33 + n14 * n21 * n33 -
                      n13 * n21 * n34 + n13 * n24 * n31 - n14 * n23 * n31) +
               n43 * (+n11 * n24 * n32 - n11 * n22 * n34 - n14 * n21 * n32 +
                      n12 * n21 * n34 + n14 * n22 * n31 - n12 * n24 * n31) +
               n44 * (-n13 * n22 * n31 - n11 * n23 * n32 + n11 * n22 * n33 +
                      n13 * n21 * n32 - n12 * n21 * n33 + n12 * n23 * n31);
    }

    // Port of three.js Matrix4.invert (general inverse; returns identity
    // for singular input, matching three's zero-matrix guard in spirit
    // while staying safe for downstream math).
    Mat4 inverted() const {
        const float n11 = e[0], n21 = e[1], n31 = e[2], n41 = e[3];
        const float n12 = e[4], n22 = e[5], n32 = e[6], n42 = e[7];
        const float n13 = e[8], n23 = e[9], n33 = e[10], n43 = e[11];
        const float n14 = e[12], n24 = e[13], n34 = e[14], n44 = e[15];

        const float t11 = n23 * n34 * n42 - n24 * n33 * n42 + n24 * n32 * n43 -
                          n22 * n34 * n43 - n23 * n32 * n44 + n22 * n33 * n44;
        const float t12 = n14 * n33 * n42 - n13 * n34 * n42 - n14 * n32 * n43 +
                          n12 * n34 * n43 + n13 * n32 * n44 - n12 * n33 * n44;
        const float t13 = n13 * n24 * n42 - n14 * n23 * n42 + n14 * n22 * n43 -
                          n12 * n24 * n43 - n13 * n22 * n44 + n12 * n23 * n44;
        const float t14 = n14 * n23 * n32 - n13 * n24 * n32 - n14 * n22 * n33 +
                          n12 * n24 * n33 + n13 * n22 * n34 - n12 * n23 * n34;

        const float det = n11 * t11 + n21 * t12 + n31 * t13 + n41 * t14;
        if (det == 0.0f) return Mat4{};
        const float d = 1.0f / det;

        Mat4 m;
        m.e[0] = t11 * d;
        m.e[1] = (n24 * n33 * n41 - n23 * n34 * n41 - n24 * n31 * n43 +
                  n21 * n34 * n43 + n23 * n31 * n44 - n21 * n33 * n44) * d;
        m.e[2] = (n22 * n34 * n41 - n24 * n32 * n41 + n24 * n31 * n42 -
                  n21 * n34 * n42 - n22 * n31 * n44 + n21 * n32 * n44) * d;
        m.e[3] = (n23 * n32 * n41 - n22 * n33 * n41 - n23 * n31 * n42 +
                  n21 * n33 * n42 + n22 * n31 * n43 - n21 * n32 * n43) * d;
        m.e[4] = t12 * d;
        m.e[5] = (n13 * n34 * n41 - n14 * n33 * n41 + n14 * n31 * n43 -
                  n11 * n34 * n43 - n13 * n31 * n44 + n11 * n33 * n44) * d;
        m.e[6] = (n14 * n32 * n41 - n12 * n34 * n41 - n14 * n31 * n42 +
                  n11 * n34 * n42 + n12 * n31 * n44 - n11 * n32 * n44) * d;
        m.e[7] = (n12 * n33 * n41 - n13 * n32 * n41 + n13 * n31 * n42 -
                  n11 * n33 * n42 - n12 * n31 * n43 + n11 * n32 * n43) * d;
        m.e[8] = t13 * d;
        m.e[9] = (n14 * n23 * n41 - n13 * n24 * n41 - n14 * n21 * n43 +
                  n11 * n24 * n43 + n13 * n21 * n44 - n11 * n23 * n44) * d;
        m.e[10] = (n12 * n24 * n41 - n14 * n22 * n41 + n14 * n21 * n42 -
                   n11 * n24 * n42 - n12 * n21 * n44 + n11 * n22 * n44) * d;
        m.e[11] = (n13 * n22 * n41 - n12 * n23 * n41 - n13 * n21 * n42 +
                   n11 * n23 * n42 + n12 * n21 * n43 - n11 * n22 * n43) * d;
        m.e[12] = t14 * d;
        m.e[13] = (n13 * n24 * n31 - n14 * n23 * n31 + n14 * n21 * n33 -
                   n11 * n24 * n33 - n13 * n21 * n34 + n11 * n23 * n34) * d;
        m.e[14] = (n14 * n22 * n31 - n12 * n24 * n31 - n14 * n21 * n32 +
                   n11 * n24 * n32 + n12 * n21 * n34 - n11 * n22 * n34) * d;
        m.e[15] = (n12 * n23 * n31 - n13 * n22 * n31 + n13 * n21 * n32 -
                   n11 * n23 * n32 - n12 * n21 * n33 + n11 * n22 * n33) * d;
        return m;
    }

    // Rotation-only view of the matrix suitable for transforming normals
    // under non-uniform scale: transpose(inverse(upper 3x3)), widened
    // back to a Mat4 with zero translation.
    Mat4 normal_matrix() const {
        Mat4 inv = inverted();
        Mat4 m;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                m.at(r, c) = inv.at(c, r);
            }
        }
        return m;
    }

    // Port of three.js Matrix4.lookAt — orients -Z at target, like a
    // camera (rotation part only; does not set translation).
    static Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 z = eye - target;
        if (z.length_sq() == 0.0f) z.z = 1.0f;
        z.normalize();
        Vec3 x = up.cross(z);
        if (x.length_sq() == 0.0f) {
            // up and z are parallel: nudge z like three.js does.
            std::abs(up.z) == 1.0f ? z.x += 0.0001f : z.z += 0.0001f;
            z.normalize();
            x = up.cross(z);
        }
        x.normalize();
        const Vec3 y = z.cross(x);
        Mat4 m;
        m.e[0] = x.x; m.e[1] = x.y; m.e[2] = x.z;
        m.e[4] = y.x; m.e[5] = y.y; m.e[6] = y.z;
        m.e[8] = z.x; m.e[9] = z.y; m.e[10] = z.z;
        return m;
    }

    // Right-handed perspective projection with a [-1, 1] clip-space Z
    // (three.js WebGL convention; the renderer remaps for the backend).
    static Mat4 perspective(float fov_y_rad, float aspect, float near,
                            float far) {
        const float top = near * std::tan(fov_y_rad / 2.0f);
        const float height = 2.0f * top;
        const float width = aspect * height;
        const float left = -width / 2.0f;
        const float right = left + width;
        const float bottom = top - height;

        Mat4 m;
        m.e[0] = 2.0f * near / (right - left);
        m.e[5] = 2.0f * near / (top - bottom);
        m.e[8] = (right + left) / (right - left);
        m.e[9] = (top + bottom) / (top - bottom);
        m.e[10] = -(far + near) / (far - near);
        m.e[11] = -1.0f;
        m.e[14] = -2.0f * far * near / (far - near);
        m.e[15] = 0.0f;
        return m;
    }

    static Mat4 orthographic(float left, float right, float top, float bottom,
                             float near, float far) {
        const float w = 1.0f / (right - left);
        const float h = 1.0f / (top - bottom);
        const float p = 1.0f / (far - near);
        Mat4 m;
        m.e[0] = 2.0f * w;
        m.e[5] = 2.0f * h;
        m.e[10] = -2.0f * p;
        m.e[12] = -(right + left) * w;
        m.e[13] = -(top + bottom) * h;
        m.e[14] = -(far + near) * p;
        return m;
    }
};

inline Quat& Quat::set_from_rotation_matrix(const Mat4& m) {
    const float m11 = m.e[0], m12 = m.e[4], m13 = m.e[8];
    const float m21 = m.e[1], m22 = m.e[5], m23 = m.e[9];
    const float m31 = m.e[2], m32 = m.e[6], m33 = m.e[10];
    const float trace = m11 + m22 + m33;

    if (trace > 0.0f) {
        const float s = 0.5f / std::sqrt(trace + 1.0f);
        w = 0.25f / s;
        x = (m32 - m23) * s;
        y = (m13 - m31) * s;
        z = (m21 - m12) * s;
    } else if (m11 > m22 && m11 > m33) {
        const float s = 2.0f * std::sqrt(1.0f + m11 - m22 - m33);
        w = (m32 - m23) / s;
        x = 0.25f * s;
        y = (m12 + m21) / s;
        z = (m13 + m31) / s;
    } else if (m22 > m33) {
        const float s = 2.0f * std::sqrt(1.0f + m22 - m11 - m33);
        w = (m13 - m31) / s;
        x = (m12 + m21) / s;
        y = 0.25f * s;
        z = (m23 + m32) / s;
    } else {
        const float s = 2.0f * std::sqrt(1.0f + m33 - m11 - m22);
        w = (m21 - m12) / s;
        x = (m13 + m31) / s;
        y = (m23 + m32) / s;
        z = 0.25f * s;
    }
    return *this;
}

inline Euler Euler::from_quat(const Quat& q) {
    // Euler.setFromRotationMatrix(XYZ) applied to the quat's matrix.
    const Mat4 m = Mat4::rotation(q);
    const float m11 = m.e[0], m12 = m.e[4], m13 = m.e[8];
    const float m22 = m.e[5], m23 = m.e[9];
    const float m32 = m.e[6], m33 = m.e[10];

    Euler out;
    out.y = std::asin(clampf(m13, -1.0f, 1.0f));
    if (std::abs(m13) < 0.9999999f) {
        out.x = std::atan2(-m23, m33);
        out.z = std::atan2(-m12, m11);
    } else {
        out.x = std::atan2(m32, m22);
        out.z = 0.0f;
    }
    return out;
}

inline Vec3 Vec3::applied(const Quat& q) const {
    // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
    const Vec3 qv{q.x, q.y, q.z};
    const Vec3 t = qv.cross(*this) * 2.0f;
    return *this + t * q.w + qv.cross(t);
}

inline Vec3 Vec3::applied(const Mat4& m) const {
    const float iw = 1.0f / (m.e[3] * x + m.e[7] * y + m.e[11] * z + m.e[15]);
    return {(m.e[0] * x + m.e[4] * y + m.e[8] * z + m.e[12]) * iw,
            (m.e[1] * x + m.e[5] * y + m.e[9] * z + m.e[13]) * iw,
            (m.e[2] * x + m.e[6] * y + m.e[10] * z + m.e[14]) * iw};
}

inline Vec3 Vec3::transformed_direction(const Mat4& m) const {
    return Vec3{m.e[0] * x + m.e[4] * y + m.e[8] * z,
                m.e[1] * x + m.e[5] * y + m.e[9] * z,
                m.e[2] * x + m.e[6] * y + m.e[10] * z}
        .normalized();
}

// ── Plane ───────────────────────────────────────────────────────────

struct Plane {
    Vec3  normal{1.0f, 0.0f, 0.0f};
    float constant{0.0f};

    Plane() = default;
    Plane(const Vec3& n, float c) : normal(n), constant(c) {}

    static Plane from_normal_and_point(const Vec3& n, const Vec3& point) {
        return {n, -point.dot(n)};
    }
    float distance_to(const Vec3& point) const {
        return normal.dot(point) + constant;
    }
};

// ── Sphere / Box3 ───────────────────────────────────────────────────

struct Sphere {
    Vec3  center;
    float radius{-1.0f};  // negative = empty, like three.js

    bool empty() const { return radius < 0.0f; }
    Sphere transformed(const Mat4& m) const {
        if (empty()) return *this;
        return {center.applied(m), radius * m.max_scale_on_axis()};
    }
};

struct Box3 {
    Vec3 min{+std::numeric_limits<float>::infinity(),
             +std::numeric_limits<float>::infinity(),
             +std::numeric_limits<float>::infinity()};
    Vec3 max{-std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity()};

    Box3() = default;
    Box3(const Vec3& mn, const Vec3& mx) : min(mn), max(mx) {}

    bool  empty() const { return max.x < min.x || max.y < min.y || max.z < min.z; }
    Box3& make_empty() { return *this = Box3{}; }
    Vec3  center() const { return empty() ? Vec3{} : (min + max) * 0.5f; }
    Vec3  size() const { return empty() ? Vec3{} : max - min; }

    Box3& expand_by_point(const Vec3& p) {
        min = min.min(p);
        max = max.max(p);
        return *this;
    }
    Box3& union_with(const Box3& b) {
        if (b.empty()) return *this;
        min = min.min(b.min);
        max = max.max(b.max);
        return *this;
    }
    // Transform all eight corners and refit (three.js Box3.applyMatrix4).
    Box3 transformed(const Mat4& m) const {
        if (empty()) return *this;
        Box3 out;
        for (int i = 0; i < 8; ++i) {
            const Vec3 corner{(i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y,
                              (i & 4) ? max.z : min.z};
            out.expand_by_point(corner.applied(m));
        }
        return out;
    }
    Sphere bounding_sphere() const {
        if (empty()) return {};
        const Vec3 c = center();
        return {c, size().length() * 0.5f};
    }
};

// ── Ray ─────────────────────────────────────────────────────────────

struct Ray {
    Vec3 origin;
    Vec3 direction{0.0f, 0.0f, -1.0f};  // unit length

    Vec3 at(float t) const { return origin + direction * t; }

    Ray transformed(const Mat4& m) const {
        Ray r;
        r.origin = origin.applied(m);
        r.direction = Vec3{m.e[0] * direction.x + m.e[4] * direction.y +
                               m.e[8] * direction.z,
                           m.e[1] * direction.x + m.e[5] * direction.y +
                               m.e[9] * direction.z,
                           m.e[2] * direction.x + m.e[6] * direction.y +
                               m.e[10] * direction.z}
                          .normalized();
        return r;
    }

    // Returns t ≥ 0 on hit, negative on miss (three.js returns null).
    float intersect_plane(const Plane& p) const {
        const float denom = p.normal.dot(direction);
        if (denom == 0.0f) {
            return p.distance_to(origin) == 0.0f ? 0.0f : -1.0f;
        }
        const float t = -(origin.dot(p.normal) + p.constant) / denom;
        return t >= 0.0f ? t : -1.0f;
    }

    bool intersects_sphere(const Sphere& s) const {
        if (s.empty()) return false;
        const Vec3  to_center = s.center - origin;
        const float tca = to_center.dot(direction);
        const float d2 = to_center.dot(to_center) - tca * tca;
        return d2 <= s.radius * s.radius &&
               (tca >= 0.0f || to_center.length_sq() <= s.radius * s.radius);
    }

    // Port of three.js Ray.intersectTriangle (Möller–Trumbore variant
    // used by three, with backface culling optional). Returns t ≥ 0 on
    // hit, negative on miss.
    float intersect_triangle(const Vec3& a, const Vec3& b, const Vec3& c,
                             bool backface_cull) const {
        const Vec3 edge1 = b - a;
        const Vec3 edge2 = c - a;
        const Vec3 normal = edge1.cross(edge2);

        float ddn = direction.dot(normal);
        float sign;
        if (ddn > 0.0f) {
            if (backface_cull) return -1.0f;
            sign = 1.0f;
        } else if (ddn < 0.0f) {
            sign = -1.0f;
            ddn = -ddn;
        } else {
            return -1.0f;
        }

        const Vec3  diff = origin - a;
        const float ddqxe2 = sign * direction.dot(diff.cross(edge2));
        if (ddqxe2 < 0.0f) return -1.0f;
        const float dde1xq = sign * direction.dot(edge1.cross(diff));
        if (dde1xq < 0.0f) return -1.0f;
        if (ddqxe2 + dde1xq > ddn) return -1.0f;

        const float qdn = -sign * diff.dot(normal);
        if (qdn < 0.0f) return -1.0f;
        return qdn / ddn;
    }

    // Port of three.js Ray.distanceSqToSegment. Returns the squared
    // distance from the ray to segment [v0, v1]; fills the closest
    // points on the ray and segment when requested.
    float distance_sq_to_segment(const Vec3& v0, const Vec3& v1,
                                 Vec3* on_ray = nullptr,
                                 Vec3* on_segment = nullptr) const {
        const Vec3  seg_center = (v0 + v1) * 0.5f;
        const Vec3  seg_dir = (v1 - v0).normalized();
        const float seg_extent = v0.distance_to(v1) * 0.5f;

        const Vec3  diff = origin - seg_center;
        const float a01 = -direction.dot(seg_dir);
        const float b0 = diff.dot(direction);
        const float b1 = -diff.dot(seg_dir);
        const float c = diff.length_sq();
        const float det = std::abs(1.0f - a01 * a01);
        float s0, s1, sq_dist;

        if (det > 0.0f) {
            s0 = a01 * b1 - b0;
            s1 = a01 * b0 - b1;
            const float ext_det = seg_extent * det;
            if (s0 >= 0.0f) {
                if (s1 >= -ext_det) {
                    if (s1 <= ext_det) {
                        const float inv_det = 1.0f / det;
                        s0 *= inv_det;
                        s1 *= inv_det;
                        sq_dist = s0 * (s0 + a01 * s1 + 2.0f * b0) +
                                  s1 * (a01 * s0 + s1 + 2.0f * b1) + c;
                    } else {
                        s1 = seg_extent;
                        s0 = std::max(0.0f, -(a01 * s1 + b0));
                        sq_dist = -s0 * s0 + s1 * (s1 + 2.0f * b1) + c;
                    }
                } else {
                    s1 = -seg_extent;
                    s0 = std::max(0.0f, -(a01 * s1 + b0));
                    sq_dist = -s0 * s0 + s1 * (s1 + 2.0f * b1) + c;
                }
            } else {
                if (s1 <= -ext_det) {
                    s0 = std::max(0.0f, -(-a01 * seg_extent + b0));
                    s1 = s0 > 0.0f ? -seg_extent
                                   : std::min(std::max(-seg_extent, -b1),
                                              seg_extent);
                    sq_dist = -s0 * s0 + s1 * (s1 + 2.0f * b1) + c;
                } else if (s1 <= ext_det) {
                    s0 = 0.0f;
                    s1 = std::min(std::max(-seg_extent, -b1), seg_extent);
                    sq_dist = s1 * (s1 + 2.0f * b1) + c;
                } else {
                    s0 = std::max(0.0f, -(a01 * seg_extent + b0));
                    s1 = s0 > 0.0f ? seg_extent
                                   : std::min(std::max(-seg_extent, -b1),
                                              seg_extent);
                    sq_dist = -s0 * s0 + s1 * (s1 + 2.0f * b1) + c;
                }
            }
        } else {
            // Parallel ray and segment.
            s1 = a01 > 0.0f ? -seg_extent : seg_extent;
            s0 = std::max(0.0f, -(a01 * s1 + b0));
            sq_dist = -s0 * s0 + s1 * (s1 + 2.0f * b1) + c;
        }

        if (on_ray) *on_ray = at(s0);
        if (on_segment) *on_segment = seg_center + seg_dir * s1;
        return sq_dist;
    }
};

// ── Spherical ───────────────────────────────────────────────────────

struct Spherical {
    float radius{1.0f};
    float phi{0.0f};    // polar angle from +Y
    float theta{0.0f};  // azimuth around Y

    Spherical& set_from_vec3(const Vec3& v) {
        radius = v.length();
        if (radius == 0.0f) {
            theta = phi = 0.0f;
        } else {
            theta = std::atan2(v.x, v.z);
            phi = std::acos(clampf(v.y / radius, -1.0f, 1.0f));
        }
        return *this;
    }
    Vec3 to_vec3() const {
        const float sin_phi_r = std::sin(phi) * radius;
        return {sin_phi_r * std::sin(theta), std::cos(phi) * radius,
                sin_phi_r * std::cos(theta)};
    }
    Spherical& make_safe() {
        constexpr float eps = 0.000001f;
        phi = clampf(phi, eps, kPi - eps);
        return *this;
    }
};

// ── Color (linear-space RGB floats) ─────────────────────────────────

struct Color {
    float r{1.0f};
    float g{1.0f};
    float b{1.0f};

    constexpr Color() = default;
    constexpr Color(float r_, float g_, float b_) : r(r_), g(g_), b(b_) {}
    // 0xRRGGBB, sRGB-encoded like every color literal in the JS samples;
    // converted to linear so lighting math happens in linear space.
    Color(std::uint32_t hex) {
        const auto srgb_to_linear = [](float c) {
            return c < 0.04045f ? c * (1.0f / 12.92f)
                                : std::pow((c + 0.055f) / 1.055f, 2.4f);
        };
        r = srgb_to_linear(static_cast<float>((hex >> 16) & 0xff) / 255.0f);
        g = srgb_to_linear(static_cast<float>((hex >> 8) & 0xff) / 255.0f);
        b = srgb_to_linear(static_cast<float>(hex & 0xff) / 255.0f);
    }

    Color operator*(float s) const { return {r * s, g * s, b * s}; }
    Color lerp(const Color& c, float t) const {
        return {r + (c.r - r) * t, g + (c.g - g) * t, b + (c.b - b) * t};
    }
};

}  // namespace e3d
