// e3d_renderer.cpp — sokol_gfx forward renderer implementation.
//
// One offscreen MSAA color+depth target, resolved into a texture the
// Painter composites; one optional directional shadow-map pass. Vertex
// data uploads lazily per BufferGeometry (positions and normals as two
// separate buffers, matching the CPU layout) and re-uploads when the
// geometry's version counter changes.
#include "e3d_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "affineui/painter.h"
#include "e3d_shaders.h"
#include "sokol_gfx.h"

namespace e3d {

namespace {

// ── Uniform blocks (must match e3d_shaders.h, std140) ───────────────

struct VsParams {
    Mat4 mvp;
    Mat4 model;
    Mat4 nmat;
    Mat4 light_vp;
};
static_assert(sizeof(VsParams) == 256);

struct MaterialUB {
    float color[4];   // rgb (linear), opacity
    float params[4];  // roughness, metalness, mode, receive_shadow
};
static_assert(sizeof(MaterialUB) == 32);

constexpr int kMaxDir = 4;    // packed as 2 vec4 each
constexpr int kMaxPoint = 8;  // packed as 2 vec4 each
constexpr int kMaxSpot = 4;   // packed as 4 vec4 each

struct LightsUB {
    float counts[4];       // n_dir, n_point, n_spot, shadow_on
    float cam_pos[4];
    float hemi_sky[4];     // rgb, intensity
    float hemi_ground[4];
    float hemi_dir[4];
    float shadow[4];       // texel_size, bias, 0, 0
    float dir[kMaxDir * 2][4];
    float point[kMaxPoint * 2][4];
    float spot[kMaxSpot * 4][4];
};
static_assert(sizeof(LightsUB) == (6 + 8 + 16 + 16) * 16);

// AFFINEUI_E3D_TRACE: dump resource states + draw counts for the first
// frames (cheap frame-debugging without a GPU tool). "1"/"stderr" logs
// to stderr; any other value is treated as a file path — useful for
// WIN32-subsystem apps whose stderr goes nowhere.
std::FILE* trace_out() {
    static std::FILE* out = []() -> std::FILE* {
        const char* v = std::getenv("AFFINEUI_E3D_TRACE");
        if (v == nullptr || v[0] == '\0' || v[0] == '0') return nullptr;
        const std::string val(v);  // copy immediately (getenv convention)
        if (val == "1" || val == "stderr") return stderr;
        return std::fopen(val.c_str(), "w");
    }();
    return out;
}
bool trace_enabled() { return trace_out() != nullptr; }

// On D3D11/Metal clip-space Z is [0, 1]; our matrices are GL-style
// [-1, 1]. Remap by pre-multiplying row 2 with 0.5*z + 0.5*w.
Mat4 to_backend_clip(const Mat4& proj) {
#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    return proj;
#else
    Mat4 m = proj;
    for (int c = 0; c < 4; ++c) {
        m.e[c * 4 + 2] = 0.5f * proj.e[c * 4 + 2] + 0.5f * proj.e[c * 4 + 3];
    }
    return m;
#endif
}

float linear_to_srgb(float c) {
    return std::pow(std::max(c, 0.0f), 1.0f / 2.2f);
}

// ── Pipeline cache key ──────────────────────────────────────────────

enum class Program : std::uint8_t { Mesh, Line, Depth };

struct PipelineKey {
    Program           program;
    sg_primitive_type prim;
    bool              indexed;
    bool              depth_test;
    bool              depth_write;
    bool              blend;
    bool              double_sided;

    std::uint32_t packed() const {
        return static_cast<std::uint32_t>(program) |
               static_cast<std::uint32_t>(prim) << 2 |
               static_cast<std::uint32_t>(indexed) << 5 |
               static_cast<std::uint32_t>(depth_test) << 6 |
               static_cast<std::uint32_t>(depth_write) << 7 |
               static_cast<std::uint32_t>(blend) << 8 |
               static_cast<std::uint32_t>(double_sided) << 9;
    }
};

// ── Per-geometry GPU state ──────────────────────────────────────────

struct GpuGeometry {
    sg_buffer     pos{};
    sg_buffer     nrm{};
    sg_buffer     idx{};
    sg_buffer     wire_idx{};   // lazy: wireframe materials
    sg_buffer     loop_idx{};   // lazy: LineLoop closing strip
    std::uint64_t version{0};
    int           wire_count{0};
};

struct DrawItem {
    Object3D* object;
    BufferGeometry* geometry;
    Material* material;
    float     depth;  // view-space distance for sorting
};

}  // namespace

// ── Impl ────────────────────────────────────────────────────────────

struct Renderer::Impl {
    int  req_width{0};
    int  req_height{0};
    int  width{0};
    int  height{0};
    int  sample_count{4};
    bool targets_dirty{false};

    sg_image color_msaa{};
    sg_image color_resolve{};
    sg_image depth{};
    sg_view  color_att{};
    sg_view  resolve_att{};
    sg_view  depth_att{};

    sg_image   shadow_map{};
    int        shadow_size{0};
    sg_view    shadow_att{};
    sg_view    shadow_tex{};
    sg_sampler shadow_sampler{};

    sg_shader mesh_shader{};
    sg_shader line_shader{};
    sg_shader depth_shader{};
    std::unordered_map<std::uint32_t, sg_pipeline> pipelines;

    std::unordered_map<const BufferGeometry*, GpuGeometry> geometries;

    affineui::Painter* painter{nullptr};
    std::uint32_t      painter_handle{0};
    std::uint32_t      painter_image_id{0};

    // Per-frame scratch, reused across renders.
    std::vector<DrawItem> opaque;
    std::vector<DrawItem> transparent;
    std::vector<DrawItem> shadow_casters;
    LightsUB lights{};
    Mat4     light_vp;
    bool     shadow_on{false};

    void release_geometry(const BufferGeometry* g) {
        auto it = geometries.find(g);
        if (it == geometries.end()) return;
        destroy_geometry(it->second);
        geometries.erase(it);
    }
    static void destroy_geometry(GpuGeometry& gg) {
        if (gg.pos.id) sg_destroy_buffer(gg.pos);
        if (gg.nrm.id) sg_destroy_buffer(gg.nrm);
        if (gg.idx.id) sg_destroy_buffer(gg.idx);
        if (gg.wire_idx.id) sg_destroy_buffer(gg.wire_idx);
        if (gg.loop_idx.id) sg_destroy_buffer(gg.loop_idx);
        gg = {};
    }
};

namespace {

// The geometry destructor hook must reach every live renderer.
std::vector<Renderer::Impl*>& live_renderers() {
    static std::vector<Renderer::Impl*> v;
    return v;
}
void geometry_release_all(const BufferGeometry* g) {
    if (!sg_isvalid()) return;
    for (Renderer::Impl* impl : live_renderers()) impl->release_geometry(g);
}

// ── Shader construction ─────────────────────────────────────────────

void select_sources(sg_shader_desc& d, const char* hlsl_vs,
                    const char* hlsl_fs, const char* glsl_vs,
                    const char* glsl_fs, const char* msl_vs,
                    const char* msl_fs) {
#if defined(SOKOL_D3D11) || defined(SOKOL_DUMMY_BACKEND)
    d.vertex_func.source = hlsl_vs;
    d.vertex_func.d3d11_target = "vs_5_0";
    d.fragment_func.source = hlsl_fs;
    d.fragment_func.d3d11_target = "ps_5_0";
    (void)glsl_vs; (void)glsl_fs; (void)msl_vs; (void)msl_fs;
#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    d.vertex_func.source = glsl_vs;
    d.fragment_func.source = glsl_fs;
    (void)hlsl_vs; (void)hlsl_fs; (void)msl_vs; (void)msl_fs;
#elif defined(SOKOL_METAL)
    d.vertex_func.source = msl_vs;
    d.vertex_func.entry = "vs_main";
    d.fragment_func.source = msl_fs;
    d.fragment_func.entry = "fs_main";
    (void)hlsl_vs; (void)hlsl_fs; (void)glsl_vs; (void)glsl_fs;
#else
#    error "e3d: unsupported sokol backend (add a shader variant)"
#endif
}

sg_shader make_mesh_shader() {
    sg_shader_desc d{};
    d.label = "e3d_mesh";
    select_sources(d, shaders::kMeshVsHlsl, shaders::kMeshFsHlsl,
                   shaders::kMeshVsGlsl, shaders::kMeshFsGlsl,
                   shaders::kMeshVsMsl, shaders::kMeshFsMsl);

    d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.attrs[0].glsl_name = "a_position";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.attrs[1].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.attrs[1].glsl_name = "a_normal";
    d.attrs[1].hlsl_sem_name = "NORMAL";

    d.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    d.uniform_blocks[0].size = sizeof(VsParams);
    d.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[0].hlsl_register_b_n = 0;
    d.uniform_blocks[0].msl_buffer_n = 0;
    d.uniform_blocks[0].glsl_uniforms[0] = {SG_UNIFORMTYPE_MAT4, 1, "u_mvp"};
    d.uniform_blocks[0].glsl_uniforms[1] = {SG_UNIFORMTYPE_MAT4, 1, "u_model"};
    d.uniform_blocks[0].glsl_uniforms[2] = {SG_UNIFORMTYPE_MAT4, 1, "u_nmat"};
    d.uniform_blocks[0].glsl_uniforms[3] = {SG_UNIFORMTYPE_MAT4, 1,
                                            "u_light_vp"};

    d.uniform_blocks[1].stage = SG_SHADERSTAGE_FRAGMENT;
    d.uniform_blocks[1].size = sizeof(MaterialUB);
    d.uniform_blocks[1].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[1].hlsl_register_b_n = 0;
    d.uniform_blocks[1].msl_buffer_n = 0;
    d.uniform_blocks[1].glsl_uniforms[0] = {SG_UNIFORMTYPE_FLOAT4, 1,
                                            "m_color"};
    d.uniform_blocks[1].glsl_uniforms[1] = {SG_UNIFORMTYPE_FLOAT4, 1,
                                            "m_params"};

    d.uniform_blocks[2].stage = SG_SHADERSTAGE_FRAGMENT;
    d.uniform_blocks[2].size = sizeof(LightsUB);
    d.uniform_blocks[2].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[2].hlsl_register_b_n = 1;
    d.uniform_blocks[2].msl_buffer_n = 1;
    d.uniform_blocks[2].glsl_uniforms[0] = {SG_UNIFORMTYPE_FLOAT4, 1,
                                            "l_counts"};
    d.uniform_blocks[2].glsl_uniforms[1] = {SG_UNIFORMTYPE_FLOAT4, 1,
                                            "l_cam_pos"};
    d.uniform_blocks[2].glsl_uniforms[2] = {SG_UNIFORMTYPE_FLOAT4, 1,
                                            "l_hemi_sky"};
    d.uniform_blocks[2].glsl_uniforms[3] = {SG_UNIFORMTYPE_FLOAT4, 1,
                                            "l_hemi_ground"};
    d.uniform_blocks[2].glsl_uniforms[4] = {SG_UNIFORMTYPE_FLOAT4, 1,
                                            "l_hemi_dir"};
    d.uniform_blocks[2].glsl_uniforms[5] = {SG_UNIFORMTYPE_FLOAT4, 1,
                                            "l_shadow"};
    d.uniform_blocks[2].glsl_uniforms[6] = {SG_UNIFORMTYPE_FLOAT4,
                                            kMaxDir * 2, "l_dir"};
    d.uniform_blocks[2].glsl_uniforms[7] = {SG_UNIFORMTYPE_FLOAT4,
                                            kMaxPoint * 2, "l_point"};
    d.uniform_blocks[2].glsl_uniforms[8] = {SG_UNIFORMTYPE_FLOAT4,
                                            kMaxSpot * 4, "l_spot"};

    d.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d.views[0].texture.image_type = SG_IMAGETYPE_2D;
    d.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_DEPTH;
    d.views[0].texture.hlsl_register_t_n = 0;
    d.views[0].texture.msl_texture_n = 0;
    d.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type = SG_SAMPLERTYPE_COMPARISON;
    d.samplers[0].hlsl_register_s_n = 0;
    d.samplers[0].msl_sampler_n = 0;
    d.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[0].view_slot = 0;
    d.texture_sampler_pairs[0].sampler_slot = 0;
    d.texture_sampler_pairs[0].glsl_name = "u_shadow_tex";

    return sg_make_shader(&d);
}

sg_shader make_line_shader() {
    sg_shader_desc d{};
    d.label = "e3d_line";
    select_sources(d, shaders::kLineVsHlsl, shaders::kLineFsHlsl,
                   shaders::kLineVsGlsl, shaders::kLineFsGlsl,
                   shaders::kLineVsMsl, shaders::kLineFsMsl);
    d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.attrs[0].glsl_name = "a_position";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    d.uniform_blocks[0].size = sizeof(Mat4);
    d.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[0].hlsl_register_b_n = 0;
    d.uniform_blocks[0].msl_buffer_n = 0;
    d.uniform_blocks[0].glsl_uniforms[0] = {SG_UNIFORMTYPE_MAT4, 1, "u_mvp"};
    d.uniform_blocks[1].stage = SG_SHADERSTAGE_FRAGMENT;
    d.uniform_blocks[1].size = 16;
    d.uniform_blocks[1].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[1].hlsl_register_b_n = 0;
    d.uniform_blocks[1].msl_buffer_n = 0;
    d.uniform_blocks[1].glsl_uniforms[0] = {SG_UNIFORMTYPE_FLOAT4, 1,
                                            "m_color"};
    return sg_make_shader(&d);
}

sg_shader make_depth_shader() {
    sg_shader_desc d{};
    d.label = "e3d_depth";
    select_sources(d, shaders::kDepthVsHlsl, shaders::kDepthFsHlsl,
                   shaders::kDepthVsGlsl, shaders::kDepthFsGlsl,
                   shaders::kDepthVsMsl, shaders::kDepthFsMsl);
    d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.attrs[0].glsl_name = "a_position";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    d.uniform_blocks[0].size = sizeof(Mat4);
    d.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[0].hlsl_register_b_n = 0;
    d.uniform_blocks[0].msl_buffer_n = 0;
    d.uniform_blocks[0].glsl_uniforms[0] = {SG_UNIFORMTYPE_MAT4, 1, "u_mvp"};
    return sg_make_shader(&d);
}

sg_buffer make_vertex_buffer(const std::vector<float>& data) {
    sg_buffer_desc bd{};
    bd.usage.vertex_buffer = true;
    bd.data = {data.data(), data.size() * sizeof(float)};
    bd.label = "e3d_vbuf";
    return sg_make_buffer(&bd);
}

sg_buffer make_index_buffer(const std::vector<std::uint32_t>& data) {
    sg_buffer_desc bd{};
    bd.usage.index_buffer = true;
    bd.data = {data.data(), data.size() * sizeof(std::uint32_t)};
    bd.label = "e3d_ibuf";
    return sg_make_buffer(&bd);
}

}  // namespace

// ── Renderer ────────────────────────────────────────────────────────

Renderer::Renderer(int sample_count) : impl_(std::make_unique<Impl>()) {
    impl_->sample_count = std::max(1, sample_count);
    live_renderers().push_back(impl_.get());
    detail::geometry_release_hook = &geometry_release_all;
}

Renderer::~Renderer() {
    auto& live = live_renderers();
    live.erase(std::remove(live.begin(), live.end(), impl_.get()),
               live.end());
    if (live.empty()) detail::geometry_release_hook = nullptr;
    if (!sg_isvalid()) return;

    if (impl_->painter && impl_->painter_handle) {
        impl_->painter->release_native_image(impl_->painter_handle);
    }
    for (auto& [geo, gg] : impl_->geometries) Impl::destroy_geometry(gg);
    for (auto& [key, pip] : impl_->pipelines) sg_destroy_pipeline(pip);
    if (impl_->mesh_shader.id) sg_destroy_shader(impl_->mesh_shader);
    if (impl_->line_shader.id) sg_destroy_shader(impl_->line_shader);
    if (impl_->depth_shader.id) sg_destroy_shader(impl_->depth_shader);
    if (impl_->color_att.id) sg_destroy_view(impl_->color_att);
    if (impl_->resolve_att.id) sg_destroy_view(impl_->resolve_att);
    if (impl_->depth_att.id) sg_destroy_view(impl_->depth_att);
    if (impl_->color_msaa.id) sg_destroy_image(impl_->color_msaa);
    if (impl_->color_resolve.id) sg_destroy_image(impl_->color_resolve);
    if (impl_->depth.id) sg_destroy_image(impl_->depth);
    if (impl_->shadow_att.id) sg_destroy_view(impl_->shadow_att);
    if (impl_->shadow_tex.id) sg_destroy_view(impl_->shadow_tex);
    if (impl_->shadow_map.id) sg_destroy_image(impl_->shadow_map);
    if (impl_->shadow_sampler.id) sg_destroy_sampler(impl_->shadow_sampler);
}

void Renderer::set_size(int width, int height) {
    width = std::max(0, width);
    height = std::max(0, height);
    if (width == impl_->req_width && height == impl_->req_height) return;
    impl_->req_width = width;
    impl_->req_height = height;
    impl_->targets_dirty = true;
}

int Renderer::width() const { return impl_->width; }
int Renderer::height() const { return impl_->height; }

std::uint32_t Renderer::color_image_id() const {
    const sg_image img = impl_->sample_count > 1 ? impl_->color_resolve
                                                 : impl_->color_msaa;
    return img.id;
}

bool Renderer::flip_y() const {
    // GL render targets have row 0 at the bottom scanline.
    return !sg_query_features().origin_top_left;
}

std::uint32_t Renderer::painter_image(affineui::Painter& painter) {
    const std::uint32_t image = color_image_id();
    if (image == 0) return 0;
    if (impl_->painter == &painter && impl_->painter_image_id == image) {
        return impl_->painter_handle;
    }
    if (impl_->painter && impl_->painter_handle) {
        impl_->painter->release_native_image(impl_->painter_handle);
    }
    impl_->painter = &painter;
    impl_->painter_image_id = image;
    impl_->painter_handle = painter.adopt_native_image(
        image, impl_->width, impl_->height, flip_y());
    return impl_->painter_handle;
}

// ── Frame internals ─────────────────────────────────────────────────

namespace {

void ensure_targets(Renderer::Impl& im) {
    if (!im.targets_dirty && im.width > 0) return;
    if (im.req_width <= 0 || im.req_height <= 0) return;
    im.targets_dirty = false;
    im.width = im.req_width;
    im.height = im.req_height;

    // Release the painter's adopted-image wrapper BEFORE destroying the sokol
    // images it references (the wrapper is NVG_IMAGE_NODELETE — it points at
    // im.color_resolve/color_msaa). Destroying the image first would leave the
    // wrapper releasing a stale native handle.
    if (im.painter && im.painter_handle) {
        im.painter->release_native_image(im.painter_handle);
        im.painter_handle = 0;
        im.painter_image_id = 0;
        im.painter = nullptr;
    }

    if (im.color_att.id) sg_destroy_view(im.color_att);
    if (im.resolve_att.id) sg_destroy_view(im.resolve_att);
    if (im.depth_att.id) sg_destroy_view(im.depth_att);
    if (im.color_msaa.id) sg_destroy_image(im.color_msaa);
    if (im.color_resolve.id) sg_destroy_image(im.color_resolve);
    if (im.depth.id) sg_destroy_image(im.depth);

    sg_image_desc cd{};
    cd.usage.color_attachment = true;
    cd.width = im.width;
    cd.height = im.height;
    cd.pixel_format = SG_PIXELFORMAT_RGBA8;
    cd.sample_count = im.sample_count;
    cd.label = "e3d_color";
    im.color_msaa = sg_make_image(&cd);

    if (im.sample_count > 1) {
        sg_image_desc rd{};
        rd.usage.resolve_attachment = true;
        rd.width = im.width;
        rd.height = im.height;
        rd.pixel_format = SG_PIXELFORMAT_RGBA8;
        rd.sample_count = 1;
        rd.label = "e3d_resolve";
        im.color_resolve = sg_make_image(&rd);
    }

    sg_image_desc dd{};
    dd.usage.depth_stencil_attachment = true;
    dd.width = im.width;
    dd.height = im.height;
    dd.pixel_format = SG_PIXELFORMAT_DEPTH;
    dd.sample_count = im.sample_count;
    dd.label = "e3d_depth";
    im.depth = sg_make_image(&dd);

    sg_view_desc vd{};
    vd.color_attachment.image = im.color_msaa;
    im.color_att = sg_make_view(&vd);
    if (im.sample_count > 1) {
        sg_view_desc rv{};
        rv.resolve_attachment.image = im.color_resolve;
        im.resolve_att = sg_make_view(&rv);
    }
    sg_view_desc dv{};
    dv.depth_stencil_attachment.image = im.depth;
    im.depth_att = sg_make_view(&dv);
    // (The stale painter wrapper was released above, before the old images
    // it referenced were destroyed.)
}

void ensure_shadow_map(Renderer::Impl& im, int size) {
    if (im.shadow_size == size) return;
    if (im.shadow_att.id) sg_destroy_view(im.shadow_att);
    if (im.shadow_tex.id) sg_destroy_view(im.shadow_tex);
    if (im.shadow_map.id) sg_destroy_image(im.shadow_map);
    im.shadow_size = size;

    sg_image_desc sd{};
    sd.usage.depth_stencil_attachment = true;
    sd.width = size;
    sd.height = size;
    sd.pixel_format = SG_PIXELFORMAT_DEPTH;
    sd.sample_count = 1;
    sd.label = "e3d_shadow_map";
    im.shadow_map = sg_make_image(&sd);

    sg_view_desc av{};
    av.depth_stencil_attachment.image = im.shadow_map;
    im.shadow_att = sg_make_view(&av);
    sg_view_desc tv{};
    tv.texture.image = im.shadow_map;
    im.shadow_tex = sg_make_view(&tv);

    if (!im.shadow_sampler.id) {
        sg_sampler_desc smp{};
        smp.min_filter = SG_FILTER_LINEAR;
        smp.mag_filter = SG_FILTER_LINEAR;
        smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
        smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        smp.compare = SG_COMPAREFUNC_LESS_EQUAL;
        smp.label = "e3d_shadow_sampler";
        im.shadow_sampler = sg_make_sampler(&smp);
    }
}

GpuGeometry& ensure_geometry(Renderer::Impl& im, BufferGeometry& g) {
    GpuGeometry& gg = im.geometries[&g];
    if (gg.version == g.version()) return gg;
    Renderer::Impl::destroy_geometry(gg);
    gg.version = g.version();
    if (!g.positions.empty()) gg.pos = make_vertex_buffer(g.positions);
    if (!g.normals.empty()) gg.nrm = make_vertex_buffer(g.normals);
    if (!g.indices.empty()) gg.idx = make_index_buffer(g.indices);
    return gg;
}

sg_pipeline ensure_pipeline(Renderer::Impl& im, const PipelineKey& key) {
    auto it = im.pipelines.find(key.packed());
    if (it != im.pipelines.end()) return it->second;

    sg_pipeline_desc p{};
    p.primitive_type = key.prim;
    p.index_type = key.indexed ? SG_INDEXTYPE_UINT32 : SG_INDEXTYPE_NONE;
    p.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT3;
    p.layout.attrs[0].buffer_index = 0;

    switch (key.program) {
    case Program::Mesh:
        p.shader = im.mesh_shader;
        p.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT3;
        p.layout.attrs[1].buffer_index = 1;
        p.cull_mode = key.double_sided ? SG_CULLMODE_NONE : SG_CULLMODE_BACK;
        p.label = "e3d_mesh_pip";
        break;
    case Program::Line:
        p.shader = im.line_shader;
        p.cull_mode = SG_CULLMODE_NONE;
        p.label = "e3d_line_pip";
        break;
    case Program::Depth:
        p.shader = im.depth_shader;
        // Front-face culling reduces acne on closed shadow casters.
        p.cull_mode = SG_CULLMODE_FRONT;
        p.label = "e3d_depth_pip";
        break;
    }
    p.face_winding = SG_FACEWINDING_CCW;

    if (key.program == Program::Depth) {
        // Depth-only: sokol reads color_count==0 as "unset" and defaults it
        // back to 1 unless colors[0].pixel_format is explicitly NONE (a
        // zero-initialized format is _SG_PIXELFORMAT_DEFAULT, not NONE). Without
        // this the shadow pipeline claims a color attachment the shadow pass
        // doesn't have, and sg_apply_pipeline trips on it.
        p.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
        p.color_count = 0;
        p.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        p.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        p.depth.write_enabled = true;
        p.sample_count = 1;
    } else {
        p.color_count = 1;
        p.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        p.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        p.depth.compare = key.depth_test ? SG_COMPAREFUNC_LESS_EQUAL
                                         : SG_COMPAREFUNC_ALWAYS;
        p.depth.write_enabled = key.depth_write;
        p.sample_count = im.sample_count;
        if (key.blend) {
            auto& b = p.colors[0].blend;
            b.enabled = true;
            b.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
            b.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            // Keep the target's alpha channel opaque.
            b.src_factor_alpha = SG_BLENDFACTOR_ZERO;
            b.dst_factor_alpha = SG_BLENDFACTOR_ONE;
        }
    }

    const sg_pipeline pip = sg_make_pipeline(&p);
    im.pipelines.emplace(key.packed(), pip);
    return pip;
}

// Visit visible objects, collecting draw items and lights.
void collect(Renderer::Impl& im, Object3D& node, const Mat4& view) {
    if (!node.visible) return;

    const auto push_item = [&](BufferGeometry* g, Material* m) {
        if (!g || !m || g->positions.empty()) return;
        const Vec3 view_pos = node.matrix_world.origin().applied(view);
        DrawItem item{&node, g, m, -view_pos.z};
        (m->transparent ? im.transparent : im.opaque).push_back(item);
        if (node.kind() == ObjectKind::Mesh && node.cast_shadow) {
            im.shadow_casters.push_back(item);
        }
    };

    switch (node.kind()) {
    case ObjectKind::Mesh: {
        auto& mesh = static_cast<Mesh&>(node);
        push_item(mesh.geometry.get(), mesh.material.get());
        break;
    }
    case ObjectKind::Line:
    case ObjectKind::LineLoop:
    case ObjectKind::LineSegments: {
        auto& line = static_cast<Line&>(node);
        push_item(line.geometry.get(), line.material.get());
        break;
    }
    case ObjectKind::HemisphereLight: {
        auto& l = static_cast<HemisphereLight&>(node);
        const Vec3 dir = l.world_position().length_sq() > 0.0f
                             ? l.world_position().normalized()
                             : Vec3::unit_y();
        im.lights.hemi_sky[0] = l.color.r;
        im.lights.hemi_sky[1] = l.color.g;
        im.lights.hemi_sky[2] = l.color.b;
        im.lights.hemi_sky[3] = l.intensity;
        im.lights.hemi_ground[0] = l.ground_color.r;
        im.lights.hemi_ground[1] = l.ground_color.g;
        im.lights.hemi_ground[2] = l.ground_color.b;
        im.lights.hemi_dir[0] = dir.x;
        im.lights.hemi_dir[1] = dir.y;
        im.lights.hemi_dir[2] = dir.z;
        break;
    }
    case ObjectKind::DirectionalLight: {
        auto& l = static_cast<DirectionalLight&>(node);
        const int n = static_cast<int>(im.lights.counts[0]);
        if (n >= kMaxDir) break;
        // The shadow-casting light must sit in slot 0 (the shader
        // applies the shadow factor to that slot only). One shadow map
        // is supported; further cast_shadow lights render lit-only.
        const bool as_shadow = l.cast_shadow && !im.shadow_on;
        int slot = n;
        if (as_shadow && n > 0) {
            std::memmove(im.lights.dir[2], im.lights.dir[0],
                         sizeof(float) * 8 * static_cast<std::size_t>(n));
            slot = 0;
        } else if (as_shadow) {
            slot = 0;
        }
        const Vec3 to_light =
            (l.world_position() - l.target).normalized();
        float* d0 = im.lights.dir[slot * 2];
        float* d1 = im.lights.dir[slot * 2 + 1];
        d0[0] = to_light.x; d0[1] = to_light.y; d0[2] = to_light.z;
        d1[0] = l.color.r * l.intensity;
        d1[1] = l.color.g * l.intensity;
        d1[2] = l.color.b * l.intensity;
        im.lights.counts[0] += 1.0f;

        if (as_shadow) {
            im.shadow_on = true;
            const auto& sh = l.shadow;
            const Vec3 pos = l.world_position();
            const Mat4 view_l =
                (Mat4::translation(pos) * Mat4::look_at(pos, l.target,
                                                        Vec3::unit_y()))
                    .inverted();
            const Mat4 proj_l = Mat4::orthographic(
                -sh.extent, sh.extent, sh.extent, -sh.extent,
                sh.near_plane, sh.far_plane);
            im.light_vp = to_backend_clip(proj_l) * view_l;
            ensure_shadow_map(im, sh.map_size);
            im.lights.shadow[0] = 1.0f / static_cast<float>(sh.map_size);
            im.lights.shadow[1] = sh.bias;
        }
        break;
    }
    case ObjectKind::PointLight: {
        auto& l = static_cast<PointLight&>(node);
        const int n = static_cast<int>(im.lights.counts[1]);
        if (n >= kMaxPoint) break;
        const Vec3 pos = l.world_position();
        float* p0 = im.lights.point[n * 2];
        float* p1 = im.lights.point[n * 2 + 1];
        p0[0] = pos.x; p0[1] = pos.y; p0[2] = pos.z; p0[3] = l.distance;
        p1[0] = l.color.r * l.intensity;
        p1[1] = l.color.g * l.intensity;
        p1[2] = l.color.b * l.intensity;
        p1[3] = l.decay;
        im.lights.counts[1] += 1.0f;
        break;
    }
    case ObjectKind::SpotLight: {
        auto& l = static_cast<SpotLight&>(node);
        const int n = static_cast<int>(im.lights.counts[2]);
        if (n >= kMaxSpot) break;
        const Vec3 pos = l.world_position();
        const Vec3 axis = (pos - l.target).normalized();
        float* s0 = im.lights.spot[n * 4];
        float* s1 = im.lights.spot[n * 4 + 1];
        float* s2 = im.lights.spot[n * 4 + 2];
        float* s3 = im.lights.spot[n * 4 + 3];
        s0[0] = pos.x; s0[1] = pos.y; s0[2] = pos.z; s0[3] = l.distance;
        s1[0] = axis.x; s1[1] = axis.y; s1[2] = axis.z;
        s1[3] = std::cos(l.angle);
        s2[0] = l.color.r * l.intensity;
        s2[1] = l.color.g * l.intensity;
        s2[2] = l.color.b * l.intensity;
        s2[3] = l.decay;
        s3[0] = std::cos(l.angle * (1.0f - l.penumbra));
        im.lights.counts[2] += 1.0f;
        break;
    }
    default:
        break;
    }

    for (const auto& child : node.children()) collect(im, *child, view);
}

struct DrawPlan {
    sg_pipeline   pipeline;
    sg_buffer     pos, nrm, idx;
    int           count{0};
    bool          is_mesh{false};
};

// Resolve the primitive + buffers for one item (mesh, wireframe mesh,
// or one of the line kinds).
DrawPlan plan_draw(Renderer::Impl& im, const DrawItem& item) {
    BufferGeometry& g = *item.geometry;
    Material& m = *item.material;
    GpuGeometry& gg = ensure_geometry(im, g);

    DrawPlan plan{};
    plan.pos = gg.pos;

    PipelineKey key{};
    key.depth_test = m.depth_test;
    key.depth_write = m.depth_write;
    key.blend = m.transparent;
    key.double_sided = m.double_sided;

    if (item.object->kind() == ObjectKind::Mesh) {
        plan.is_mesh = true;
        plan.nrm = gg.nrm.id ? gg.nrm : gg.pos;
        key.program = Program::Mesh;
        if (m.wireframe) {
            if (!gg.wire_idx.id) {
                const auto& wire = g.wireframe_indices();
                if (wire.empty()) return plan;
                gg.wire_idx = make_index_buffer(wire);
                gg.wire_count = static_cast<int>(wire.size());
            }
            key.prim = SG_PRIMITIVETYPE_LINES;
            key.indexed = true;
            plan.idx = gg.wire_idx;
            plan.count = gg.wire_count;
        } else {
            key.prim = SG_PRIMITIVETYPE_TRIANGLES;
            key.indexed = gg.idx.id != 0;
            plan.idx = gg.idx;
            plan.count = key.indexed
                             ? static_cast<int>(g.indices.size())
                             : static_cast<int>(g.vertex_count());
        }
    } else {
        key.program = Program::Line;
        switch (item.object->kind()) {
        case ObjectKind::LineSegments:
            key.prim = SG_PRIMITIVETYPE_LINES;
            plan.count = static_cast<int>(g.vertex_count()) & ~1;
            break;
        case ObjectKind::LineLoop: {
            if (!gg.loop_idx.id) {
                std::vector<std::uint32_t> loop(g.vertex_count() + 1);
                for (std::size_t i = 0; i < g.vertex_count(); ++i) {
                    loop[i] = static_cast<std::uint32_t>(i);
                }
                loop.back() = 0;
                gg.loop_idx = make_index_buffer(loop);
            }
            key.prim = SG_PRIMITIVETYPE_LINE_STRIP;
            key.indexed = true;
            plan.idx = gg.loop_idx;
            plan.count = static_cast<int>(g.vertex_count()) + 1;
            break;
        }
        default:  // Line (open strip)
            key.prim = SG_PRIMITIVETYPE_LINE_STRIP;
            plan.count = static_cast<int>(g.vertex_count());
            break;
        }
    }
    if (plan.count <= 0 || !plan.pos.id) {
        plan.count = 0;
        return plan;
    }
    plan.pipeline = ensure_pipeline(im, key);
    return plan;
}

MaterialUB material_ub(const Material& m, const Object3D& obj) {
    MaterialUB ub{};
    ub.color[0] = m.color.r;
    ub.color[1] = m.color.g;
    ub.color[2] = m.color.b;
    ub.color[3] = m.opacity;
    ub.params[0] = m.roughness;
    ub.params[1] = m.metalness;
    ub.params[2] = m.kind == MaterialKind::Shadow ? 2.0f
                   : m.kind == MaterialKind::Basic ? 1.0f
                                                   : 0.0f;
    ub.params[3] = obj.receive_shadow ? 1.0f : 0.0f;
    return ub;
}

void draw_items(Renderer::Impl& im, std::vector<DrawItem>& items,
                const Mat4& view_proj) {
    for (const DrawItem& item : items) {
        const DrawPlan plan = plan_draw(im, item);
        if (plan.count <= 0) continue;

        sg_apply_pipeline(plan.pipeline);

        sg_bindings b{};
        b.vertex_buffers[0] = plan.pos;
        if (plan.is_mesh) {
            b.vertex_buffers[1] = plan.nrm;
            b.views[0] = im.shadow_tex;
            b.samplers[0] = im.shadow_sampler;
        }
        if (plan.idx.id) b.index_buffer = plan.idx;
        sg_apply_bindings(&b);

        const Mat4& model = item.object->matrix_world;
        if (plan.is_mesh) {
            VsParams vs{};
            vs.mvp = view_proj * model;
            vs.model = model;
            vs.nmat = model.normal_matrix();
            vs.light_vp = im.light_vp;
            sg_apply_uniforms(0, SG_RANGE_REF(vs));
            const MaterialUB mat = material_ub(*item.material, *item.object);
            sg_apply_uniforms(1, SG_RANGE_REF(mat));
            sg_apply_uniforms(2, SG_RANGE_REF(im.lights));
        } else {
            const Mat4 mvp = view_proj * model;
            sg_apply_uniforms(0, SG_RANGE_REF(mvp));
            const MaterialUB mat = material_ub(*item.material, *item.object);
            sg_apply_uniforms(1, sg_range{mat.color, 16});
        }
        sg_draw(0, plan.count, 1);
    }
}

}  // namespace

void Renderer::render(Scene& scene, PerspectiveCamera& camera) {
    Impl& im = *impl_;
    if (im.req_width <= 0 || im.req_height <= 0 || !sg_isvalid()) return;
    ensure_targets(im);

    if (!im.mesh_shader.id) {
        im.mesh_shader = make_mesh_shader();
        im.line_shader = make_line_shader();
        im.depth_shader = make_depth_shader();
        // The shader declares the shadow texture unconditionally; keep
        // a small placeholder map until a shadow light appears.
        ensure_shadow_map(im, 16);
        if (trace_enabled()) {
            std::fprintf(trace_out(),
                         "[e3d] shaders mesh=%d line=%d depth=%d "
                         "(2=VALID 3=FAILED)\n",
                         sg_query_shader_state(im.mesh_shader),
                         sg_query_shader_state(im.line_shader),
                         sg_query_shader_state(im.depth_shader));
            std::fflush(trace_out());
        }
    }

    const float aspect =
        static_cast<float>(im.width) / static_cast<float>(im.height);
    if (std::abs(camera.aspect - aspect) > 1e-5f) {
        camera.aspect = aspect;
        camera.update_projection_matrix();
    }

    scene.update_matrix_world();
    const Mat4 view = camera.matrix_world.inverted();
    const Mat4 view_proj =
        to_backend_clip(camera.projection_matrix) * view;

    // Collect lights + draw items.
    im.opaque.clear();
    im.transparent.clear();
    im.shadow_casters.clear();
    im.lights = {};
    im.shadow_on = false;
    im.light_vp = Mat4::identity();
    collect(im, scene, view);

    const Vec3 cam_pos = camera.matrix_world.origin();
    im.lights.cam_pos[0] = cam_pos.x;
    im.lights.cam_pos[1] = cam_pos.y;
    im.lights.cam_pos[2] = cam_pos.z;
    im.lights.counts[3] =
        im.shadow_on && !im.shadow_casters.empty() ? 1.0f : 0.0f;

    // three.js draw order: renderOrder first, then opaque front-to-back
    // and transparent back-to-front.
    std::sort(im.opaque.begin(), im.opaque.end(),
              [](const DrawItem& a, const DrawItem& b) {
                  if (a.object->render_order != b.object->render_order) {
                      return a.object->render_order < b.object->render_order;
                  }
                  return a.depth < b.depth;
              });
    std::sort(im.transparent.begin(), im.transparent.end(),
              [](const DrawItem& a, const DrawItem& b) {
                  if (a.object->render_order != b.object->render_order) {
                      return a.object->render_order < b.object->render_order;
                  }
                  return a.depth > b.depth;
              });

    // Shadow pass.
    if (im.lights.counts[3] > 0.5f) {
        sg_pass pass{};
        pass.attachments.depth_stencil = im.shadow_att;
        pass.action.depth.load_action = SG_LOADACTION_CLEAR;
        pass.action.depth.store_action = SG_STOREACTION_STORE;
        pass.action.depth.clear_value = 1.0f;
        pass.label = "e3d_shadow_pass";
        sg_begin_pass(&pass);

        PipelineKey key{};
        key.program = Program::Depth;
        key.prim = SG_PRIMITIVETYPE_TRIANGLES;
        for (const DrawItem& item : im.shadow_casters) {
            BufferGeometry& g = *item.geometry;
            GpuGeometry& gg = ensure_geometry(im, g);
            if (!gg.pos.id) continue;
            key.indexed = gg.idx.id != 0;
            sg_apply_pipeline(ensure_pipeline(im, key));
            sg_bindings b{};
            b.vertex_buffers[0] = gg.pos;
            if (key.indexed) b.index_buffer = gg.idx;
            sg_apply_bindings(&b);
            const Mat4 mvp = im.light_vp * item.object->matrix_world;
            sg_apply_uniforms(0, SG_RANGE_REF(mvp));
            sg_draw(0,
                    key.indexed ? static_cast<int>(g.indices.size())
                                : static_cast<int>(g.vertex_count()),
                    1);
        }
        sg_end_pass();
    }

    // Main pass.
    {
        sg_pass pass{};
        pass.attachments.colors[0] = im.color_att;
        if (im.sample_count > 1) {
            pass.attachments.resolves[0] = im.resolve_att;
        }
        pass.attachments.depth_stencil = im.depth_att;
        pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        pass.action.colors[0].store_action = im.sample_count > 1
                                                 ? SG_STOREACTION_DONTCARE
                                                 : SG_STOREACTION_STORE;
        // The shader writes sRGB-encoded color; encode the clear too.
        pass.action.colors[0].clear_value = {
            linear_to_srgb(scene.background.r),
            linear_to_srgb(scene.background.g),
            linear_to_srgb(scene.background.b), scene.background_alpha};
        pass.action.depth.load_action = SG_LOADACTION_CLEAR;
        pass.action.depth.clear_value = 1.0f;
        pass.label = "e3d_main_pass";
        sg_begin_pass(&pass);
        draw_items(im, im.opaque, view_proj);
        draw_items(im, im.transparent, view_proj);
        sg_end_pass();
    }

    if (trace_enabled()) {
        static int frames = 0;
        if (frames < 3) {
            ++frames;
            std::fprintf(
                trace_out(),
                "[e3d] frame %d: %dx%d opaque=%zu transparent=%zu "
                "casters=%zu lights(d/p/s)=%g/%g/%g shadow=%g cam=(%g,%g,%g) "
                "pipes=%zu color_img=%u\n",
                frames, im.width, im.height, im.opaque.size(),
                im.transparent.size(), im.shadow_casters.size(),
                im.lights.counts[0], im.lights.counts[1],
                im.lights.counts[2], im.lights.counts[3], cam_pos.x,
                cam_pos.y, cam_pos.z, im.pipelines.size(),
                color_image_id());
            for (const auto& [key, pip] : im.pipelines) {
                std::fprintf(trace_out(),
                             "[e3d]   pipeline key=%x state=%d\n", key,
                             sg_query_pipeline_state(pip));
            }
            std::fflush(trace_out());
        }
    }
}

}  // namespace e3d
