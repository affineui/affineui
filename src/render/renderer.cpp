// affineui::Renderer — graphics side of the engine, now driving NanoVG
// through sokol_gfx (see affineui_nanovg/src/nanovg_sokol.h). No raw GL.
//
// Two render entry points exist:
//   * render(): low-level compatibility path for callers that already opened
//     a sokol_gfx pass; it replays the cached display list into that pass.
//   * render_to(): owns the target pass, so it can rasterize the document into
//     a retained root texture and composite that texture into the swapchain.
// The sokol/app adapters use render_to() so static pages and unchanged
// display lists avoid document repaint and root-layer rasterization.

#include "affineui/renderer.h"

#include "affineui/document.h"
#include "affineui/painter.h"

#include <array>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "internal/embed_log.h"

#if !defined(AFFINEUI_STUB_BUILD)
#    include <algorithm>
#    include "internal/display_list_painter.h"
#    include "internal/paint_internal.h"
#    include "sokol_gfx.h"
#    include "sokol_log.h"
#    include "nanovg.h"
#    include "nanovg_sokol.h"
#endif

namespace affineui {

namespace detail {

struct RendererImpl {
    Color clear_color{30, 30, 46, 255};
    bool  ready{false};
    RenderStats stats{};

#if !defined(AFFINEUI_STUB_BUILD)
    NVGcontext*              vg{nullptr};
    std::unique_ptr<Painter> painter;
    int                      last_w{-1};
    int                      last_h{-1};
    float                    last_dpi{1.0f};
    bool                     first_frame{true};
    bool                     owns_sg{false};  // we called sg_setup (embedded)
    sg_pixel_format          sw_color{SG_PIXELFORMAT_BGRA8};         // embedded swapchain formats
    sg_pixel_format          sw_depth{SG_PIXELFORMAT_DEPTH_STENCIL};
    Allocator                alloc{};         // copied host allocator (embedded)
    bool                     has_alloc{false};
    DisplayList              cached_display_list;
    bool                     cached_display_list_valid{false};
    bool                     last_frame_had_active_animations{false};
    // Correctness gate for retained-surface partial raster. The machinery is
    // kept in-tree, but live rendering must not clear/replay sub-rects until
    // dirty coverage and clipped replay are proven exact. Otherwise text and
    // controls can accumulate or punch holes in the cached root texture.
    bool                     partial_root_raster_enabled{false};
    struct RootLayer {
        int      w{0};
        int      h{0};
        int      content_w{0};
        int      content_h{0};
        sg_image color_img{};
        sg_image depth_img{};
        sg_view  color_tex{};
        sg_view  color_att{};
        sg_view  depth_att{};
        int      nvg_image{0};
        bool     valid{false};
    } root_layer;
    struct RootComposite {
        sg_shader       shader{};
        sg_pipeline     pipeline{};
        sg_buffer       vertex_buffer{};
        sg_sampler      sampler{};
        sg_pixel_format color_format{SG_PIXELFORMAT_NONE};
        sg_pixel_format depth_format{SG_PIXELFORMAT_NONE};
        int             sample_count{0};
        float           u_max{-1.0f};
        float           v_max{-1.0f};
        bool            valid{false};
    } root_composite;
#endif
};

}  // namespace detail

Renderer::Renderer() : impl_(std::make_unique<detail::RendererImpl>()) {}
Renderer::~Renderer() { shutdown(); }
Renderer::Renderer(Renderer&&) noexcept            = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

bool Renderer::ready() const noexcept { return impl_->ready; }

void Renderer::set_clear_color(Color c) { impl_->clear_color = c; }
Color Renderer::clear_color() const     { return impl_->clear_color; }

#if defined(AFFINEUI_STUB_BUILD)

void Renderer::init_gl() {}
void Renderer::init_embedded(const GpuContext&, const Allocator*) {}
void Renderer::shutdown() { impl_->ready = false; }
void Renderer::render(Document&, int, int, float) {}
void Renderer::draw_debug_overlay(std::string_view,
                                  std::span<const float>,
                                  int,
                                  int,
                                  float,
                                  DebugOverlayCorner) {}
void Renderer::render_to(Document&, const FrameTarget&) {}
const RenderStats& Renderer::stats() const noexcept { return impl_->stats; }

#else  // real sokol_gfx path

void Renderer::init_gl() {
    if (impl_->ready) return;

    // Requires sg_setup() to have run and a sokol_gfx render pass to be
    // available; the caller (App::cb_init) guarantees this.
    impl_->vg = nvgCreateSokol(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!impl_->vg) {
        detail::log_msg(LogLevel::error, "[affineui] nvgCreateSokol failed");
        return;
    }
    // Best-effort default sans-serif. If none of the platform paths
    // exist, text simply won't render — by design we never crash.
    detail::register_default_font(impl_->vg);
    impl_->painter = detail::make_nanovg_painter(impl_->vg);
    impl_->ready = true;
}

namespace {

// Neutral PixelFormat → sokol_gfx. NanoVG's fill/stroke uses the stencil
// buffer, so a depth target without stencil renders incorrectly; the
// default depth maps to DEPTH_STENCIL deliberately.
sg_pixel_format to_sg_format(PixelFormat f, bool is_depth) {
    switch (f) {
        case PixelFormat::rgba8:         return SG_PIXELFORMAT_RGBA8;
        case PixelFormat::bgra8:         return SG_PIXELFORMAT_BGRA8;
        case PixelFormat::depth:         return SG_PIXELFORMAT_DEPTH;
        case PixelFormat::depth_stencil: return SG_PIXELFORMAT_DEPTH_STENCIL;
        case PixelFormat::default_:
        default:
            return is_depth ? SG_PIXELFORMAT_DEPTH_STENCIL : SG_PIXELFORMAT_BGRA8;
    }
}

// Thunks that adapt our Allocator (alloc/free with alignment) to sokol's
// (size,user)/(ptr,user) shape. `user` is a pointer to our stored copy.
void* sokol_alloc_thunk(size_t size, void* user) {
    auto* a = static_cast<const Allocator*>(user);
    return a->alloc(size, alignof(std::max_align_t), a->user);
}
void sokol_free_thunk(void* ptr, void* user) {
    auto* a = static_cast<const Allocator*>(user);
    a->free(ptr, a->user);
}

// The backend AffineUI was compiled against (compile-time selected).
constexpr Backend compiled_backend() {
#if defined(AFFINEUI_BACKEND_D3D11)
    return Backend::d3d11;
#elif defined(AFFINEUI_BACKEND_METAL)
    return Backend::metal;
#elif defined(AFFINEUI_BACKEND_WGPU)
    return Backend::wgpu;
#else
    return Backend::gl;
#endif
}

struct PreparedFrame {
    int  pt_w{0};
    int  pt_h{0};
    std::uint32_t layout_us{0};
    std::uint32_t display_list_record_us{0};
    bool recorded{false};
    bool display_list_changed{false};
    bool display_list_diff_bounds_known{false};
    bool viewport_changed{false};
    bool layout_dirty{false};
    bool paint_dirty{false};
    bool animations_active{false};
    std::uint32_t dirty_rects{0};
    Rect dirty_bounds{};
    Rect raster_bounds{};
    Rect display_list_diff_bounds{};
    std::uint32_t display_list_diff_changed_ops{0};
    std::uint8_t display_list_diff_first_kind{0xFF};
    Rect display_list_diff_first_old_bounds{};
    Rect display_list_diff_first_new_bounds{};
    std::uint8_t display_list_diff_largest_kind{0xFF};
    Rect display_list_diff_largest_bounds{};
};

bool renderer_rect_valid(const Rect& r) {
    return r.w > 0 && r.h > 0;
}

Rect renderer_union_rect(const Rect& a, const Rect& b) {
    if (!renderer_rect_valid(a)) return b;
    if (!renderer_rect_valid(b)) return a;
    const int x0 = std::min(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w);
    const int y1 = std::max(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

Rect inflate_and_clip(Rect r, int pad, int w, int h) {
    if (!renderer_rect_valid(r) || w <= 0 || h <= 0) return {};
    const int x0 = std::clamp(r.x - pad, 0, w);
    const int y0 = std::clamp(r.y - pad, 0, h);
    const int x1 = std::clamp(r.x + r.w + pad, 0, w);
    const int y1 = std::clamp(r.y + r.h + pad, 0, h);
    if (x1 <= x0 || y1 <= y0) return {};
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

int round_layer_extent(int v) {
    // Root layer storage is deliberately a little larger than the current
    // viewport. During an interactive resize, most adjacent sizes can reuse
    // the same GPU image and only reraster the live content rectangle.
    constexpr int kLayerQuantum = 256;
    if (v <= 0) return 0;
    return ((v + kLayerQuantum - 1) / kLayerQuantum) * kLayerQuantum;
}

int points_to_pixels(int points, float dpi_scale) {
    const float d = dpi_scale > 0.0f ? dpi_scale : 1.0f;
    return static_cast<int>(static_cast<float>(points) * d + 0.5f);
}

std::uint32_t clamp_u32(std::uint64_t v) {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(v, std::numeric_limits<std::uint32_t>::max()));
}

std::uint32_t elapsed_us(std::chrono::steady_clock::time_point start) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    return clamp_u32(static_cast<std::uint64_t>(std::max<std::int64_t>(0, us)));
}

Rect pixel_rect_to_point_rect(int x, int y, int w, int h,
                              float dpi_scale, int pt_w, int pt_h) {
    if (w <= 0 || h <= 0 || pt_w <= 0 || pt_h <= 0) return {};
    const double d = dpi_scale > 0.0f ? static_cast<double>(dpi_scale) : 1.0;
    const int x0 = static_cast<int>(std::floor(static_cast<double>(x) / d));
    const int y0 = static_cast<int>(std::floor(static_cast<double>(y) / d));
    const int x1 =
        static_cast<int>(std::ceil(static_cast<double>(x + w) / d));
    const int y1 =
        static_cast<int>(std::ceil(static_cast<double>(y + h) / d));
    return inflate_and_clip(Rect{x0, y0, x1 - x0, y1 - y0}, 1, pt_w, pt_h);
}

struct RootCompositeVertex {
    float x, y;
    float u, v;
};

struct RootLayerUpdate {
    bool allocated{false};
    bool content_resized{false};
    int old_content_w{0};
    int old_content_h{0};
    int new_content_w{0};
    int new_content_h{0};
};

sg_shader make_root_composite_shader() {
    sg_shader_desc d{};
    d.attrs[0].glsl_name = "position";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.attrs[1].glsl_name = "texcoord";
    d.attrs[1].hlsl_sem_name = "TEXCOORD";
    d.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d.views[0].texture.image_type = SG_IMAGETYPE_2D;
    d.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    d.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    d.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[0].view_slot = 0;
    d.texture_sampler_pairs[0].sampler_slot = 0;
    d.texture_sampler_pairs[0].glsl_name = "tex";
    d.label = "affineui-root-composite-shader";

#if defined(SOKOL_D3D11) || defined(SOKOL_DUMMY_BACKEND)
    d.vertex_func.source =
        "struct vs_in { float2 position : POSITION; float2 texcoord : TEXCOORD0; };\n"
        "struct vs_out { float4 position : SV_Position; float2 uv : TEXCOORD0; };\n"
        "vs_out main(vs_in inp) {\n"
        "  vs_out outp;\n"
        "  outp.position = float4(inp.position, 0.0, 1.0);\n"
        "  outp.uv = inp.texcoord;\n"
        "  return outp;\n"
        "}\n";
    d.vertex_func.d3d11_target = "vs_4_0";
    d.fragment_func.source =
        "Texture2D tex : register(t0);\n"
        "SamplerState smp : register(s0);\n"
        "struct fs_in { float4 position : SV_Position; float2 uv : TEXCOORD0; };\n"
        "float4 main(fs_in inp) : SV_Target0 {\n"
        "  return tex.Sample(smp, inp.uv);\n"
        "}\n";
    d.fragment_func.d3d11_target = "ps_4_0";
#elif defined(SOKOL_GLCORE)
    d.vertex_func.source =
        "#version 330\n"
        "in vec2 position;\n"
        "in vec2 texcoord;\n"
        "out vec2 uv;\n"
        "void main(void) {\n"
        "  gl_Position = vec4(position, 0.0, 1.0);\n"
        "  uv = texcoord;\n"
        "}\n";
    d.fragment_func.source =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "in vec2 uv;\n"
        "out vec4 frag_color;\n"
        "void main(void) {\n"
        "  frag_color = texture(tex, uv);\n"
        "}\n";
#elif defined(SOKOL_METAL)
    d.vertex_func.source =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct vs_in { float2 position [[attribute(0)]]; float2 texcoord [[attribute(1)]]; };\n"
        "struct vs_out { float4 position [[position]]; float2 uv; };\n"
        "vertex vs_out vs_main(vs_in inp [[stage_in]]) {\n"
        "  vs_out outp;\n"
        "  outp.position = float4(inp.position, 0.0, 1.0);\n"
        "  outp.uv = inp.texcoord;\n"
        "  return outp;\n"
        "}\n";
    d.vertex_func.entry = "vs_main";
    d.fragment_func.source =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct fs_in { float4 position [[position]]; float2 uv; };\n"
        "fragment float4 fs_main(fs_in inp [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {\n"
        "  return tex.sample(smp, inp.uv);\n"
        "}\n";
    d.fragment_func.entry = "fs_main";
#else
    return {};
#endif

    return sg_make_shader(&d);
}

void destroy_root_composite(detail::RendererImpl& impl) {
    auto& comp = impl.root_composite;
    if (comp.pipeline.id != SG_INVALID_ID) {
        sg_destroy_pipeline(comp.pipeline);
        comp.pipeline = {};
    }
    if (comp.shader.id != SG_INVALID_ID) {
        sg_destroy_shader(comp.shader);
        comp.shader = {};
    }
    if (comp.vertex_buffer.id != SG_INVALID_ID) {
        sg_destroy_buffer(comp.vertex_buffer);
        comp.vertex_buffer = {};
    }
    if (comp.sampler.id != SG_INVALID_ID) {
        sg_destroy_sampler(comp.sampler);
        comp.sampler = {};
    }
    comp.valid = false;
    comp.color_format = SG_PIXELFORMAT_NONE;
    comp.depth_format = SG_PIXELFORMAT_NONE;
    comp.sample_count = 0;
    comp.u_max = -1.0f;
    comp.v_max = -1.0f;
}

bool ensure_root_composite(detail::RendererImpl& impl, int sample_count) {
    auto& comp = impl.root_composite;
    if (comp.valid &&
        comp.color_format == impl.sw_color &&
        comp.depth_format == impl.sw_depth &&
        comp.sample_count == sample_count) {
        return true;
    }
    destroy_root_composite(impl);

    comp.shader = make_root_composite_shader();
    if (comp.shader.id == SG_INVALID_ID ||
        sg_query_shader_state(comp.shader) != SG_RESOURCESTATE_VALID) {
        destroy_root_composite(impl);
        return false;
    }

    sg_buffer_desc bd{};
    bd.size = sizeof(RootCompositeVertex) * 4;
    bd.usage.vertex_buffer = true;
    bd.usage.stream_update = true;
    bd.label = "affineui-root-composite-quad";
    comp.vertex_buffer = sg_make_buffer(&bd);

    sg_sampler_desc sd{};
    // The retained root is composited as a 1:1 texel copy from the live
    // content rectangle, even when the backing image has spare resize
    // capacity. Filtering it would resample every glyph and edge a second
    // time, making text look soft/crunchy compared with a direct paint.
    // Future transformed/scaled layers can opt into linear sampling per
    // layer; the root layer should stay nearest-neighbor.
    sd.min_filter = SG_FILTER_NEAREST;
    sd.mag_filter = SG_FILTER_NEAREST;
    sd.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    comp.sampler = sg_make_sampler(&sd);

    sg_pipeline_desc pd{};
    pd.shader = comp.shader;
    pd.layout.buffers[0].stride = static_cast<int>(sizeof(RootCompositeVertex));
    pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[1].offset = static_cast<int>(sizeof(float) * 2);
    pd.primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
    pd.index_type = SG_INDEXTYPE_NONE;
    pd.cull_mode = SG_CULLMODE_NONE;
    pd.depth.pixel_format = impl.sw_depth;
    pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
    pd.depth.write_enabled = false;
    pd.color_count = 1;
    pd.colors[0].pixel_format = impl.sw_color;
    pd.colors[0].blend.enabled = true;
    pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.op_rgb = SG_BLENDOP_ADD;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.op_alpha = SG_BLENDOP_ADD;
    pd.sample_count = sample_count > 0 ? sample_count : 1;
    pd.label = "affineui-root-composite-pipeline";
    comp.pipeline = sg_make_pipeline(&pd);

    comp.color_format = impl.sw_color;
    comp.depth_format = impl.sw_depth;
    comp.sample_count = pd.sample_count;
    comp.valid =
        comp.vertex_buffer.id != SG_INVALID_ID &&
        comp.sampler.id != SG_INVALID_ID &&
        comp.pipeline.id != SG_INVALID_ID &&
        sg_query_buffer_state(comp.vertex_buffer) == SG_RESOURCESTATE_VALID &&
        sg_query_sampler_state(comp.sampler) == SG_RESOURCESTATE_VALID &&
        sg_query_pipeline_state(comp.pipeline) == SG_RESOURCESTATE_VALID;
    if (!comp.valid) {
        destroy_root_composite(impl);
        return false;
    }
    return true;
}

PreparedFrame prepare_frame(detail::RendererImpl& impl,
                            Document& doc,
                            int fb_w,
                            int fb_h,
                            float dpi_scale) {
    const auto prepare_start = std::chrono::steady_clock::now();
    PreparedFrame frame{};
    const float d = dpi_scale > 0.0f ? dpi_scale : 1.0f;
    // Layout and CSS live in logical CSS pixels/points. The framebuffer,
    // root layer, and swapchain live in physical pixels. A Retina 1024x680
    // window therefore lays out at 1024x680 but rasterizes into about
    // 2048x1360, with NanoVG's devicePixelRatio preserving real 1px CSS
    // geometry rather than upscaling a half-resolution image.
    frame.pt_w = static_cast<int>(static_cast<float>(fb_w) / d + 0.5f);
    frame.pt_h = static_cast<int>(static_cast<float>(fb_h) / d + 0.5f);

    frame.viewport_changed =
        impl.first_frame || fb_w != impl.last_w || fb_h != impl.last_h ||
        dpi_scale != impl.last_dpi;
    // The document's "needs layout" signal is content_size == 0 (set by
    // every layout-affecting mutation; dispatch's hidden relayout keys
    // off the same bit). Comparing against the viewport width instead
    // relayouted EVERY rendered frame whenever content legitimately
    // overflowed the viewport by a pixel — a permanent 5-10 ms/frame
    // layout thrash that only showed up under sustained repaints
    // (cable drags). Viewport/dpi changes are handled above.
    frame.layout_dirty = doc.content_size().width == 0;
    if (frame.viewport_changed || frame.layout_dirty) {
        const auto layout_start = std::chrono::steady_clock::now();
        doc.layout(frame.pt_w, frame.pt_h, impl.painter.get());
        frame.layout_us = elapsed_us(layout_start);
        impl.last_w      = fb_w;
        impl.last_h      = fb_h;
        impl.last_dpi    = dpi_scale;
        impl.first_frame = false;
    }

    const auto dirty_rects = doc.take_dirty_rects();
    frame.dirty_rects = static_cast<std::uint32_t>(dirty_rects.size());
    for (const auto& r : dirty_rects) {
        frame.dirty_bounds = renderer_union_rect(frame.dirty_bounds, r);
    }
    frame.dirty_bounds = inflate_and_clip(frame.dirty_bounds, 2,
                                          frame.pt_w, frame.pt_h);
    const std::uint64_t frame_area =
        static_cast<std::uint64_t>(std::max(frame.pt_w, 1)) *
        static_cast<std::uint64_t>(std::max(frame.pt_h, 1));
    frame.paint_dirty = doc.take_paint_dirty();
    frame.animations_active = doc.has_active_animations();
    const bool document_changed =
        frame.viewport_changed || frame.layout_dirty || frame.paint_dirty ||
        !dirty_rects.empty() || frame.animations_active ||
        impl.last_frame_had_active_animations;

    if (!impl.cached_display_list_valid || document_changed) {
        const auto record_start = std::chrono::steady_clock::now();
        detail::DisplayListBuilder builder(impl.painter.get());
        builder.begin_frame(frame.pt_w, frame.pt_h, dpi_scale);
        doc.draw(builder);
        builder.end_frame();
        const bool had_cached_list = impl.cached_display_list_valid;
        frame.display_list_changed =
            !had_cached_list ||
            builder.list().content_hash != impl.cached_display_list.content_hash ||
            builder.list().ops.size() != impl.cached_display_list.ops.size() ||
            builder.list().text_pool.size() !=
                impl.cached_display_list.text_pool.size();
        if (had_cached_list && frame.display_list_changed) {
            const auto diff = detail::display_list_diff_bounds(
                impl.cached_display_list, builder.list());
            frame.display_list_diff_bounds_known =
                diff.known && renderer_rect_valid(diff.bounds);
            frame.display_list_diff_changed_ops = diff.changed_ops;
            frame.display_list_diff_first_kind = diff.first_changed_kind;
            frame.display_list_diff_first_old_bounds = diff.first_old_bounds;
            frame.display_list_diff_first_new_bounds = diff.first_new_bounds;
            frame.display_list_diff_largest_kind = diff.largest_changed_kind;
            frame.display_list_diff_largest_bounds = diff.largest_changed_bounds;
            if (frame.display_list_diff_bounds_known) {
                frame.display_list_diff_bounds = inflate_and_clip(
                    diff.bounds, 4, frame.pt_w, frame.pt_h);
            }
        }
        impl.cached_display_list = std::move(builder.list());
        impl.cached_display_list_valid = true;
        frame.recorded = true;
        frame.display_list_record_us = elapsed_us(record_start);
    }
    impl.last_frame_had_active_animations = frame.animations_active;

    frame.raster_bounds = frame.dirty_bounds;
    if (frame.display_list_diff_bounds_known) {
        frame.raster_bounds =
            renderer_union_rect(frame.raster_bounds, frame.display_list_diff_bounds);
        frame.raster_bounds = inflate_and_clip(frame.raster_bounds, 2,
                                              frame.pt_w, frame.pt_h);
    }
    const std::uint64_t dirty_area =
        renderer_rect_valid(frame.raster_bounds)
            ? static_cast<std::uint64_t>(frame.raster_bounds.w) *
                  static_cast<std::uint64_t>(frame.raster_bounds.h)
            : 0;

    impl.stats.frames += 1;
    if (frame.recorded) impl.stats.display_list_records += 1;
    if (frame.recorded) {
        if (frame.display_list_changed) {
            impl.stats.display_list_changes += 1;
        } else {
            impl.stats.display_list_unchanged += 1;
        }
    }
    impl.stats.cached_ops =
        static_cast<std::uint32_t>(impl.cached_display_list.ops.size());
    impl.stats.dirty_rects = frame.dirty_rects;
    impl.stats.dirty_area_px = clamp_u32(dirty_area);
    impl.stats.dirty_area_pct_x100 =
        frame_area > 0 ? clamp_u32((dirty_area * 10000u) / frame_area) : 0;
    impl.stats.display_list_ops_culled_this_frame = 0;
    impl.stats.display_list_diff_changed_ops =
        frame.display_list_diff_changed_ops;
    impl.stats.display_list_diff_first_kind =
        frame.display_list_diff_first_kind;
    impl.stats.display_list_diff_first_old_bounds =
        frame.display_list_diff_first_old_bounds;
    impl.stats.display_list_diff_first_new_bounds =
        frame.display_list_diff_first_new_bounds;
    impl.stats.display_list_diff_largest_kind =
        frame.display_list_diff_largest_kind;
    impl.stats.display_list_diff_largest_bounds =
        frame.display_list_diff_largest_bounds;
    impl.stats.prepare_us_this_frame = elapsed_us(prepare_start);
    impl.stats.layout_us_this_frame = frame.layout_us;
    impl.stats.display_list_record_us_this_frame =
        frame.display_list_record_us;
    impl.stats.raster_us_this_frame = 0;
    impl.stats.composite_us_this_frame = 0;
    impl.stats.recorded_this_frame = frame.recorded;
    impl.stats.display_list_changed_this_frame = frame.display_list_changed;
    impl.stats.root_layer_rasterized_this_frame = false;
    impl.stats.root_layer_partial_this_frame = false;
    impl.stats.root_layer_direct_this_frame = false;
    impl.stats.root_layer_reused_this_frame = false;
    impl.stats.root_layer_allocated_this_frame = false;
    impl.stats.viewport_changed = frame.viewport_changed;
    impl.stats.layout_dirty = frame.layout_dirty;
    impl.stats.paint_dirty = frame.paint_dirty;
    impl.stats.animations_active = frame.animations_active;
    return frame;
}

void destroy_root_layer(detail::RendererImpl& impl) {
    auto& layer = impl.root_layer;
    if (layer.nvg_image != 0 && impl.vg) {
        nvgDeleteImage(impl.vg, layer.nvg_image);
        layer.nvg_image = 0;
    }
    if (layer.depth_att.id != SG_INVALID_ID) {
        sg_destroy_view(layer.depth_att);
        layer.depth_att = {};
    }
    if (layer.color_att.id != SG_INVALID_ID) {
        sg_destroy_view(layer.color_att);
        layer.color_att = {};
    }
    if (layer.color_tex.id != SG_INVALID_ID) {
        sg_destroy_view(layer.color_tex);
        layer.color_tex = {};
    }
    if (layer.depth_img.id != SG_INVALID_ID) {
        sg_destroy_image(layer.depth_img);
        layer.depth_img = {};
    }
    if (layer.color_img.id != SG_INVALID_ID) {
        sg_destroy_image(layer.color_img);
        layer.color_img = {};
    }
    layer.w = 0;
    layer.h = 0;
    layer.content_w = 0;
    layer.content_h = 0;
    layer.valid = false;
}

bool ensure_root_layer(detail::RendererImpl& impl, int w, int h,
                       RootLayerUpdate* update = nullptr) {
    auto& layer = impl.root_layer;
    if (update) {
        *update = {};
    }
    if (layer.color_img.id != SG_INVALID_ID && layer.w >= w && layer.h >= h) {
        if (layer.content_w != w || layer.content_h != h) {
            if (update) {
                update->content_resized = true;
                update->old_content_w = layer.content_w;
                update->old_content_h = layer.content_h;
                update->new_content_w = w;
                update->new_content_h = h;
            }
            layer.content_w = w;
            layer.content_h = h;
            // The visible size changed but the texture stayed in place. The
            // cached raster is for the OLD content size — its glyphs and
            // boxes no longer line up with the new layout, and the composite
            // samples a region that was never drawn (the resize-to-blank
            // bug). Unless the exposed-strip partial path is proven on,
            // force a full re-raster by invalidating: the render_to branch
            // then takes the RASTER path this same frame.
            if (!impl.partial_root_raster_enabled) {
                layer.valid = false;
            }
        }
        impl.stats.root_layer_capacity_w = static_cast<std::uint32_t>(layer.w);
        impl.stats.root_layer_capacity_h = static_cast<std::uint32_t>(layer.h);
        impl.stats.root_layer_content_w = static_cast<std::uint32_t>(layer.content_w);
        impl.stats.root_layer_content_h = static_cast<std::uint32_t>(layer.content_h);
        return true;
    }
    destroy_root_layer(impl);
    if (w <= 0 || h <= 0) return false;
    const int alloc_w = round_layer_extent(w);
    const int alloc_h = round_layer_extent(h);

    sg_image_desc color_desc{};
    color_desc.width = alloc_w;
    color_desc.height = alloc_h;
    color_desc.pixel_format = impl.sw_color;
    color_desc.sample_count = 1;
    color_desc.usage.color_attachment = true;
    color_desc.label = "affineui-root-layer-color";
    layer.color_img = sg_make_image(&color_desc);

    sg_image_desc depth_desc{};
    depth_desc.width = alloc_w;
    depth_desc.height = alloc_h;
    depth_desc.pixel_format = impl.sw_depth;
    depth_desc.sample_count = 1;
    depth_desc.usage.depth_stencil_attachment = true;
    depth_desc.label = "affineui-root-layer-depth";
    layer.depth_img = sg_make_image(&depth_desc);
    if (layer.color_img.id == SG_INVALID_ID ||
        layer.depth_img.id == SG_INVALID_ID) {
        destroy_root_layer(impl);
        return false;
    }

    sg_view_desc color_view_desc{};
    color_view_desc.color_attachment.image = layer.color_img;
    layer.color_att = sg_make_view(&color_view_desc);

    sg_view_desc texture_view_desc{};
    texture_view_desc.texture.image = layer.color_img;
    layer.color_tex = sg_make_view(&texture_view_desc);

    sg_view_desc depth_view_desc{};
    depth_view_desc.depth_stencil_attachment.image = layer.depth_img;
    layer.depth_att = sg_make_view(&depth_view_desc);

    layer.nvg_image = nvsgCreateImageFromHandle(
        impl.vg, layer.color_img, sg_sampler{}, alloc_w, alloc_h,
        NVG_IMAGE_NODELETE);
    if (layer.nvg_image == 0 ||
        layer.color_img.id == SG_INVALID_ID ||
        layer.depth_img.id == SG_INVALID_ID ||
        layer.color_tex.id == SG_INVALID_ID ||
        layer.color_att.id == SG_INVALID_ID ||
        layer.depth_att.id == SG_INVALID_ID) {
        destroy_root_layer(impl);
        return false;
    }

    layer.w = alloc_w;
    layer.h = alloc_h;
    layer.content_w = w;
    layer.content_h = h;
    layer.valid = false;
    if (update) {
        update->allocated = true;
        update->content_resized = true;
        update->new_content_w = w;
        update->new_content_h = h;
    }
    impl.stats.root_layer_allocated_this_frame = true;
    impl.stats.root_layer_capacity_w = static_cast<std::uint32_t>(layer.w);
    impl.stats.root_layer_capacity_h = static_cast<std::uint32_t>(layer.h);
    impl.stats.root_layer_content_w = static_cast<std::uint32_t>(layer.content_w);
    impl.stats.root_layer_content_h = static_cast<std::uint32_t>(layer.content_h);
    return true;
}

void clear_nvg_rect(NVGcontext* vg, const Rect& r);

void rasterize_root_layer(detail::RendererImpl& impl,
                          int pt_w,
                          int pt_h,
                          float dpi_scale) {
    const auto raster_start = std::chrono::steady_clock::now();
    auto& layer = impl.root_layer;
    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = sg_color{0.0f, 0.0f, 0.0f, 0.0f};
    pass.action.depth.load_action = SG_LOADACTION_CLEAR;
    pass.action.depth.clear_value = 1.0f;
    pass.action.stencil.load_action = SG_LOADACTION_CLEAR;
    pass.action.stencil.clear_value = 0;
    pass.attachments.colors[0] = layer.color_att;
    pass.attachments.depth_stencil = layer.depth_att;

    sg_begin_pass(&pass);
    const int px_w = std::clamp(points_to_pixels(pt_w, dpi_scale), 0, layer.w);
    const int px_h = std::clamp(points_to_pixels(pt_h, dpi_scale), 0, layer.h);
    sg_apply_viewport(0, 0, px_w, px_h, /*origin_top_left*/ true);
    sg_apply_scissor_rect(0, 0, px_w, px_h, /*origin_top_left*/ true);
    impl.painter->begin_frame(pt_w, pt_h, dpi_scale);
    detail::replay(impl.cached_display_list, *impl.painter);
    impl.painter->end_frame();
    sg_end_pass();

    layer.valid = true;
    impl.stats.display_list_replays += 1;
    impl.stats.root_layer_rasterizes += 1;
    impl.stats.root_layer_rasterized_this_frame = true;
    impl.stats.raster_us_this_frame = elapsed_us(raster_start);
}

void clear_nvg_rect(NVGcontext* vg, const Rect& r) {
    nvgSave(vg);
    nvgScissor(vg, static_cast<float>(r.x), static_cast<float>(r.y),
               static_cast<float>(r.w), static_cast<float>(r.h));
    nvgGlobalCompositeOperation(vg, NVG_COPY);
    nvgBeginPath(vg);
    nvgRect(vg, static_cast<float>(r.x), static_cast<float>(r.y),
            static_cast<float>(r.w), static_cast<float>(r.h));
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 0));
    nvgFill(vg);
    nvgRestore(vg);
}

void rasterize_root_layer_region(detail::RendererImpl& impl,
                                 int pt_w,
                                 int pt_h,
                                 float dpi_scale,
                                 const Rect& dirty) {
    if (!renderer_rect_valid(dirty)) return;
    const auto raster_start = std::chrono::steady_clock::now();
    auto& layer = impl.root_layer;
    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_LOAD;
    pass.action.depth.load_action = SG_LOADACTION_CLEAR;
    pass.action.depth.clear_value = 1.0f;
    pass.action.stencil.load_action = SG_LOADACTION_CLEAR;
    pass.action.stencil.clear_value = 0;
    pass.attachments.colors[0] = layer.color_att;
    pass.attachments.depth_stencil = layer.depth_att;

    sg_begin_pass(&pass);
    const int px_w = std::clamp(points_to_pixels(pt_w, dpi_scale), 0, layer.w);
    const int px_h = std::clamp(points_to_pixels(pt_h, dpi_scale), 0, layer.h);
    sg_apply_viewport(0, 0, px_w, px_h, /*origin_top_left*/ true);
    sg_apply_scissor_rect(0, 0, px_w, px_h, /*origin_top_left*/ true);
    impl.painter->begin_frame(pt_w, pt_h, dpi_scale);
    clear_nvg_rect(impl.vg, dirty);
    nvgSave(impl.vg);
    nvgScissor(impl.vg,
               static_cast<float>(dirty.x), static_cast<float>(dirty.y),
               static_cast<float>(dirty.w), static_cast<float>(dirty.h));
    const auto replay_stats =
        detail::replay_clipped(impl.cached_display_list, *impl.painter, dirty);
    nvgRestore(impl.vg);
    impl.painter->end_frame();
    sg_end_pass();

    layer.valid = true;
    impl.stats.display_list_replays += 1;
    impl.stats.display_list_ops_culled += replay_stats.culled;
    impl.stats.display_list_ops_culled_this_frame =
        clamp_u32(replay_stats.culled);
    impl.stats.root_layer_rasterizes += 1;
    impl.stats.root_layer_partial_rasterizes += 1;
    impl.stats.root_layer_rasterized_this_frame = true;
    impl.stats.root_layer_partial_this_frame = true;
    impl.stats.raster_us_this_frame += elapsed_us(raster_start);
}

void composite_root_layer(detail::RendererImpl& impl,
                           int pt_w,
                           int pt_h,
                           float dpi_scale,
                           int sample_count,
                           int visible_px_w,
                           int visible_px_h) {
    const auto composite_start = std::chrono::steady_clock::now();
    auto& layer = impl.root_layer;
    if (layer.color_tex.id != SG_INVALID_ID &&
        ensure_root_composite(impl, sample_count)) {
        auto& comp = impl.root_composite;
        const float u_max = layer.w > 0
            ? std::clamp(static_cast<float>(visible_px_w) /
                         static_cast<float>(layer.w), 0.0f, 1.0f)
            : 1.0f;
        const float v_max = layer.h > 0
            ? std::clamp(static_cast<float>(visible_px_h) /
                         static_cast<float>(layer.h), 0.0f, 1.0f)
            : 1.0f;
        if (u_max != comp.u_max || v_max != comp.v_max) {
            const RootCompositeVertex quad[] = {
                {-1.0f, -1.0f, 0.0f,  v_max},
                { 1.0f, -1.0f, u_max, v_max},
                {-1.0f,  1.0f, 0.0f,  0.0f},
                { 1.0f,  1.0f, u_max, 0.0f},
            };
            sg_update_buffer(comp.vertex_buffer, SG_RANGE(quad));
            comp.u_max = u_max;
            comp.v_max = v_max;
        }
        sg_apply_pipeline(comp.pipeline);
        sg_bindings b{};
        b.vertex_buffers[0] = comp.vertex_buffer;
        b.views[0] = layer.color_tex;
        b.samplers[0] = comp.sampler;
        sg_apply_bindings(&b);
        sg_draw(0, 4, 1);
        impl.stats.root_layer_composites += 1;
        impl.stats.root_layer_direct_composites += 1;
        impl.stats.root_layer_direct_this_frame = true;
        impl.stats.root_layer_reused_this_frame =
            !impl.stats.root_layer_rasterized_this_frame;
        impl.stats.composite_us_this_frame = elapsed_us(composite_start);
        return;
    }

    NVGcontext* vg = impl.vg;
    nvgBeginFrame(vg, static_cast<float>(pt_w), static_cast<float>(pt_h),
                  dpi_scale);
    const float image_w = dpi_scale > 0.0f
        ? static_cast<float>(layer.w) / dpi_scale
        : static_cast<float>(pt_w);
    const float image_h = dpi_scale > 0.0f
        ? static_cast<float>(layer.h) / dpi_scale
        : static_cast<float>(pt_h);
    const NVGpaint image = nvgImagePattern(
        vg, 0.0f, 0.0f, image_w, image_h,
        0.0f, layer.nvg_image, 1.0f);
    nvgBeginPath(vg);
    nvgRect(vg, 0.0f, 0.0f, static_cast<float>(pt_w),
            static_cast<float>(pt_h));
    nvgFillPaint(vg, image);
    nvgFill(vg);
    nvgEndFrame(vg);
    impl.stats.root_layer_composites += 1;
    impl.stats.root_layer_reused_this_frame =
        !impl.stats.root_layer_rasterized_this_frame;
    impl.stats.composite_us_this_frame = elapsed_us(composite_start);
}

}  // namespace

void Renderer::init_embedded(const GpuContext& gpu, const Allocator* allocator) {
    if (impl_->ready) return;

    if (gpu.backend != compiled_backend()) {
        detail::log_msg(LogLevel::error,
            "[affineui] init_embedded: GpuContext backend does not match the "
            "backend AffineUI was compiled for; rebuild with the matching "
            "AFFINEUI_BACKEND.");
        return;
    }

    sg_environment env{};
    env.defaults.color_format = to_sg_format(gpu.color_format, false);
    env.defaults.depth_format = to_sg_format(gpu.depth_format, true);
    env.defaults.sample_count = gpu.sample_count > 0 ? gpu.sample_count : 1;
    impl_->sw_color = env.defaults.color_format;
    impl_->sw_depth = env.defaults.depth_format;
    // Only the active backend's handles are read by sokol_gfx; copying all
    // is harmless and keeps this branch backend-agnostic.
    env.metal.device         = gpu.metal.device;
    env.d3d11.device         = gpu.d3d11.device;
    env.d3d11.device_context = gpu.d3d11.device_context;
    env.wgpu.device          = gpu.wgpu.device;

    sg_desc d{};
    d.environment = env;
    d.logger.func = slog_func;  // sokol prints validation errors / panics
    // Route sokol_gfx allocation through the host allocator when supplied.
    // We copy the small POD (principle: never retain a host pointer) and
    // point sokol's user_data at our copy. TODO(embed §6): route stb /
    // fontstash / lexbor / nanovg-fork + std (global new) through it too.
    if (allocator && allocator->alloc && allocator->free) {
        impl_->alloc      = *allocator;
        impl_->has_alloc  = true;
        d.allocator.alloc_fn  = sokol_alloc_thunk;
        d.allocator.free_fn   = sokol_free_thunk;
        d.allocator.user_data = &impl_->alloc;
    }
    sg_setup(&d);
    if (!sg_isvalid()) {
        detail::log_msg(LogLevel::error, "[affineui] init_embedded: sg_setup failed");
        return;
    }
    impl_->owns_sg = true;

    // NanoVG + painter on top of the now-live sokol_gfx context.
    init_gl();
}

void Renderer::render_to(Document& doc, const FrameTarget& t) {
    if (!impl_->ready) {
        // In embedded mode init_embedded() usually created the sokol_gfx
        // context. The standalone window adapters also use FrameTarget now,
        // but they set up sokol_gfx themselves and rely on lazy NanoVG init.
        init_gl();
        if (!impl_->ready) {
            detail::log_msg(LogLevel::error,
                "[affineui] render_to: renderer is not initialized; ensure "
                "sokol_gfx is set up or call Ui::init() with a GpuContext.");
            return;
        }
    }
    if (t.width <= 0 || t.height <= 0) return;  // nothing to draw into

    // Optional sub-rect viewport. The pass clear is whole-RTV, so a sub-rect
    // panel cannot use it — it draws over existing content (its body
    // background paints the panel backdrop). Clamp to the target (princ. 5).
    const bool sub = t.viewport.w > 0 && t.viewport.h > 0;
    int vx = 0, vy = 0, vw = t.width, vh = t.height;
    if (sub) {
        vx = t.viewport.x < 0 ? 0 : (t.viewport.x > t.width  ? t.width  : t.viewport.x);
        vy = t.viewport.y < 0 ? 0 : (t.viewport.y > t.height ? t.height : t.viewport.y);
        vw = t.viewport.w > t.width  - vx ? t.width  - vx : t.viewport.w;
        vh = t.viewport.h > t.height - vy ? t.height - vy : t.viewport.h;
        if (vw <= 0 || vh <= 0) return;
    }

    sg_swapchain sc{};
    sc.width        = t.width;
    sc.height       = t.height;
    sc.sample_count = t.sample_count > 0 ? t.sample_count : 1;
    sc.color_format = impl_->sw_color;   // match sglue_swapchain(): set explicitly
    sc.depth_format = impl_->sw_depth;
    sc.metal.current_drawable      = t.metal.current_drawable;
    sc.metal.depth_stencil_texture = t.metal.depth_stencil_texture;
    sc.metal.msaa_color_texture    = t.metal.msaa_color_texture;
    sc.d3d11.render_view           = t.d3d11.render_view;
    sc.d3d11.resolve_view          = t.d3d11.resolve_view;
    sc.d3d11.depth_stencil_view    = t.d3d11.depth_stencil_view;
    sc.wgpu.render_view            = t.wgpu.render_view;
    sc.wgpu.resolve_view           = t.wgpu.resolve_view;
    sc.wgpu.depth_stencil_view     = t.wgpu.depth_stencil_view;
    sc.gl.framebuffer              = t.gl.framebuffer;

    sg_pass pass{};
    if (t.clear && !sub) {
        const Color c = impl_->clear_color;
        pass.action.colors[0].load_action  = SG_LOADACTION_CLEAR;
        pass.action.colors[0].clear_value.r = c.r / 255.0f;
        pass.action.colors[0].clear_value.g = c.g / 255.0f;
        pass.action.colors[0].clear_value.b = c.b / 255.0f;
        pass.action.colors[0].clear_value.a = c.a / 255.0f;
    } else {
        pass.action.colors[0].load_action = SG_LOADACTION_LOAD;
    }
    pass.action.depth.load_action   = SG_LOADACTION_CLEAR;
    pass.action.depth.clear_value   = 1.0f;
    pass.action.stencil.load_action = SG_LOADACTION_CLEAR;
    pass.action.stencil.clear_value = 0;
    pass.swapchain = sc;

    const PreparedFrame frame = prepare_frame(*impl_, doc, vw, vh, t.dpi_scale);
    const std::uint64_t frame_area =
        static_cast<std::uint64_t>(std::max(frame.pt_w, 1)) *
        static_cast<std::uint64_t>(std::max(frame.pt_h, 1));
    const std::uint64_t dirty_area =
        renderer_rect_valid(frame.raster_bounds)
            ? static_cast<std::uint64_t>(frame.raster_bounds.w) *
                  static_cast<std::uint64_t>(frame.raster_bounds.h)
            : 0;
    const bool dirty_covers_frame =
        !renderer_rect_valid(frame.raster_bounds) ||
        (frame_area > 0 && dirty_area * 100u >= frame_area * 80u);
    const bool has_retained_surface =
        impl_->root_layer.color_img.id != SG_INVALID_ID;
    const bool direct_live_resize =
        !sub &&
        has_retained_surface &&
        frame.viewport_changed &&
        frame.display_list_changed &&
        dirty_covers_frame;
    if (direct_live_resize) {
        const auto raster_start = std::chrono::steady_clock::now();
        impl_->root_layer.valid = false;
        sg_begin_pass(&pass);
        sg_apply_viewport(vx, vy, vw, vh, /*origin_top_left*/ true);
        sg_apply_scissor_rect(vx, vy, vw, vh, /*origin_top_left*/ true);
        impl_->painter->begin_frame(frame.pt_w, frame.pt_h, t.dpi_scale);
        detail::replay(impl_->cached_display_list, *impl_->painter);
        impl_->painter->end_frame();
        impl_->stats.display_list_replays += 1;
        impl_->stats.root_layer_direct_this_frame = true;
        impl_->stats.raster_us_this_frame = elapsed_us(raster_start);
        if (!t.debug_overlay_text.empty()) {
            draw_debug_overlay(
                t.debug_overlay_text,
                std::span<const float>(t.debug_overlay_frame_ms,
                                       t.debug_overlay_frame_ms_count),
                t.width, t.height, t.dpi_scale, t.debug_overlay_corner);
        }
        sg_end_pass();
        if (t.commit) sg_commit();
        return;
    }

    RootLayerUpdate layer_update{};
    const bool layer_ready = ensure_root_layer(*impl_, vw, vh, &layer_update);
    if (!layer_ready) {
        sg_begin_pass(&pass);
        sg_apply_viewport(vx, vy, vw, vh, /*origin_top_left*/ true);
        sg_apply_scissor_rect(vx, vy, vw, vh, /*origin_top_left*/ true);
        impl_->painter->begin_frame(frame.pt_w, frame.pt_h, t.dpi_scale);
        detail::replay(impl_->cached_display_list, *impl_->painter);
        impl_->painter->end_frame();
        impl_->stats.display_list_replays += 1;
        if (!t.debug_overlay_text.empty()) {
            draw_debug_overlay(
                t.debug_overlay_text,
                std::span<const float>(t.debug_overlay_frame_ms,
                                       t.debug_overlay_frame_ms_count),
                t.width, t.height, t.dpi_scale, t.debug_overlay_corner);
        }
        sg_end_pass();
        if (t.commit) sg_commit();
        return;
    }

    if (frame.display_list_changed || !impl_->root_layer.valid) {
        const bool bounded_known_partial =
            impl_->root_layer.valid &&
            frame.display_list_changed &&
            renderer_rect_valid(frame.raster_bounds) &&
            frame.display_list_diff_bounds_known &&
            !frame.paint_dirty &&
            frame_area > 0 &&
            dirty_area * 100u <= frame_area * 40u;
        const bool can_partial_raster =
            impl_->partial_root_raster_enabled &&
            (bounded_known_partial ||
             (impl_->root_layer.valid &&
              frame.display_list_changed &&
              renderer_rect_valid(frame.raster_bounds) &&
              frame.display_list_diff_bounds_known &&
              !frame.viewport_changed &&
              !frame.paint_dirty));
        if (can_partial_raster) {
            rasterize_root_layer_region(*impl_, frame.pt_w, frame.pt_h,
                                        t.dpi_scale, frame.raster_bounds);
        } else {
            rasterize_root_layer(*impl_, frame.pt_w, frame.pt_h, t.dpi_scale);
        }
    } else if (layer_update.content_resized) {
        if (!impl_->partial_root_raster_enabled) {
            rasterize_root_layer(*impl_, frame.pt_w, frame.pt_h, t.dpi_scale);
        } else {
            std::array<Rect, 2> exposed_regions{};
            std::size_t exposed_count = 0;
            const int old_w = std::max(layer_update.old_content_w, 0);
            const int old_h = std::max(layer_update.old_content_h, 0);
            const int new_w = layer_update.new_content_w;
            const int new_h = layer_update.new_content_h;
            if (new_w > old_w) {
                exposed_regions[exposed_count++] = pixel_rect_to_point_rect(
                    old_w, 0, new_w - old_w, new_h, t.dpi_scale,
                    frame.pt_w, frame.pt_h);
            }
            if (new_h > old_h) {
                // If width also grew, the right strip above already covers the
                // bottom-right corner. Restrict the bottom strip to the
                // previously painted width so resize growth stays a pair of
                // narrow repaints, not a full-viewport union.
                exposed_regions[exposed_count++] = pixel_rect_to_point_rect(
                    0, old_h, std::min(new_w, old_w), new_h - old_h,
                    t.dpi_scale, frame.pt_w, frame.pt_h);
            }
            for (std::size_t i = 0; i < exposed_count; ++i) {
                if (renderer_rect_valid(exposed_regions[i])) {
                    rasterize_root_layer_region(*impl_, frame.pt_w, frame.pt_h,
                                                t.dpi_scale, exposed_regions[i]);
                }
            }
        }
    }

    sg_begin_pass(&pass);
    sg_apply_viewport(vx, vy, vw, vh, /*origin_top_left*/ true);
    sg_apply_scissor_rect(vx, vy, vw, vh, /*origin_top_left*/ true);
    composite_root_layer(*impl_, frame.pt_w, frame.pt_h, t.dpi_scale,
                         sc.sample_count, vw, vh);
    if (!t.debug_overlay_text.empty()) {
        draw_debug_overlay(
            t.debug_overlay_text,
            std::span<const float>(t.debug_overlay_frame_ms,
                                   t.debug_overlay_frame_ms_count),
            t.width, t.height, t.dpi_scale, t.debug_overlay_corner);
    }
    sg_end_pass();
    if (t.commit) sg_commit();
}

void Renderer::shutdown() {
    if (!impl_->ready && !impl_->owns_sg) return;
    impl_->painter.reset();
    destroy_root_layer(*impl_);
    destroy_root_composite(*impl_);
    if (impl_->vg) {
        nvgDeleteSokol(impl_->vg);
        impl_->vg = nullptr;
    }
    impl_->ready = false;
    if (impl_->owns_sg) {
        sg_shutdown();
        impl_->owns_sg = false;
    }
}

void Renderer::render(Document& doc, int fb_w, int fb_h, float dpi_scale) {
    if (!impl_->ready) {
        init_gl();
        if (!impl_->ready) return;
    }

    // CSS layout works in points; the framebuffer is in pixels.
    const PreparedFrame frame = prepare_frame(*impl_, doc, fb_w, fb_h, dpi_scale);

    // Record paint into a DisplayList (CPU only; no GPU calls)…
    // …then replay through the NanoVG painter, which emits sokol_gfx
    // draws into the render pass the caller opened. The clear is handled
    // by that pass's load action (App builds it from clear_color).
    impl_->painter->begin_frame(frame.pt_w, frame.pt_h, dpi_scale);
    detail::replay(impl_->cached_display_list, *impl_->painter);
    impl_->painter->end_frame();
    impl_->stats.display_list_replays += 1;
}

const RenderStats& Renderer::stats() const noexcept {
    return impl_->stats;
}

void Renderer::draw_debug_overlay(std::string_view text,
                                  std::span<const float> frame_ms,
                                  int fb_w,
                                  int fb_h,
                                  float dpi_scale,
                                  DebugOverlayCorner corner) {
    if (!impl_->ready || !impl_->vg || text.empty()) return;
    const float d = dpi_scale > 0.0f ? dpi_scale : 1.0f;
    const int pt_w = static_cast<int>(static_cast<float>(fb_w) / d + 0.5f);
    const int pt_h = static_cast<int>(static_cast<float>(fb_h) / d + 0.5f);
    if (pt_w <= 0 || pt_h <= 0) return;

    const std::string s{text};
    const auto bounds = debug_overlay_bounds(pt_w, pt_h, corner);
    const float x = static_cast<float>(bounds.x);
    const float y = static_cast<float>(bounds.y);
    const float box_w = static_cast<float>(bounds.w);
    const float box_h = static_cast<float>(bounds.h);

    auto* vg = impl_->vg;
    nvgBeginFrame(vg, static_cast<float>(pt_w), static_cast<float>(pt_h), d);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, box_w, box_h, 6.0f);
    nvgFillColor(vg, nvgRGBA(10, 12, 18, 210));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 42));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    nvgFontFace(vg, "sans");
    nvgFontSize(vg, 12.0f);
    nvgFillColor(vg, nvgRGBA(236, 241, 248, 245));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgTextBox(vg, x + 8.0f, y + 7.0f, box_w - 16.0f,
               s.c_str(), nullptr);
    if (!frame_ms.empty()) {
        constexpr float graph_x = 8.0f;
        constexpr float graph_y = 93.0f;
        const float graph_w = box_w - 16.0f;
        constexpr float graph_h = 14.0f;
        nvgBeginPath(vg);
        nvgRect(vg, x + graph_x, y + graph_y, graph_w, graph_h);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 18));
        nvgFill(vg);

        const float step = graph_w / std::max(1.0f, static_cast<float>(frame_ms.size() - 1));
        nvgBeginPath(vg);
        for (std::size_t i = 0; i < frame_ms.size(); ++i) {
            const float clamped = std::min(frame_ms[i], 50.0f);
            const float px = x + graph_x + static_cast<float>(i) * step;
            const float py = y + graph_y + graph_h -
                (clamped / 50.0f) * graph_h;
            if (i == 0) {
                nvgMoveTo(vg, px, py);
            } else {
                nvgLineTo(vg, px, py);
            }
        }
        nvgStrokeColor(vg, nvgRGBA(103, 232, 249, 230));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }
    nvgEndFrame(vg);
}

#endif  // AFFINEUI_STUB_BUILD

}  // namespace affineui
