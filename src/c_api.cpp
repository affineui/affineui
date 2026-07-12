// Thin extern "C" surface mirroring the C++ API.
//
// Lets bindings from Rust / Zig / Odin / Python ctypes drive AffineUI
// without a C++ ABI dance. Functions are named affineui_<verb_object>;
// opaque handle types are `affineui_<type>*`.

#include "affineui/affineui.h"
#include "affineui/c_api.h"
#include "affineui/embed.h"
#include "affineui/tools.h"
#include "affineui/ui.h"

#include "c_api_util.h"

#include <string_view>
#include <utility>

namespace {

affineui::Backend to_cpp_backend(affineui_backend b) {
    switch (b) {
        case AFFINEUI_GPU_METAL: return affineui::Backend::metal;
        case AFFINEUI_GPU_GL:    return affineui::Backend::gl;
        case AFFINEUI_GPU_WGPU:  return affineui::Backend::wgpu;
        case AFFINEUI_GPU_D3D11:
        default:                 return affineui::Backend::d3d11;
    }
}

affineui::PixelFormat to_cpp_format(int f) {
    switch (f) {
        case AFFINEUI_FORMAT_RGBA8:         return affineui::PixelFormat::rgba8;
        case AFFINEUI_FORMAT_BGRA8:         return affineui::PixelFormat::bgra8;
        case AFFINEUI_FORMAT_DEPTH:         return affineui::PixelFormat::depth;
        case AFFINEUI_FORMAT_DEPTH_STENCIL: return affineui::PixelFormat::depth_stencil;
        case AFFINEUI_FORMAT_DEFAULT:
        default:                            return affineui::PixelFormat::default_;
    }
}

// The C log callback can't bind directly to the C++ LogFn (enum types
// differ), so stash it and route through a static thunk.
affineui_log_fn g_c_log      = nullptr;
void*           g_c_log_user = nullptr;

void cpp_log_thunk(affineui::LogLevel level, const char* msg, void* /*user*/) {
    if (g_c_log) {
        g_c_log(static_cast<affineui_log_level>(static_cast<int>(level)), msg, g_c_log_user);
    }
}

}  // namespace

extern "C" {

int affineui_tools_listen(int port) {
    return affineui::tools_listen(port) ? 1 : 0;
}

int affineui_tools_active() {
    return affineui::tools_active() ? 1 : 0;
}

int affineui_tools_port() {
    return affineui::tools_port();
}

void affineui_tools_shutdown() {
    affineui::tools_shutdown();
}

int affineui_c_abi_version() {
    return AFFINEUI_C_ABI_VERSION;
}

const char* affineui_version() {
    return AFFINEUI_VERSION_STRING;
}

affineui_ui* affineui_ui_create(void) {
    return reinterpret_cast<affineui_ui*>(new affineui::Ui());
}

void affineui_ui_destroy(affineui_ui* ui) {
    delete reinterpret_cast<affineui::Ui*>(ui);
}

void affineui_ui_init(affineui_ui* ui, const affineui_init_desc* desc) {
    if (!ui || !desc) return;
    auto& cpp = *reinterpret_cast<affineui::Ui*>(ui);

    affineui::GpuContext gpu{};
    affineui::InitDesc init{};
    if (desc->gpu) {
        const auto& g = *desc->gpu;
        gpu.backend                = to_cpp_backend(g.backend);
        gpu.d3d11.device           = g.d3d11_device;
        gpu.d3d11.device_context   = g.d3d11_device_context;
        gpu.metal.device           = g.metal_device;
        gpu.wgpu.device            = g.wgpu_device;
        gpu.color_format           = to_cpp_format(g.color_format);
        gpu.depth_format           = to_cpp_format(g.depth_format);
        gpu.sample_count           = g.sample_count > 0 ? g.sample_count : 1;
        init.gpu = &gpu;
    }
    // affineui_allocator and affineui::Allocator are layout-compatible POD;
    // init() copies it synchronously so the borrow is safe.
    if (desc->allocator) {
        init.allocator = reinterpret_cast<const affineui::Allocator*>(desc->allocator);
    }
    if (desc->log) {
        g_c_log      = desc->log;
        g_c_log_user = desc->log_user;
        init.log     = cpp_log_thunk;
    }
    if (desc->default_font_family) init.default_font_family = desc->default_font_family;
    if (desc->default_font_size > 0) init.default_font_size = desc->default_font_size;

    cpp.init(init);  // gpu/allocator point at locals; init() consumes them synchronously
}

void affineui_ui_set_html(affineui_ui* ui, const char* html) {
    if (!ui || !html) return;
    reinterpret_cast<affineui::Ui*>(ui)->html(std::string_view{html});
}

void affineui_ui_set_css(affineui_ui* ui, const char* css) {
    if (!ui || !css) return;
    reinterpret_cast<affineui::Ui*>(ui)->css(std::string_view{css});
}

void affineui_ui_set_clear_color(affineui_ui* ui,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!ui) return;
    reinterpret_cast<affineui::Ui*>(ui)->set_clear_color(affineui::Color{r, g, b, a});
}

void affineui_ui_render(affineui_ui* ui, const affineui_frame_target* t) {
    if (!ui || !t) return;
    affineui::FrameTarget target{};
    target.width        = t->width;
    target.height       = t->height;
    target.dpi_scale    = t->dpi_scale > 0.0f ? t->dpi_scale : 1.0f;
    target.sample_count = t->sample_count > 0 ? t->sample_count : 1;
    target.clear        = t->clear != 0;
    target.d3d11.render_view        = t->d3d11_render_view;
    target.d3d11.resolve_view       = t->d3d11_resolve_view;
    target.d3d11.depth_stencil_view = t->d3d11_depth_stencil_view;
    target.metal.current_drawable      = t->metal_current_drawable;
    target.metal.depth_stencil_texture = t->metal_depth_stencil_texture;
    target.metal.msaa_color_texture    = t->metal_msaa_color_texture;
    target.wgpu.render_view        = t->wgpu_render_view;
    target.wgpu.resolve_view       = t->wgpu_resolve_view;
    target.wgpu.depth_stencil_view = t->wgpu_depth_stencil_view;
    target.gl.framebuffer          = t->gl_framebuffer;
    target.viewport.x = t->viewport_x;
    target.viewport.y = t->viewport_y;
    target.viewport.w = t->viewport_w;
    target.viewport.h = t->viewport_h;
    reinterpret_cast<affineui::Ui*>(ui)->render(target);
}

int affineui_ui_needs_update(const affineui_ui* ui) {
    if (!ui) return 0;
    return reinterpret_cast<const affineui::Ui*>(ui)->needs_update() ? 1 : 0;
}

void affineui_ui_mark_dirty(affineui_ui* ui) {
    if (!ui) return;
    reinterpret_cast<affineui::Ui*>(ui)->mark_dirty();
}

void affineui_ui_reset(affineui_ui* ui) {
    if (!ui) return;
    reinterpret_cast<affineui::Ui*>(ui)->reset();
}

int affineui_run_html(const char* html) {
    if (!html) return 1;
    return ::affineui::run(std::string_view{html});
}

// ── Input / live mutation (embedded) ─────────────────────────────────

int affineui_ui_load_file(affineui_ui* ui, const char* path) {
    if (!ui || !path) return 0;
    return reinterpret_cast<affineui::Ui*>(ui)->load(std::string_view{path}) ? 1 : 0;
}

int affineui_ui_dispatch(affineui_ui* ui, const affineui_event* ev) {
    if (!ui || !ev) return 0;
    return reinterpret_cast<affineui::Ui*>(ui)->dispatch(affineui_c::to_event(*ev)) ? 1 : 0;
}

void affineui_ui_on_event_capture(affineui_ui* ui,
                                  affineui_event_capture_fn fn,
                                  void* user,
                                  affineui_user_free_fn user_free) {
    if (!ui || !fn) {
        if (user_free) user_free(user);
        return;
    }
    auto data = affineui_c::hold_user(user, user_free);
    reinterpret_cast<affineui::Ui*>(ui)->on_event_capture(
        [fn, data = std::move(data)](
            const affineui::Event& ev,
            const std::vector<affineui::Document::HoverInfo>&) {
            const affineui_event c_ev = affineui_c::to_c_event(ev);
            return fn(data->user, &c_ev) != 0;
        });
}

void affineui_ui_on_click(affineui_ui* ui,
                          const char* selector,
                          affineui_ui_click_fn fn,
                          void* user,
                          affineui_user_free_fn user_free) {
    if (!ui || !selector || !fn) {
        // Honor the exactly-once user_free contract even on a rejected
        // registration, so bindings never leak their closure state.
        if (user_free) user_free(user);
        return;
    }
    auto data = affineui_c::hold_user(user, user_free);
    reinterpret_cast<affineui::Ui*>(ui)->on_click(
        std::string_view{selector},
        [fn, data = std::move(data)] { fn(data->user); });
}

int affineui_ui_hovered_cursor(const affineui_ui* ui) {
    if (!ui) return 0;
    return reinterpret_cast<const affineui::Ui*>(ui)->hovered_cursor();
}

int affineui_ui_text_input_active(const affineui_ui* ui) {
    if (!ui) return 0;
    return reinterpret_cast<const affineui::Ui*>(ui)->text_input_active() ? 1 : 0;
}

void affineui_ui_caret_rect(const affineui_ui* ui,
                            int* out_x, int* out_y,
                            int* out_w, int* out_h) {
    affineui::Rect r{};
    if (ui) r = reinterpret_cast<const affineui::Ui*>(ui)->caret_rect();
    if (out_x) *out_x = r.x;
    if (out_y) *out_y = r.y;
    if (out_w) *out_w = r.w;
    if (out_h) *out_h = r.h;
}

void affineui_ui_set_caret_blink_interval(affineui_ui* ui,
                                          double milliseconds) {
    if (!ui) return;
    reinterpret_cast<affineui::Ui*>(ui)->document()
        .set_caret_blink_interval(milliseconds);
}

double affineui_ui_caret_blink_interval(const affineui_ui* ui) {
    if (!ui) return 0.0;
    return reinterpret_cast<const affineui::Ui*>(ui)->document()
        .caret_blink_interval();
}

int affineui_ui_set_attr(affineui_ui* ui, const char* elem_id,
                         const char* name, const char* value) {
    if (!ui || !elem_id || !name) return 0;
    return reinterpret_cast<affineui::Ui*>(ui)->set_attr(
               std::string_view{elem_id}, std::string_view{name},
               affineui_c::sv(value))
               ? 1
               : 0;
}

int affineui_ui_remove_attr(affineui_ui* ui, const char* elem_id, const char* name) {
    if (!ui || !elem_id || !name) return 0;
    return reinterpret_cast<affineui::Ui*>(ui)->remove_attr(
               std::string_view{elem_id}, std::string_view{name})
               ? 1
               : 0;
}

int affineui_ui_set_text(affineui_ui* ui, const char* elem_id, const char* text) {
    if (!ui || !elem_id) return 0;
    return reinterpret_cast<affineui::Ui*>(ui)->set_text(std::string_view{elem_id},
                                                         affineui_c::sv(text))
               ? 1
               : 0;
}

void affineui_ui_content_size(const affineui_ui* ui, int* out_w, int* out_h) {
    affineui::Size size{};
    if (ui) size = reinterpret_cast<const affineui::Ui*>(ui)->content_size();
    if (out_w) *out_w = size.width;
    if (out_h) *out_h = size.height;
}

void affineui_string_free(char* s) {
    std::free(s);
}

}  // extern "C"
