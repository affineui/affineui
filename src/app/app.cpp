// affineui::App — convenience wrapper that drives sokol_app's frame
// loop and hands off frame rendering to affineui::Renderer.
//
// What lives here:
//   • sokol_app sapp_desc construction
//   • sokol_app callback wiring (init/frame/event/cleanup)
//   • cursor-keyword mapping + sapp_set_mouse_cursor calls
//   • event translation from sapp_event → affineui::Event
//
// What used to live here but moved to Renderer:
//   • NanoVG context lifecycle
//   • FBO / compositor / display-list paint pipeline
//   • All raw GL state
//
// In stub mode (no deps fetched), App::run is a no-op so tests still
// pass without a windowing system.

#include "affineui/app.h"

#include "affineui/document.h"
#include "affineui/renderer.h"
#include "affineui/themes.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <utility>

#if !defined(AFFINEUI_STUB_BUILD)
#    include "sokol_gfx.h"
#    include "sokol_app.h"
#    include "sokol_glue.h"
#    include "sokol_log.h"
#endif

namespace affineui {

namespace detail {

struct AppImpl {
    App::Config           config;
    Document              document;
    Renderer              renderer;
    std::function<void()> view_fn;
    bool                  quit_requested{false};
    int                   exit_code{0};
    int                   last_cursor{-1};  // last sapp cursor we set
    bool                  dirty{true};
    bool                  animations_active{false};
    int                   last_w{-1};
    int                   last_h{-1};
    float                 last_dpi{1.0f};   // updated each frame for event→pt conversion
};

}  // namespace detail

App::App() : App(Config{}) {}

App::App(Config cfg) : impl_{std::make_unique<detail::AppImpl>()} {
    impl_->config = std::move(cfg);
    impl_->renderer.set_clear_color(impl_->config.clear_color);
    if (impl_->config.resource_loader) {
        impl_->document.set_resource_loader(impl_->config.resource_loader);
    }
    // User-agent baseline so unstyled docs pick up sensible defaults.
    impl_->document.set_user_stylesheet(theme::ua_default());
}

App::~App() = default;
App::App(App&&) noexcept            = default;
App& App::operator=(App&&) noexcept = default;

void App::load_html(std::string_view html)     { impl_->document.set_html(html); impl_->dirty = true; impl_->animations_active = false; }
void App::load_view(const View& view)          { load_html(view.to_html_document()); }
bool App::load_html_file(std::string_view)     { return false; }
void App::set_stylesheet(std::string_view css) { impl_->document.set_user_stylesheet(css); impl_->dirty = true; impl_->animations_active = false; }
void App::mount(std::function<void()> view_fn) { impl_->view_fn = std::move(view_fn); impl_->dirty = true; impl_->animations_active = false; }
void App::invalidate() { impl_->dirty = true; impl_->animations_active = false; }

#if defined(AFFINEUI_STUB_BUILD)

int App::run() { return impl_->exit_code; }

#else  // Real sokol_app loop.

namespace {

// Map our Document::hovered_cursor() int onto a sokol cursor enum.
// Keep the mapping in one place so the order in document.h stays
// load-bearing.
sapp_mouse_cursor map_cursor(int c) {
    switch (c) {
        case 1: return SAPP_MOUSECURSOR_POINTING_HAND;
        case 2: return SAPP_MOUSECURSOR_IBEAM;
        case 3: return SAPP_MOUSECURSOR_CROSSHAIR;
        case 4: return SAPP_MOUSECURSOR_RESIZE_ALL;
        case 5: return SAPP_MOUSECURSOR_NOT_ALLOWED;
        case 6: return SAPP_MOUSECURSOR_RESIZE_EW;
        case 7: return SAPP_MOUSECURSOR_RESIZE_NS;
        default: return SAPP_MOUSECURSOR_DEFAULT;
    }
}

// sokol_app's *_userdata_cb hooks each receive the void* we set on
// sapp_desc.user_data. We stash the AppImpl pointer there so each
// callback recovers state with one cast.

void cb_init(void* user) {
    auto* impl = static_cast<detail::AppImpl*>(user);
    // Bring up sokol_gfx against the swapchain sokol_app just created.
    sg_desc sgd{};
    sgd.environment = sglue_environment();
    sgd.logger.func = slog_func;
    sg_setup(&sgd);
    if (!sg_isvalid()) {
        impl->exit_code = 1;
        sapp_request_quit();
        return;
    }
    // NanoVG-on-sokol_gfx resources. (The renderer also inits lazily on
    // the first render(), but doing it here surfaces failures early.)
    impl->renderer.init_gl();
    if (!impl->renderer.ready()) {
        impl->exit_code = 1;
        sapp_request_quit();
    }
}

void cb_frame(void* user) {
    auto* impl = static_cast<detail::AppImpl*>(user);
    impl->last_dpi = sapp_dpi_scale();
    const int w = sapp_width();
    const int h = sapp_height();
    const bool viewport_changed =
        w != impl->last_w || h != impl->last_h;
    if (!impl->dirty && !impl->animations_active && !viewport_changed) {
        if (impl->quit_requested) sapp_request_quit();
        return;
    }
    impl->last_w = w;
    impl->last_h = h;

    const sg_swapchain sc = sglue_swapchain();
    FrameTarget target{};
    target.width = w;
    target.height = h;
    target.dpi_scale = impl->last_dpi;
    target.sample_count = sc.sample_count > 0 ? sc.sample_count : 1;
    target.clear = true;
    target.commit = true;
    target.metal.current_drawable = sc.metal.current_drawable;
    target.metal.depth_stencil_texture = sc.metal.depth_stencil_texture;
    target.metal.msaa_color_texture = sc.metal.msaa_color_texture;
    target.d3d11.render_view = sc.d3d11.render_view;
    target.d3d11.resolve_view = sc.d3d11.resolve_view;
    target.d3d11.depth_stencil_view = sc.d3d11.depth_stencil_view;
    target.wgpu.render_view = sc.wgpu.render_view;
    target.wgpu.resolve_view = sc.wgpu.resolve_view;
    target.wgpu.depth_stencil_view = sc.wgpu.depth_stencil_view;
    target.gl.framebuffer = sc.gl.framebuffer;
    impl->renderer.render_to(impl->document, target);
    impl->animations_active = impl->renderer.stats().animations_active;
    impl->dirty = impl->animations_active;

    if (impl->quit_requested) sapp_request_quit();
}

void cb_cleanup(void* user) {
    auto* impl = static_cast<detail::AppImpl*>(user);
    impl->renderer.shutdown();
    sg_shutdown();
}

// Translate a sokol_app event to our `affineui::Event` and forward to
// Document::dispatch. Coordinate conversion: sokol_app gives mouse
// coords in framebuffer pixels; layout works in CSS points.
void cb_event(const sapp_event* ev, void* user) {
    auto* impl = static_cast<detail::AppImpl*>(user);
    if (!ev) return;

    Event aui_ev{};
    switch (ev->type) {
        case SAPP_EVENTTYPE_MOUSE_MOVE:  aui_ev.type = EventType::MouseMove; break;
        case SAPP_EVENTTYPE_MOUSE_DOWN:  aui_ev.type = EventType::MouseDown; break;
        case SAPP_EVENTTYPE_MOUSE_UP:    aui_ev.type = EventType::MouseUp;   break;
        case SAPP_EVENTTYPE_RESIZED:
            aui_ev.type = EventType::Resize;
            impl->document.dispatch(aui_ev);
            impl->dirty = true;
            return;
        case SAPP_EVENTTYPE_MOUSE_LEAVE:
            aui_ev.type = EventType::MouseMove;
            aui_ev.pos  = Point{-1, -1};
            if (impl->document.dispatch(aui_ev).redraw_requested) {
                impl->dirty = true;
            }
            return;
        default:
            return;
    }

    const float dpi = impl->last_dpi > 0.0f ? impl->last_dpi : 1.0f;
    aui_ev.pos.x = static_cast<int>(ev->mouse_x / dpi);
    aui_ev.pos.y = static_cast<int>(ev->mouse_y / dpi);
    if (ev->type == SAPP_EVENTTYPE_MOUSE_DOWN ||
        ev->type == SAPP_EVENTTYPE_MOUSE_UP) {
        switch (ev->mouse_button) {
            case SAPP_MOUSEBUTTON_LEFT:   aui_ev.button = MouseButton::Left;   break;
            case SAPP_MOUSEBUTTON_RIGHT:  aui_ev.button = MouseButton::Right;  break;
            case SAPP_MOUSEBUTTON_MIDDLE: aui_ev.button = MouseButton::Middle; break;
            default: break;
        }
    }

    const auto result = impl->document.dispatch(aui_ev);
    if (result.redraw_requested || result.invalidate_view) {
        impl->dirty = true;
    }

    // Cursor must be applied synchronously inside the macOS mouse-
    // event handler — calling sapp_set_mouse_cursor later from the
    // frame callback misses Cocoa's cursor refresh window for this
    // event. sokol's own current_cursor cache makes repeated calls a
    // no-op so this is cheap.
    if (aui_ev.type == EventType::MouseMove) {
        const int cur = impl->document.hovered_cursor();
        sapp_set_mouse_cursor(map_cursor(cur));
        impl->last_cursor = cur;
    }
}

}  // namespace

int App::run() {
    sapp_desc desc{};
    desc.user_data            = impl_.get();
    desc.init_userdata_cb     = cb_init;
    desc.frame_userdata_cb    = cb_frame;
    desc.cleanup_userdata_cb  = cb_cleanup;
    desc.event_userdata_cb    = cb_event;
    desc.width                = impl_->config.width;
    desc.height               = impl_->config.height;
    desc.window_title         = impl_->config.title.c_str();
    desc.high_dpi             = impl_->config.high_dpi;
    desc.swap_interval        = impl_->config.vsync ? 1 : 0;
    desc.sample_count         = 1;
    // GL 4.1 core on the GL backend (ignored by D3D11/Metal). sokol's Linux
    // default of GL 4.3 fails to create a context on drivers that cap lower
    // (e.g. WSLg's Mesa/D3D12 at GL 4.2); 4.1 is enough for sokol_gfx + our
    // shaders. See affineui::sokol::wire() for the examples' path.
    desc.gl.major_version     = 4;
    desc.gl.minor_version     = 1;
    desc.logger.func          = slog_func;
    sapp_run(&desc);
    return impl_->exit_code;
}

#endif  // AFFINEUI_STUB_BUILD

int App::run(std::function<void()> view_fn) {
    mount(std::move(view_fn));
    return run();
}

void App::quit(int code) {
    impl_->quit_requested = true;
    impl_->exit_code      = code;
#if !defined(AFFINEUI_STUB_BUILD)
    sapp_request_quit();
#endif
}

Document&       App::document()       { return impl_->document; }
const Document& App::document() const { return impl_->document; }

Size App::window_size() const {
#if defined(AFFINEUI_STUB_BUILD)
    return {impl_->config.width, impl_->config.height};
#else
    return {sapp_width(), sapp_height()};
#endif
}

float App::dpi_scale() const {
#if defined(AFFINEUI_STUB_BUILD)
    return 1.0f;
#else
    return sapp_dpi_scale();
#endif
}

}  // namespace affineui
