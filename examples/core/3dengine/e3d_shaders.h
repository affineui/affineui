// e3d_shaders.h — hand-written per-backend shader sources (HLSL5 for
// D3D11, GLSL 330 for desktop GL, MSL for Metal), selected on the
// SOKOL_* backend macro like the in-tree NanoVG-on-sokol backend does.
// Three programs:
//
//   mesh  — lit/unlit triangle surfaces (position + normal), with one
//           directional PCF shadow. Material modes: 0 = standard
//           (roughness/metalness), 1 = basic (unlit), 2 = shadow
//           catcher (three.js ShadowMaterial).
//   line  — solid-color lines (position only).
//   depth — depth-only shadow-map pass.
//
// Uniform layouts are std140 and must match the C++ structs in
// e3d_renderer.cpp exactly:
//
//   VS ub0: mat4 u_mvp, u_model, u_nmat, u_light_vp
//   FS ub1: vec4 m_color (rgb, opacity)
//           vec4 m_params (roughness, metalness, mode, receive_shadow)
//   FS ub2: vec4 l_counts (n_dir, n_point, n_spot, shadow_on)
//           vec4 l_cam_pos
//           vec4 l_hemi_sky (rgb, intensity), l_hemi_ground, l_hemi_dir
//           vec4 l_shadow (texel_size, bias, 0, 0)
//           vec4 l_dir[8]   — 4 x { (to_light.xyz, 0), (color*I, 0) }
//           vec4 l_point[8] — 4 x { (pos.xyz, cutoff), (color*I, decay) }
//           vec4 l_spot[16] — 4 x { (pos, cutoff), (to_light_axis, cos_outer),
//                                   (color*I, decay), (cos_inner, 0, 0, 0) }
//
// Lighting follows three.js physical mode: Lambert diffuse (albedo/pi),
// inverse-square distance falloff with cutoff, smoothstep spot cones,
// and a normalized Blinn-Phong specular lobe standing in for GGX.
#pragma once

namespace e3d::shaders {

// ── D3D11 / HLSL ────────────────────────────────────────────────────

inline const char* kMeshVsHlsl = R"(
cbuffer vs_params : register(b0) {
    float4x4 u_mvp;
    float4x4 u_model;
    float4x4 u_nmat;
    float4x4 u_light_vp;
};
struct vs_in  { float3 pos : POSITION; float3 normal : NORMAL; };
struct vs_out {
    float4 pos    : SV_Position;
    float3 world  : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 shadow : TEXCOORD2;
};
vs_out main(vs_in inp) {
    vs_out o;
    float4 wp = mul(u_model, float4(inp.pos, 1.0));
    o.world  = wp.xyz;
    o.normal = mul((float3x3)u_nmat, inp.normal);
    o.shadow = mul(u_light_vp, wp);
    o.pos    = mul(u_mvp, float4(inp.pos, 1.0));
    return o;
}
)";

inline const char* kMeshFsHlsl = R"(
cbuffer material : register(b0) {
    float4 m_color;
    float4 m_params;
};
cbuffer lights : register(b1) {
    float4 l_counts;
    float4 l_cam_pos;
    float4 l_hemi_sky;
    float4 l_hemi_ground;
    float4 l_hemi_dir;
    float4 l_shadow;
    float4 l_dir[8];
    float4 l_point[16];
    float4 l_spot[16];
};
Texture2D              u_shadow_tex : register(t0);
SamplerComparisonState u_shadow_smp : register(s0);

struct fs_in {
    float4 pos    : SV_Position;
    float3 world  : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 shadow : TEXCOORD2;
};

static const float PI = 3.14159265;

float dist_atten(float dist, float cutoff, float decay) {
    float falloff = 1.0 / max(pow(dist, decay), 0.01);
    if (cutoff > 0.0) {
        float r = saturate(1.0 - pow(dist / cutoff, 4.0));
        falloff *= r * r;
    }
    return falloff;
}

float3 shade(float3 N, float3 V, float3 albedo, float rough, float metal,
             float3 L, float3 radiance) {
    float ndl = saturate(dot(N, L));
    if (ndl <= 0.0) return float3(0.0, 0.0, 0.0);
    float3 H = normalize(L + V);
    float a = max(rough * rough, 0.04);
    float shininess = clamp(2.0 / (a * a) - 2.0, 1.0, 2048.0);
    float3 spec_col = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float3 diffuse = albedo * (1.0 - metal) / PI;
    float3 spec = spec_col * pow(saturate(dot(N, H)), shininess) *
                  (shininess + 8.0) / (8.0 * PI);
    return (diffuse + spec) * radiance * ndl;
}

float shadow_factor(float4 sp) {
    float3 p = sp.xyz / sp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    float  ref = p.z - l_shadow.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        p.z > 1.0) return 1.0;
    float lit = 0.0;
    [unroll] for (int y = -1; y <= 1; y++) {
        [unroll] for (int x = -1; x <= 1; x++) {
            float2 off = float2(x, y) * l_shadow.x;
            lit += u_shadow_tex.SampleCmpLevelZero(u_shadow_smp, uv + off, ref);
        }
    }
    return lit / 9.0;
}

float4 main(fs_in inp) : SV_Target {
    float mode = m_params.z;
    float3 albedo = m_color.rgb;
    float alpha = m_color.a;

    float sh = 1.0;
    if (l_counts.w > 0.5 && m_params.w > 0.5) sh = shadow_factor(inp.shadow);

    if (mode > 1.5) {  // shadow catcher
        return float4(albedo, alpha * (1.0 - sh));
    }
    if (mode > 0.5) {  // unlit
        return float4(pow(abs(albedo), 1.0 / 2.2), alpha);
    }

    float3 N = normalize(inp.normal);
    float3 V = normalize(l_cam_pos.xyz - inp.world);
    if (dot(N, V) < 0.0) N = -N;

    float hemi_mix = dot(N, l_hemi_dir.xyz) * 0.5 + 0.5;
    float3 irradiance =
        lerp(l_hemi_ground.rgb, l_hemi_sky.rgb, hemi_mix) * l_hemi_sky.a;
    float3 col = irradiance * albedo * (1.0 - m_params.y) / PI;

    int n_dir = (int)l_counts.x;
    for (int i = 0; i < n_dir; i++) {
        float3 L = l_dir[i * 2].xyz;
        float3 radiance = l_dir[i * 2 + 1].rgb;
        if (i == 0) radiance *= sh;
        col += shade(N, V, albedo, m_params.x, m_params.y, L, radiance);
    }
    int n_point = (int)l_counts.y;
    for (int j = 0; j < n_point; j++) {
        float3 to_l = l_point[j * 2].xyz - inp.world;
        float dist = length(to_l);
        float3 radiance = l_point[j * 2 + 1].rgb *
            dist_atten(dist, l_point[j * 2].w, l_point[j * 2 + 1].w);
        col += shade(N, V, albedo, m_params.x, m_params.y, to_l / max(dist, 1e-4), radiance);
    }
    int n_spot = (int)l_counts.z;
    for (int k = 0; k < n_spot; k++) {
        float3 to_l = l_spot[k * 4].xyz - inp.world;
        float dist = length(to_l);
        float3 L = to_l / max(dist, 1e-4);
        float angle_cos = dot(L, l_spot[k * 4 + 1].xyz);
        float cone = smoothstep(l_spot[k * 4 + 1].w, l_spot[k * 4 + 3].x, angle_cos);
        float3 radiance = l_spot[k * 4 + 2].rgb * cone *
            dist_atten(dist, l_spot[k * 4].w, l_spot[k * 4 + 2].w);
        col += shade(N, V, albedo, m_params.x, m_params.y, L, radiance);
    }
    return float4(pow(abs(col), 1.0 / 2.2), alpha);
}
)";

inline const char* kLineVsHlsl = R"(
cbuffer vs_params : register(b0) { float4x4 u_mvp; };
float4 main(float3 pos : POSITION) : SV_Position {
    return mul(u_mvp, float4(pos, 1.0));
}
)";

inline const char* kLineFsHlsl = R"(
cbuffer material : register(b0) { float4 m_color; };
float4 main() : SV_Target {
    return float4(pow(abs(m_color.rgb), 1.0 / 2.2), m_color.a);
}
)";

inline const char* kDepthVsHlsl = R"(
cbuffer vs_params : register(b0) { float4x4 u_mvp; };
float4 main(float3 pos : POSITION) : SV_Position {
    return mul(u_mvp, float4(pos, 1.0));
}
)";

inline const char* kDepthFsHlsl = R"(
void main() {}
)";

// ── GL core / GLSL 330 ──────────────────────────────────────────────

inline const char* kMeshVsGlsl = R"(#version 330
uniform mat4 u_mvp;
uniform mat4 u_model;
uniform mat4 u_nmat;
uniform mat4 u_light_vp;
layout(location=0) in vec3 a_position;
layout(location=1) in vec3 a_normal;
out vec3 v_world;
out vec3 v_normal;
out vec4 v_shadow;
void main() {
    vec4 wp = u_model * vec4(a_position, 1.0);
    v_world = wp.xyz;
    v_normal = mat3(u_nmat) * a_normal;
    v_shadow = u_light_vp * wp;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
)";

inline const char* kMeshFsGlsl = R"(#version 330
uniform vec4 m_color;
uniform vec4 m_params;
uniform vec4 l_counts;
uniform vec4 l_cam_pos;
uniform vec4 l_hemi_sky;
uniform vec4 l_hemi_ground;
uniform vec4 l_hemi_dir;
uniform vec4 l_shadow;
uniform vec4 l_dir[8];
uniform vec4 l_point[16];
uniform vec4 l_spot[16];
uniform sampler2DShadow u_shadow_tex;

in vec3 v_world;
in vec3 v_normal;
in vec4 v_shadow;
out vec4 frag_color;

const float PI = 3.14159265;

float dist_atten(float dist, float cutoff, float decay) {
    float falloff = 1.0 / max(pow(dist, decay), 0.01);
    if (cutoff > 0.0) {
        float r = clamp(1.0 - pow(dist / cutoff, 4.0), 0.0, 1.0);
        falloff *= r * r;
    }
    return falloff;
}

vec3 shade(vec3 N, vec3 V, vec3 albedo, float rough, float metal,
           vec3 L, vec3 radiance) {
    float ndl = max(dot(N, L), 0.0);
    if (ndl <= 0.0) return vec3(0.0);
    vec3 H = normalize(L + V);
    float a = max(rough * rough, 0.04);
    float shininess = clamp(2.0 / (a * a) - 2.0, 1.0, 2048.0);
    vec3 spec_col = mix(vec3(0.04), albedo, metal);
    vec3 diffuse = albedo * (1.0 - metal) / PI;
    vec3 spec = spec_col * pow(max(dot(N, H), 0.0), shininess) *
                (shininess + 8.0) / (8.0 * PI);
    return (diffuse + spec) * radiance * ndl;
}

float shadow_factor(vec4 sp) {
    vec3 p = sp.xyz / sp.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    float ref = p.z * 0.5 + 0.5 - l_shadow.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        ref > 1.0) return 1.0;
    float lit = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 off = vec2(float(x), float(y)) * l_shadow.x;
            lit += texture(u_shadow_tex, vec3(uv + off, ref));
        }
    }
    return lit / 9.0;
}

void main() {
    float mode = m_params.z;
    vec3 albedo = m_color.rgb;
    float alpha = m_color.a;

    float sh = 1.0;
    if (l_counts.w > 0.5 && m_params.w > 0.5) sh = shadow_factor(v_shadow);

    if (mode > 1.5) {
        frag_color = vec4(albedo, alpha * (1.0 - sh));
        return;
    }
    if (mode > 0.5) {
        frag_color = vec4(pow(albedo, vec3(1.0 / 2.2)), alpha);
        return;
    }

    vec3 N = normalize(v_normal);
    vec3 V = normalize(l_cam_pos.xyz - v_world);
    if (dot(N, V) < 0.0) N = -N;

    float hemi_mix = dot(N, l_hemi_dir.xyz) * 0.5 + 0.5;
    vec3 irradiance =
        mix(l_hemi_ground.rgb, l_hemi_sky.rgb, hemi_mix) * l_hemi_sky.a;
    vec3 col = irradiance * albedo * (1.0 - m_params.y) / PI;

    int n_dir = int(l_counts.x);
    for (int i = 0; i < n_dir; i++) {
        vec3 radiance = l_dir[i * 2 + 1].rgb;
        if (i == 0) radiance *= sh;
        col += shade(N, V, albedo, m_params.x, m_params.y,
                     l_dir[i * 2].xyz, radiance);
    }
    int n_point = int(l_counts.y);
    for (int j = 0; j < n_point; j++) {
        vec3 to_l = l_point[j * 2].xyz - v_world;
        float dist = length(to_l);
        vec3 radiance = l_point[j * 2 + 1].rgb *
            dist_atten(dist, l_point[j * 2].w, l_point[j * 2 + 1].w);
        col += shade(N, V, albedo, m_params.x, m_params.y,
                     to_l / max(dist, 1e-4), radiance);
    }
    int n_spot = int(l_counts.z);
    for (int k = 0; k < n_spot; k++) {
        vec3 to_l = l_spot[k * 4].xyz - v_world;
        float dist = length(to_l);
        vec3 L = to_l / max(dist, 1e-4);
        float angle_cos = dot(L, l_spot[k * 4 + 1].xyz);
        float cone = smoothstep(l_spot[k * 4 + 1].w, l_spot[k * 4 + 3].x,
                                angle_cos);
        vec3 radiance = l_spot[k * 4 + 2].rgb * cone *
            dist_atten(dist, l_spot[k * 4].w, l_spot[k * 4 + 2].w);
        col += shade(N, V, albedo, m_params.x, m_params.y, L, radiance);
    }
    frag_color = vec4(pow(col, vec3(1.0 / 2.2)), alpha);
}
)";

inline const char* kLineVsGlsl = R"(#version 330
uniform mat4 u_mvp;
layout(location=0) in vec3 a_position;
void main() { gl_Position = u_mvp * vec4(a_position, 1.0); }
)";

inline const char* kLineFsGlsl = R"(#version 330
uniform vec4 m_color;
out vec4 frag_color;
void main() {
    frag_color = vec4(pow(m_color.rgb, vec3(1.0 / 2.2)), m_color.a);
}
)";

inline const char* kDepthVsGlsl = R"(#version 330
uniform mat4 u_mvp;
layout(location=0) in vec3 a_position;
void main() { gl_Position = u_mvp * vec4(a_position, 1.0); }
)";

inline const char* kDepthFsGlsl = R"(#version 330
void main() {}
)";

// ── Metal / MSL ─────────────────────────────────────────────────────

inline const char* kMeshVsMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct vs_params {
    float4x4 u_mvp;
    float4x4 u_model;
    float4x4 u_nmat;
    float4x4 u_light_vp;
};
struct vs_in {
    float3 pos    [[attribute(0)]];
    float3 normal [[attribute(1)]];
};
struct vs_out {
    float4 pos [[position]];
    float3 world;
    float3 normal;
    float4 shadow;
};
vertex vs_out vs_main(vs_in in [[stage_in]],
                      constant vs_params& u [[buffer(0)]]) {
    vs_out o;
    float4 wp = u.u_model * float4(in.pos, 1.0);
    o.world = wp.xyz;
    o.normal = float3x3(u.u_nmat[0].xyz, u.u_nmat[1].xyz, u.u_nmat[2].xyz) *
               in.normal;
    o.shadow = u.u_light_vp * wp;
    o.pos = u.u_mvp * float4(in.pos, 1.0);
    return o;
}
)";

inline const char* kMeshFsMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct material_ub {
    float4 m_color;
    float4 m_params;
};
struct lights_ub {
    float4 l_counts;
    float4 l_cam_pos;
    float4 l_hemi_sky;
    float4 l_hemi_ground;
    float4 l_hemi_dir;
    float4 l_shadow;
    float4 l_dir[8];
    float4 l_point[16];
    float4 l_spot[16];
};
struct fs_in {
    float4 pos [[position]];
    float3 world;
    float3 normal;
    float4 shadow;
};

constant float PI = 3.14159265;

static float dist_atten(float dist, float cutoff, float decay) {
    float falloff = 1.0 / max(pow(dist, decay), 0.01);
    if (cutoff > 0.0) {
        float r = saturate(1.0 - pow(dist / cutoff, 4.0));
        falloff *= r * r;
    }
    return falloff;
}

static float3 shade(float3 N, float3 V, float3 albedo, float rough,
                    float metal, float3 L, float3 radiance) {
    float ndl = saturate(dot(N, L));
    if (ndl <= 0.0) return float3(0.0);
    float3 H = normalize(L + V);
    float a = max(rough * rough, 0.04f);
    float shininess = clamp(2.0 / (a * a) - 2.0, 1.0, 2048.0);
    float3 spec_col = mix(float3(0.04), albedo, metal);
    float3 diffuse = albedo * (1.0 - metal) / PI;
    float3 spec = spec_col * pow(saturate(dot(N, H)), shininess) *
                  (shininess + 8.0) / (8.0 * PI);
    return (diffuse + spec) * radiance * ndl;
}

static float shadow_factor(float4 sp, constant lights_ub& l,
                           depth2d<float> tex, sampler smp) {
    float3 p = sp.xyz / sp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    float ref = p.z - l.l_shadow.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        p.z > 1.0) return 1.0;
    float lit = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            float2 off = float2(x, y) * l.l_shadow.x;
            lit += tex.sample_compare(smp, uv + off, ref);
        }
    }
    return lit / 9.0;
}

fragment float4 fs_main(fs_in in [[stage_in]],
                        constant material_ub& m [[buffer(0)]],
                        constant lights_ub& l [[buffer(1)]],
                        depth2d<float> u_shadow_tex [[texture(0)]],
                        sampler u_shadow_smp [[sampler(0)]]) {
    float mode = m.m_params.z;
    float3 albedo = m.m_color.rgb;
    float alpha = m.m_color.a;

    float sh = 1.0;
    if (l.l_counts.w > 0.5 && m.m_params.w > 0.5) {
        sh = shadow_factor(in.shadow, l, u_shadow_tex, u_shadow_smp);
    }

    if (mode > 1.5) {
        return float4(albedo, alpha * (1.0 - sh));
    }
    if (mode > 0.5) {
        return float4(pow(albedo, 1.0 / 2.2), alpha);
    }

    float3 N = normalize(in.normal);
    float3 V = normalize(l.l_cam_pos.xyz - in.world);
    if (dot(N, V) < 0.0) N = -N;

    float hemi_mix = dot(N, l.l_hemi_dir.xyz) * 0.5 + 0.5;
    float3 irradiance =
        mix(l.l_hemi_ground.rgb, l.l_hemi_sky.rgb, hemi_mix) *
        l.l_hemi_sky.a;
    float3 col = irradiance * albedo * (1.0 - m.m_params.y) / PI;

    int n_dir = int(l.l_counts.x);
    for (int i = 0; i < n_dir; i++) {
        float3 radiance = l.l_dir[i * 2 + 1].rgb;
        if (i == 0) radiance *= sh;
        col += shade(N, V, albedo, m.m_params.x, m.m_params.y,
                     l.l_dir[i * 2].xyz, radiance);
    }
    int n_point = int(l.l_counts.y);
    for (int j = 0; j < n_point; j++) {
        float3 to_l = l.l_point[j * 2].xyz - in.world;
        float dist = length(to_l);
        float3 radiance = l.l_point[j * 2 + 1].rgb *
            dist_atten(dist, l.l_point[j * 2].w, l.l_point[j * 2 + 1].w);
        col += shade(N, V, albedo, m.m_params.x, m.m_params.y,
                     to_l / max(dist, 1e-4f), radiance);
    }
    int n_spot = int(l.l_counts.z);
    for (int k = 0; k < n_spot; k++) {
        float3 to_l = l.l_spot[k * 4].xyz - in.world;
        float dist = length(to_l);
        float3 L = to_l / max(dist, 1e-4f);
        float angle_cos = dot(L, l.l_spot[k * 4 + 1].xyz);
        float cone = smoothstep(l.l_spot[k * 4 + 1].w,
                                l.l_spot[k * 4 + 3].x, angle_cos);
        float3 radiance = l.l_spot[k * 4 + 2].rgb * cone *
            dist_atten(dist, l.l_spot[k * 4].w, l.l_spot[k * 4 + 2].w);
        col += shade(N, V, albedo, m.m_params.x, m.m_params.y, L, radiance);
    }
    return float4(pow(col, 1.0 / 2.2), alpha);
}
)";

inline const char* kLineVsMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct vs_params { float4x4 u_mvp; };
struct vs_in { float3 pos [[attribute(0)]]; };
struct vs_out { float4 pos [[position]]; };
vertex vs_out vs_main(vs_in in [[stage_in]],
                      constant vs_params& u [[buffer(0)]]) {
    vs_out o;
    o.pos = u.u_mvp * float4(in.pos, 1.0);
    return o;
}
)";

inline const char* kLineFsMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct material_ub { float4 m_color; };
fragment float4 fs_main(constant material_ub& m [[buffer(0)]]) {
    return float4(pow(m.m_color.rgb, 1.0 / 2.2), m.m_color.a);
}
)";

inline const char* kDepthVsMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct vs_params { float4x4 u_mvp; };
struct vs_in { float3 pos [[attribute(0)]]; };
struct vs_out { float4 pos [[position]]; };
vertex vs_out vs_main(vs_in in [[stage_in]],
                      constant vs_params& u [[buffer(0)]]) {
    vs_out o;
    o.pos = u.u_mvp * float4(in.pos, 1.0);
    return o;
}
)";

inline const char* kDepthFsMsl = R"(
#include <metal_stdlib>
fragment void fs_main() {}
)";

}  // namespace e3d::shaders
