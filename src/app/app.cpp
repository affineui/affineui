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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#if !defined(AFFINEUI_STUB_BUILD)
#    include "sokol_gfx.h"
#    include "sokol_app.h"
#    include "sokol_glue.h"
#    include "sokol_log.h"
#endif

namespace affineui {

namespace detail {

// Sokol does not expose exact swap-image age to App today. Use the common
// triple-buffered desktop budget so a newly composited retained layer reaches
// every backbuffer before the app goes back to zero-idle work.
constexpr int kSwapchainSettleFrames = 3;

struct AppImpl {
    App::Config           config;
    Document              document;
    Renderer              renderer;
    std::function<void()> view_fn;
    std::vector<WidgetClickBinding> view_click_bindings;
    std::vector<WidgetChangeBinding> view_change_bindings;
    bool                  quit_requested{false};
    int                   exit_code{0};
    int                   last_cursor{-1};  // last sapp cursor we set
    bool                  dirty{true};
    bool                  animations_active{false};
    int                   last_w{-1};
    int                   last_h{-1};
    float                 last_dpi{1.0f};   // updated each frame for event→pt conversion
    int                   settle_frames{0};
    bool                  perf_overlay_enabled{false};
    DebugOverlayCorner    perf_overlay_corner{DebugOverlayCorner::top_right};
    double                perf_accum_s{0.0};
    int                   perf_frames{0};
    double                perf_ms{0.0};
    double                perf_fps{0.0};
    std::array<float, 64> perf_ms_history{};
    int                   perf_history_head{0};
    int                   perf_history_count{0};
    char                  perf_text[384]{};
};

bool local_asset_url(std::string_view url) {
    return !url.empty() &&
           url.find("://") == std::string_view::npos &&
           url.rfind("data:", 0) != 0;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.good()) return {};

    std::stringstream bytes;
    bytes << file.rdbuf();
    return bytes.str();
}

bool safe_relative_asset_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    const auto normalized = path.lexically_normal();
    for (const auto& part : normalized) {
        if (part == "..") return false;
    }
    return true;
}

ResourceLoader make_asset_resource_loader(std::vector<std::string> folders) {
    if (folders.empty()) folders.push_back(".");
    std::vector<std::filesystem::path> roots;
    roots.reserve(folders.size());
    for (const auto& folder : folders) {
        roots.emplace_back(folder);
    }

    return [roots](std::string_view url) -> std::string {
        if (!local_asset_url(url)) return {};

        const std::filesystem::path rel =
            std::filesystem::path{std::string(url)}.lexically_normal();
        if (!safe_relative_asset_path(rel)) return {};
        for (const auto& root : roots) {
            if (auto bytes = read_file(root / rel); !bytes.empty()) {
                return bytes;
            }
        }
        return {};
    };
}

bool dispatch_loaded_view_event(AppImpl& impl, const Event& ev) {
    if (ev.type == EventType::Resize) {
        impl.dirty = true;
    }

    const auto result = impl.document.dispatch(ev);
    if (result.redraw_requested || result.invalidate_view) {
        impl.dirty = true;
    }
    if (result.layout_changed && impl.config.on_layout_changed) {
        try {
            impl.config.on_layout_changed();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "AffineUI on_layout_changed failed: %s\n",
                         e.what());
        } catch (...) {
            std::fprintf(stderr, "AffineUI on_layout_changed failed\n");
        }
    }

    bool consumed = result.redraw_requested || result.invalidate_view;
    if (ev.type == EventType::MouseUp && ev.button == MouseButton::Left &&
        !impl.view_click_bindings.empty()) {
        const auto activations = impl.document.take_activated_widgets();
        std::vector<std::function<void()>> callbacks;
        for (const auto& name : activations) {
            for (const auto& binding : impl.view_click_bindings) {
                if (binding.name == name && binding.handler) {
                    callbacks.push_back(binding.handler);
                }
            }
        }
        for (const auto& cb : callbacks) {
            try {
                cb();
                consumed = true;
                impl.dirty = true;
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                             "AffineUI click callback failed: %s\n",
                             e.what());
            } catch (...) {
                std::fprintf(stderr,
                             "AffineUI click callback failed\n");
            }
        }
    } else if (ev.type == EventType::MouseUp) {
        (void) impl.document.take_activated_widgets();
    }

    const auto changes = impl.document.take_widget_changes();
    if (!changes.empty()) {
        struct PendingChange {
            std::function<void(std::string_view)> handler;
            std::string value;
        };
        std::vector<PendingChange> callbacks;
        for (const auto& change : changes) {
            for (const auto& binding : impl.view_change_bindings) {
                if (binding.name == change.name && binding.handler) {
                    callbacks.push_back({binding.handler, change.value});
                }
            }
        }
        for (const auto& cb : callbacks) {
            try {
                cb.handler(cb.value);
                consumed = true;
                impl.dirty = true;
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                             "AffineUI change callback failed: %s\n",
                             e.what());
            } catch (...) {
                std::fprintf(stderr,
                             "AffineUI change callback failed\n");
            }
        }
    }

    return consumed;
}

void log_event_loop_exception(const char* where, const std::exception& e) {
    std::fprintf(stderr, "AffineUI %s failed: %s\n", where, e.what());
}

void log_event_loop_exception(const char* where) {
    std::fprintf(stderr, "AffineUI %s failed\n", where);
}

}  // namespace detail

App::App() : App(Config{}) {}

App::App(Config cfg) : impl_{std::make_unique<detail::AppImpl>()} {
    impl_->config = std::move(cfg);
    impl_->perf_overlay_enabled = impl_->config.perf_overlay;
    // Env override: AFFINEUI_PERF_OVERLAY=1 turns on the perf/DPI HUD without a
    // code change — it prints "fb WxH css WxH dpi N.NN", handy for confirming
    // the live DPI scale (should read your display scale, e.g. 1.25, not 1.00).
    if (const char* e = std::getenv("AFFINEUI_PERF_OVERLAY");
        e != nullptr && e[0] != '\0' && e[0] != '0') {
        impl_->perf_overlay_enabled = true;
    }
    impl_->perf_overlay_corner = impl_->config.perf_overlay_corner;
    impl_->renderer.set_clear_color(impl_->config.clear_color);
    impl_->document.set_resource_loader(
        impl_->config.resource_loader
            ? impl_->config.resource_loader
            : detail::make_asset_resource_loader(impl_->config.asset_folders));
#if !defined(AFFINEUI_STUB_BUILD)
    impl_->document.set_clipboard(
        []() -> std::string {
            const char* text = sapp_get_clipboard_string();
            return text ? std::string(text) : std::string{};
        },
        [](std::string_view text) {
            const std::string owned{text};
            sapp_set_clipboard_string(owned.c_str());
        });
#endif
    // User-agent baseline so unstyled docs pick up sensible defaults.
    impl_->document.set_user_stylesheet(theme::ua_default());
}

App::~App() = default;
App::App(App&&) noexcept            = default;
App& App::operator=(App&&) noexcept = default;

void App::load_html(std::string_view html) {
    impl_->view_click_bindings.clear();
    impl_->view_change_bindings.clear();
    impl_->document.clear_scripts();
    impl_->document.set_html(html);
    impl_->dirty = true;
    impl_->animations_active = false;
    impl_->settle_frames = detail::kSwapchainSettleFrames;
}
void App::load_view(const View& view) {
    impl_->config.clear_color = view.background_color();
    impl_->renderer.set_clear_color(impl_->config.clear_color);
    load_html(view.to_html_document());
    impl_->document.attach_script(DocumentScript::UiControls);
    impl_->view_click_bindings = view.click_bindings();
    impl_->view_change_bindings = view.change_bindings();
}
bool App::load_html_file(std::string_view)     { return false; }
void App::set_stylesheet(std::string_view css) { impl_->document.set_user_stylesheet(css); impl_->dirty = true; impl_->animations_active = false; }
void App::set_stylesheet(std::string_view css, std::string_view base_url) { impl_->document.set_user_stylesheet(css, base_url); impl_->dirty = true; impl_->animations_active = false; }
void App::mount(std::function<void()> view_fn) { impl_->view_fn = std::move(view_fn); impl_->dirty = true; impl_->animations_active = false; }
void App::invalidate() { impl_->dirty = true; impl_->animations_active = false; }

void App::set_perf_overlay_enabled(bool enabled) {
    impl_->perf_overlay_enabled = enabled;
    impl_->settle_frames = detail::kSwapchainSettleFrames;
}

bool App::perf_overlay_enabled() const noexcept {
    return impl_->perf_overlay_enabled;
}

void App::set_perf_overlay_corner(DebugOverlayCorner corner) {
    impl_->perf_overlay_corner = corner;
    impl_->settle_frames = detail::kSwapchainSettleFrames;
}

bool App::dispatch(const Event& ev) {
    return detail::dispatch_loaded_view_event(*impl_, ev);
}

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
        case 8: return SAPP_MOUSECURSOR_RESIZE_NWSE;
        case 9: return SAPP_MOUSECURSOR_RESIZE_NESW;
        default: return SAPP_MOUSECURSOR_DEFAULT;
    }
}

Key key_to_affine(int sapp_keycode) {
    switch (sapp_keycode) {
        case SAPP_KEYCODE_ESCAPE:    return Key::Escape;
        case SAPP_KEYCODE_TAB:       return Key::Tab;
        case SAPP_KEYCODE_ENTER:     return Key::Enter;
        case SAPP_KEYCODE_BACKSPACE: return Key::Backspace;
        case SAPP_KEYCODE_DELETE:    return Key::Delete;
        case SAPP_KEYCODE_LEFT:      return Key::ArrowLeft;
        case SAPP_KEYCODE_RIGHT:     return Key::ArrowRight;
        case SAPP_KEYCODE_UP:        return Key::ArrowUp;
        case SAPP_KEYCODE_DOWN:      return Key::ArrowDown;
        case SAPP_KEYCODE_HOME:      return Key::Home;
        case SAPP_KEYCODE_END:       return Key::End;
        default: break;
    }
    // Letters A–Z and digits 0–9 are contiguous in both enums; map by offset.
    if (sapp_keycode >= SAPP_KEYCODE_A && sapp_keycode <= SAPP_KEYCODE_Z) {
        return static_cast<Key>(
            static_cast<int>(Key::A) + (sapp_keycode - SAPP_KEYCODE_A));
    }
    if (sapp_keycode >= SAPP_KEYCODE_0 && sapp_keycode <= SAPP_KEYCODE_9) {
        return static_cast<Key>(
            static_cast<int>(Key::Digit0) + (sapp_keycode - SAPP_KEYCODE_0));
    }
    return Key::Unknown;
}

void apply_modifiers(Event& out, std::uint32_t modifiers) {
    out.shift = (modifiers & SAPP_MODIFIER_SHIFT) != 0;
    out.ctrl  = (modifiers & SAPP_MODIFIER_CTRL) != 0;
    out.alt   = (modifiers & SAPP_MODIFIER_ALT) != 0;
    out.super = (modifiers & SAPP_MODIFIER_SUPER) != 0;
}

std::string utf8_from_codepoint(std::uint32_t cp) {
    std::string out;
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
    return out;
}

DebugOverlayBounds perf_overlay_bounds(int fb_w,
                                       int fb_h,
                                       float dpi_scale,
                                       DebugOverlayCorner corner) {
    const float d = dpi_scale > 0.0f ? dpi_scale : 1.0f;
    const int pt_w = static_cast<int>(static_cast<float>(fb_w) / d + 0.5f);
    const int pt_h = static_cast<int>(static_cast<float>(fb_h) / d + 0.5f);
    return debug_overlay_bounds(pt_w, pt_h, corner);
}

bool point_in_perf_overlay(const detail::AppImpl& impl,
                           int x,
                           int y,
                           int fb_w,
                           int fb_h,
                           float dpi_scale) {
    if (!impl.perf_overlay_enabled) return false;
    const auto b =
        perf_overlay_bounds(fb_w, fb_h, dpi_scale, impl.perf_overlay_corner);
    return debug_overlay_contains(b, x, y);
}

float current_dpi_scale(const detail::AppImpl& impl) {
    const float live_dpi = sapp_dpi_scale();
    if (live_dpi > 0.0f) return live_dpi;
    return impl.last_dpi > 0.0f ? impl.last_dpi : 1.0f;
}

void update_perf_overlay_text(detail::AppImpl& impl) {
    const double dt = sapp_frame_duration_unfiltered();
    if (dt > 0.0) {
        impl.perf_ms_history[static_cast<std::size_t>(impl.perf_history_head)] =
            static_cast<float>(dt * 1000.0);
        impl.perf_history_head =
            (impl.perf_history_head + 1) %
            static_cast<int>(impl.perf_ms_history.size());
        impl.perf_history_count = std::min(
            impl.perf_history_count + 1,
            static_cast<int>(impl.perf_ms_history.size()));
        impl.perf_accum_s += dt;
        impl.perf_frames += 1;
    }
    if (impl.perf_accum_s < 0.5 && impl.perf_text[0] != '\0') return;

    impl.perf_fps = impl.perf_accum_s > 0.0
        ? static_cast<double>(impl.perf_frames) / impl.perf_accum_s
        : 0.0;
    impl.perf_ms = impl.perf_fps > 0.0 ? 1000.0 / impl.perf_fps : 0.0;
    const float dpi = current_dpi_scale(impl);
    const float d = dpi > 0.0f ? dpi : 1.0f;
    const int fb_w = sapp_width();
    const int fb_h = sapp_height();
    const int css_w = static_cast<int>(static_cast<float>(fb_w) / d + 0.5f);
    const int css_h = static_cast<int>(static_cast<float>(fb_h) / d + 0.5f);
    const auto& stats = impl.renderer.stats();
    std::snprintf(impl.perf_text, sizeof(impl.perf_text),
                  "%.1f ms/frame  %.1f fps\n"
                  "fb %dx%d css %dx%d dpi %.2f\n"
                  "work prep %.2f layout %.2f dl %.2f\n"
                  "rast %.2f comp %.2f ms layer %ux%u%s\n"
                  "ops %u culled %u rects %u dirty %u.%02u%%\n"
                  "flags %c%c%c%c%c%c%c",
                  impl.perf_ms,
                  impl.perf_fps,
                  fb_w,
                  fb_h,
                  css_w,
                  css_h,
                  static_cast<double>(dpi),
                  static_cast<double>(stats.prepare_us_this_frame) / 1000.0,
                  static_cast<double>(stats.layout_us_this_frame) / 1000.0,
                  static_cast<double>(
                      stats.display_list_record_us_this_frame) / 1000.0,
                  static_cast<double>(stats.raster_us_this_frame) / 1000.0,
                  static_cast<double>(stats.composite_us_this_frame) / 1000.0,
                  stats.root_layer_capacity_w,
                  stats.root_layer_capacity_h,
                  stats.root_layer_allocated_this_frame ? " alloc" : "",
                  stats.cached_ops,
                  stats.display_list_ops_culled_this_frame,
                  stats.dirty_rects,
                  stats.dirty_area_pct_x100 / 100,
                  stats.dirty_area_pct_x100 % 100,
                  stats.recorded_this_frame ? 'R' : '-',
                  stats.display_list_changed_this_frame ? 'D' : '-',
                  stats.root_layer_partial_this_frame ? 'p' : '-',
                  stats.root_layer_direct_this_frame ? 'q' : '-',
                  stats.layout_dirty ? 'L' : '-',
                  stats.paint_dirty ? 'P' : '-',
                  stats.animations_active ? 'A' : '-');
    impl.perf_accum_s = 0.0;
    impl.perf_frames = 0;
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
    try {
        impl->last_dpi = sapp_dpi_scale();
        const int w = sapp_width();
        const int h = sapp_height();
        const bool viewport_changed =
            w != impl->last_w || h != impl->last_h;
        if (impl->dirty || impl->animations_active || viewport_changed) {
            // Sokol apps normally present through a two or three image swapchain.
            // After one UI change, drawing only the current backbuffer can leave
            // stale pixels in older swap images; those images can flash when the
            // app goes quiet. Keep compositing until the whole swapchain has seen
            // the latest retained root layer, then return to zero-idle work.
            impl->settle_frames =
                std::max(impl->settle_frames, detail::kSwapchainSettleFrames);
        }
        if (!impl->perf_overlay_enabled && !impl->dirty &&
            !impl->animations_active && !viewport_changed &&
            impl->settle_frames <= 0) {
            if (impl->quit_requested) sapp_request_quit();
            return;
        }
        impl->last_w = w;
        impl->last_h = h;
        std::array<float, 64> ordered_perf_ms{};
        int ordered_perf_count = 0;
        if (impl->perf_overlay_enabled) {
            update_perf_overlay_text(*impl);
            ordered_perf_count = impl->perf_history_count;
            for (int i = 0; i < ordered_perf_count; ++i) {
                const int src =
                    (impl->perf_history_head - ordered_perf_count + i +
                     static_cast<int>(impl->perf_ms_history.size())) %
                    static_cast<int>(impl->perf_ms_history.size());
                ordered_perf_ms[static_cast<std::size_t>(i)] =
                    impl->perf_ms_history[static_cast<std::size_t>(src)];
            }
        }

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
        if (impl->perf_overlay_enabled) {
            target.debug_overlay_text = impl->perf_text;
            target.debug_overlay_frame_ms = ordered_perf_ms.data();
            target.debug_overlay_frame_ms_count =
                static_cast<std::size_t>(ordered_perf_count);
            target.debug_overlay_corner = impl->perf_overlay_corner;
        }
        impl->renderer.render_to(impl->document, target);
        impl->animations_active = impl->renderer.stats().animations_active;
        impl->dirty = impl->animations_active;
        if (impl->settle_frames > 0 && !impl->dirty) {
            --impl->settle_frames;
        }

        if (impl->quit_requested) sapp_request_quit();
    } catch (const std::exception& e) {
        detail::log_event_loop_exception("frame callback", e);
        impl->exit_code = 1;
        sapp_request_quit();
    } catch (...) {
        detail::log_event_loop_exception("frame callback");
        impl->exit_code = 1;
        sapp_request_quit();
    }
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
    try {

    Event aui_ev{};
    apply_modifiers(aui_ev, ev->modifiers);
    switch (ev->type) {
        case SAPP_EVENTTYPE_MOUSE_MOVE:  aui_ev.type = EventType::MouseMove; break;
        case SAPP_EVENTTYPE_MOUSE_DOWN:  aui_ev.type = EventType::MouseDown; break;
        case SAPP_EVENTTYPE_MOUSE_UP:    aui_ev.type = EventType::MouseUp;   break;
        case SAPP_EVENTTYPE_MOUSE_SCROLL:
            aui_ev.type = EventType::MouseWheel;
            break;
        case SAPP_EVENTTYPE_KEY_DOWN:
            aui_ev.type     = EventType::KeyDown;
            aui_ev.key_code = static_cast<int>(ev->key_code);
            aui_ev.key      = key_to_affine(ev->key_code);
            (void) detail::dispatch_loaded_view_event(*impl, aui_ev);
            return;
        case SAPP_EVENTTYPE_KEY_UP:
            aui_ev.type     = EventType::KeyUp;
            aui_ev.key_code = static_cast<int>(ev->key_code);
            aui_ev.key      = key_to_affine(ev->key_code);
            (void) detail::dispatch_loaded_view_event(*impl, aui_ev);
            return;
        case SAPP_EVENTTYPE_CHAR:
            aui_ev.type = EventType::TextInput;
            aui_ev.text = utf8_from_codepoint(ev->char_code);
            if (!aui_ev.text.empty()) {
                (void) detail::dispatch_loaded_view_event(*impl, aui_ev);
            }
            return;
        case SAPP_EVENTTYPE_RESIZED:
            aui_ev.type = EventType::Resize;
            detail::dispatch_loaded_view_event(*impl, aui_ev);
            return;
        case SAPP_EVENTTYPE_MOUSE_LEAVE:
            aui_ev.type = EventType::MouseMove;
            aui_ev.pos  = Point{-1, -1};
            (void) detail::dispatch_loaded_view_event(*impl, aui_ev);
            return;
        default:
            return;
    }

    const float dpi = current_dpi_scale(*impl);
    impl->last_dpi = dpi;
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
    if (ev->type == SAPP_EVENTTYPE_MOUSE_SCROLL) {
        aui_ev.wheel_dx = ev->scroll_x;
        aui_ev.wheel_dy = ev->scroll_y;
    }
    const bool over_perf_overlay =
        point_in_perf_overlay(*impl, aui_ev.pos.x, aui_ev.pos.y,
                              sapp_width(), sapp_height(), dpi);
    if (over_perf_overlay) {
        if (aui_ev.type == EventType::MouseMove) {
            sapp_set_mouse_cursor(SAPP_MOUSECURSOR_POINTING_HAND);
            impl->last_cursor = 1;
            return;
        }
        if (aui_ev.type == EventType::MouseDown &&
            aui_ev.button == MouseButton::Left) {
            impl->perf_overlay_corner =
                next_debug_overlay_corner(impl->perf_overlay_corner);
            impl->settle_frames = detail::kSwapchainSettleFrames;
            return;
        }
    }

    const bool consumed = detail::dispatch_loaded_view_event(*impl, aui_ev);
    (void) consumed;

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
    } catch (const std::exception& e) {
        detail::log_event_loop_exception("event callback", e);
        impl->exit_code = 1;
        sapp_request_quit();
    } catch (...) {
        detail::log_event_loop_exception("event callback");
        impl->exit_code = 1;
        sapp_request_quit();
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
    desc.enable_clipboard     = true;
    desc.clipboard_size       = 1024 * 1024;
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
    const float dpi = dpi_scale();
    const float d = dpi > 0.0f ? dpi : 1.0f;
    return {
        static_cast<int>(static_cast<float>(sapp_width()) / d + 0.5f),
        static_cast<int>(static_cast<float>(sapp_height()) / d + 0.5f),
    };
#endif
}

Size App::framebuffer_size() const {
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
