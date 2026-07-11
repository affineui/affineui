// e3d_material.h — the four material models the samples use, folded
// into one value struct (three.js MeshStandardMaterial /
// MeshBasicMaterial / LineBasicMaterial / ShadowMaterial).
#pragma once

#include <memory>

#include "e3d_math.h"

namespace e3d {

enum class MaterialKind {
    Standard,  // lit surface: color, roughness, metalness
    Basic,     // unlit surface: flat color
    Line,      // unlit line color
    Shadow,    // invisible surface that only shows received shadow
};

struct Material {
    MaterialKind kind{MaterialKind::Standard};
    Color color{1.0f, 1.0f, 1.0f};
    float roughness{1.0f};
    float metalness{0.0f};
    float opacity{1.0f};
    bool  transparent{false};
    bool  wireframe{false};    // draw triangle meshes as line edges
    bool  depth_test{true};
    bool  depth_write{true};
    bool  double_sided{false};

    static std::shared_ptr<Material> standard(std::uint32_t hex,
                                              float roughness = 1.0f,
                                              float metalness = 0.0f) {
        auto m = std::make_shared<Material>();
        m->kind = MaterialKind::Standard;
        m->color = Color(hex);
        m->roughness = roughness;
        m->metalness = metalness;
        return m;
    }
    static std::shared_ptr<Material> basic(std::uint32_t hex) {
        auto m = std::make_shared<Material>();
        m->kind = MaterialKind::Basic;
        m->color = Color(hex);
        return m;
    }
    static std::shared_ptr<Material> line(std::uint32_t hex,
                                          float opacity = 1.0f) {
        auto m = std::make_shared<Material>();
        m->kind = MaterialKind::Line;
        m->color = Color(hex);
        m->opacity = opacity;
        m->transparent = opacity < 1.0f;
        return m;
    }
    /// three.js ShadowMaterial: renders nothing but the shadow falling
    /// on it, at the given darkness.
    static std::shared_ptr<Material> shadow(float opacity = 0.32f) {
        auto m = std::make_shared<Material>();
        m->kind = MaterialKind::Shadow;
        m->color = Color{0.0f, 0.0f, 0.0f};
        m->opacity = opacity;
        m->transparent = true;
        m->depth_write = false;
        return m;
    }
};

using MaterialPtr = std::shared_ptr<Material>;

}  // namespace e3d
