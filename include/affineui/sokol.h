#pragma once

// Sokol adapter — opt-in.
//
// Include via the umbrella header by defining the macro first:
//
//     #define AFFINEUI_WITH_SOKOL
//     #include <affineui/affineui.h>
//
// Or include this header directly *after* including sokol_app.h:
//
//     #include <sokol_app.h>
//     #include <affineui/sokol.h>
//
// What's in here:
//   • affineui::sokol::dispatch(Ui&, sapp_event*)
//       Translate a sokol_app event into affineui::Event and route it
//       through the Ui's hit-test + click-handler pipeline. Sets the
//       OS cursor synchronously inside the mouse event so Cocoa
//       honors it. Returns true if the UI consumed the event (you
//       should skip your game's handling of it in that case).
//
//   • affineui::sokol::render(Ui&)
//       Render the UI into the current sokol_gfx pass, querying
//       framebuffer dimensions + DPI from sokol_app itself. This is the
//       low-level helper for mixed renderers that already own the pass.
//
//   • affineui::sokol::render_frame(Ui&)
//       Render the UI into sokol_app's swapchain with AffineUI owning the
//       pass. This path can use retained root-layer caching.
//
//   • affineui::sokol::wire(sapp_desc&, Ui&)
//       Wire frame_userdata_cb / event_userdata_cb / cleanup_userdata_cb
//       to default implementations that forward to the Ui. Use when
//       your app is *just* AffineUI (a tool / settings dialog / pure-
//       UI game) — for mixed games, write your own callbacks and call
//       dispatch/render explicitly.
//
// Threading: same as Ui — same thread that owns the GL context.

#include "affineui/ui.h"

#include <sokol_gfx.h>
#include <sokol_app.h>
#include <sokol_glue.h>
#include <sokol_log.h>

#include <cstdio>
#include <algorithm>
#include <array>
#include <filesystem>
#include <ctime>
#include <cstdint>
#include <span>
#include <string>
#include <system_error>
#include <vector>
#if defined(_WIN32)
#    if !defined(WIN32_LEAN_AND_MEAN)
#        define WIN32_LEAN_AND_MEAN
#    endif
#    if !defined(NOMINMAX)
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace affineui::sokol {

// Sokol swapchains are commonly double- or triple-buffered. Whenever the
// retained AffineUI layer changes, the compositor must present that newest
// layer into every swap image that can later come back around. The public API
// does not currently expose exact backbuffer count, so three frames is the
// conservative desktop default; future backend adapters can replace this with
// explicit swap-image age.
inline constexpr int kSwapchainSettleFrames = 3;

// ── Cursor mapping ──────────────────────────────────────────────────

inline sapp_mouse_cursor cursor_to_sokol(int c) {
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

// ── Event translation + dispatch ────────────────────────────────────

// Map sokol_app's SAPP_KEYCODE_* into our platform-independent
// Key enum. Only the keys AffineUI actually dispatches on are
// listed — everything else falls through to Key::Unknown (the
// caller still gets the raw scancode in Event::key_code if needed).
inline Key key_to_affine(int sapp_keycode) {
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
        default:                     return Key::Unknown;
    }
}

inline std::string utf8_from_codepoint(std::uint32_t cp) {
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

/// Translate a sokol_app event to affineui::Event. Mouse coords are
/// converted from framebuffer pixels (sokol's units) to CSS points
/// (Ui's units) using `sapp_dpi_scale()`.
inline Event translate(const sapp_event* ev) {
    Event out{};
    if (!ev) return out;
    switch (ev->type) {
        case SAPP_EVENTTYPE_MOUSE_MOVE:   out.type = EventType::MouseMove;  break;
        case SAPP_EVENTTYPE_MOUSE_DOWN:   out.type = EventType::MouseDown;  break;
        case SAPP_EVENTTYPE_MOUSE_UP:     out.type = EventType::MouseUp;    break;
        case SAPP_EVENTTYPE_MOUSE_SCROLL: out.type = EventType::MouseWheel; break;
        case SAPP_EVENTTYPE_MOUSE_LEAVE:
            out.type = EventType::MouseMove;
            out.pos  = Point{-1, -1};
            return out;
        case SAPP_EVENTTYPE_KEY_DOWN:
            out.type     = EventType::KeyDown;
            out.key_code = static_cast<int>(ev->key_code);
            out.key      = key_to_affine(ev->key_code);
            return out;
        case SAPP_EVENTTYPE_KEY_UP:
            out.type     = EventType::KeyUp;
            out.key_code = static_cast<int>(ev->key_code);
            out.key      = key_to_affine(ev->key_code);
            return out;
        case SAPP_EVENTTYPE_CHAR:
            out.type = EventType::TextInput;
            out.text = utf8_from_codepoint(ev->char_code);
            return out;
        case SAPP_EVENTTYPE_RESIZED:
            out.type = EventType::Resize;
            return out;
        default:
            return out;  // type stays None → caller skips
    }
    const float dpi = sapp_dpi_scale();
    const float d   = dpi > 0.0f ? dpi : 1.0f;
    out.pos.x = static_cast<int>(ev->mouse_x / d);
    out.pos.y = static_cast<int>(ev->mouse_y / d);
    if (ev->type == SAPP_EVENTTYPE_MOUSE_DOWN ||
        ev->type == SAPP_EVENTTYPE_MOUSE_UP) {
        switch (ev->mouse_button) {
            case SAPP_MOUSEBUTTON_LEFT:   out.button = MouseButton::Left;   break;
            case SAPP_MOUSEBUTTON_RIGHT:  out.button = MouseButton::Right;  break;
            case SAPP_MOUSEBUTTON_MIDDLE: out.button = MouseButton::Middle; break;
            default: break;
        }
    }
    if (ev->type == SAPP_EVENTTYPE_MOUSE_SCROLL) {
        out.wheel_dx = ev->scroll_x;
        out.wheel_dy = ev->scroll_y;
    }
    return out;
}

/// Forward a sokol_app event to the Ui's dispatch pipeline. Applies
/// the OS cursor synchronously (required on macOS — `[NSCursor set]`
/// must happen inside the mouseMoved: handler). Returns true if the
/// UI handled the event (click hit a registered handler), false
/// otherwise so the caller can route it to game logic.
inline bool dispatch(Ui& ui, const sapp_event* ev) {
    if (!ev) return false;
    const auto e = translate(ev);
    if (e.type == EventType::None) return false;
    const bool consumed = ui.dispatch(e);
    if (e.type == EventType::MouseMove) {
        sapp_set_mouse_cursor(cursor_to_sokol(ui.hovered_cursor()));
    }
    return consumed;
}

// ── Frame ───────────────────────────────────────────────────────────

/// Render the UI into the current pass. Queries dimensions + DPI from
/// sokol_app. Call once per sokol_app frame callback, anywhere inside
/// your `sg_begin_default_pass(...)` ... `sg_end_pass()` bracket.
inline void render(Ui& ui) {
    ui.render(sapp_width(), sapp_height(), sapp_dpi_scale());
}

/// Render the UI into sokol_app's swapchain. Unlike render(), this owns the
/// pass, so the renderer can rasterize dirty layers before compositing.
inline FrameTarget frame_target(bool clear = true, bool commit = true) {
    const sg_swapchain sc = sglue_swapchain();
    FrameTarget target{};
    target.width = sapp_width();
    target.height = sapp_height();
    target.dpi_scale = sapp_dpi_scale();
    target.sample_count = sc.sample_count > 0 ? sc.sample_count : 1;
    target.clear = clear;
    target.commit = commit;
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
    return target;
}

inline void render_frame(Ui& ui, bool clear = true, bool commit = true) {
    ui.render(frame_target(clear, commit));
}

// ── One-call wire-up ────────────────────────────────────────────────

namespace detail {
struct RecordedMouseEvent {
    const char* type{"move"};
    int x{0};
    int y{0};
};

struct PerfHudState {
    Ui*    ui{nullptr};
    bool   enabled{false};
    double accum_s{0.0};
    int    frames{0};
    double ms{0.0};
    double fps{0.0};
    std::array<float, 64> ms_history{};
    int history_head{0};
    int history_count{0};
    char   text[512]{};
    std::uint64_t last_render_epoch{0};
    std::uint64_t last_html_epoch{0};
    std::uint64_t render_count{0};
    std::uint64_t html_count{0};
    int    last_w{-1};
    int    last_h{-1};
    float  last_dpi{-1.0f};
    std::vector<RecordedMouseEvent> mouse_path;
    RecordedMouseEvent              last_mouse_point{};
    bool                            has_last_mouse_point{false};
    std::uint32_t                   recording_serial{0};
    std::filesystem::path           recording_dir;
    int                             settle_frames{0};
};

inline void trim_mouse_path(PerfHudState& state) {
    if (state.mouse_path.size() > 24000) {
        state.mouse_path.erase(state.mouse_path.begin(),
                               state.mouse_path.begin() + 8000);
    }
}

inline void record_mouse_event(PerfHudState& state, const Event& e) {
    if (e.pos.x < 0 || e.pos.y < 0) {
        return;
    }
    const char* type = nullptr;
    if (e.type == EventType::MouseMove) {
        type = "move";
    } else if (e.type == EventType::MouseDown &&
               e.button == MouseButton::Left) {
        type = "down";
    } else if (e.type == EventType::MouseUp &&
               e.button == MouseButton::Left) {
        type = "up";
    } else {
        return;
    }
    constexpr int min_distance_px = 2;
    if (e.type == EventType::MouseMove && state.has_last_mouse_point) {
        const int dx = e.pos.x - state.last_mouse_point.x;
        const int dy = e.pos.y - state.last_mouse_point.y;
        if (dx * dx + dy * dy < min_distance_px * min_distance_px) {
            return;
        }
    }
    state.mouse_path.push_back(RecordedMouseEvent{type, e.pos.x, e.pos.y});
    if (e.type == EventType::MouseMove) {
        state.last_mouse_point = RecordedMouseEvent{"move", e.pos.x, e.pos.y};
        state.has_last_mouse_point = true;
    }
    trim_mouse_path(state);
}

inline std::filesystem::path default_recording_dir() {
#if defined(_WIN32)
    std::array<wchar_t, 32768> exe_path{};
    const unsigned long n = GetModuleFileNameW(
        nullptr, exe_path.data(),
        static_cast<unsigned long>(exe_path.size()));
    if (n > 0 && static_cast<std::size_t>(n) < exe_path.size()) {
        std::filesystem::path exe{exe_path.data()};
        if (exe.has_parent_path()) return exe.parent_path();
    }
#endif
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec && !cwd.empty()) return cwd;
    return std::filesystem::path{"."};
}

inline bool write_mouse_path_file(const PerfHudState& state,
                                  const std::filesystem::path& path) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) {
        std::fprintf(stderr,
                      "[affineui] mouse path recorder: could not open %s\n",
                     path.string().c_str());
        return false;
    }
    std::fprintf(f, "{\n  \"mouse_recording\": [\n");
    for (std::size_t i = 0; i < state.mouse_path.size(); ++i) {
        const auto& ev = state.mouse_path[i];
        std::fprintf(f,
                     "    { \"type\": \"%s\", \"x\": %d, \"y\": %d }%s\n",
                     ev.type, ev.x, ev.y,
                     i + 1 == state.mouse_path.size() ? "" : ",");
    }
    std::fprintf(f,
                 "  ],\n"
                 "  \"step_ms\": 0,\n"
                  "  \"snapshot_prefix\": \"path\"\n"
                  "}\n");
    std::fclose(f);
    return true;
}

inline void save_mouse_path(PerfHudState& state) {
    if (state.mouse_path.empty()) return;
    const std::time_t now = std::time(nullptr);
    char name[96]{};
    std::snprintf(name, sizeof(name), "affineui_mouse_path_%lld_%u.json",
                  static_cast<long long>(now), ++state.recording_serial);
    const auto dir = state.recording_dir.empty()
        ? default_recording_dir()
        : state.recording_dir;
    const auto timestamped = dir / name;
    const auto latest = dir / "affineui_mouse_path_latest.json";
    const bool wrote = write_mouse_path_file(state, timestamped);
    if (wrote) {
        write_mouse_path_file(state, latest);
        std::fprintf(stderr,
                     "[affineui] mouse path recorder: wrote %s (%zu points)\n",
                     timestamped.string().c_str(), state.mouse_path.size());
    }
    state.mouse_path.clear();
    state.has_last_mouse_point = false;
}

inline void cb_init_(void* user) {
    (void)user;
    // Bring up sokol_gfx against the swapchain sokol_app created. NanoVG
    // resources init lazily on the first render() (which needs sg valid).
    sg_desc d{};
    d.environment = sglue_environment();
    d.logger.func = slog_func;
    sg_setup(&d);
}
inline void cb_frame_(void* user) {
    auto& state = *static_cast<PerfHudState*>(user);
    auto& ui = *state.ui;
    const int   w   = sapp_width();
    const int   h   = sapp_height();
    const float dpi = sapp_dpi_scale();
    const bool viewport_changed =
        w != state.last_w || h != state.last_h || dpi != state.last_dpi;
    const bool ui_requested_update = ui.needs_update();
    if (ui_requested_update || viewport_changed) {
        // Sokol apps commonly run with two or three swapchain images. A
        // single composite after a state change leaves older backbuffers with
        // stale pixels, which can surface as one-frame holes or flashes when
        // the app idles. Composite the retained root layer a few more times,
        // then go fully quiet again.
        state.settle_frames =
            std::max(state.settle_frames, kSwapchainSettleFrames);
    }
    if (!state.enabled && !ui_requested_update && !viewport_changed &&
        state.settle_frames <= 0) {
        return;
    }
    state.last_w = w;
    state.last_h = h;
    state.last_dpi = dpi;

    if (state.enabled) {
        const double dt = sapp_frame_duration_unfiltered();
        if (dt > 0.0) {
            state.ms_history[static_cast<std::size_t>(state.history_head)] =
                static_cast<float>(dt * 1000.0);
            state.history_head =
                (state.history_head + 1) %
                static_cast<int>(state.ms_history.size());
            state.history_count = std::min(
                state.history_count + 1,
                static_cast<int>(state.ms_history.size()));
            state.accum_s += dt;
            state.frames += 1;
            if (state.accum_s >= 0.5) {
                state.fps = static_cast<double>(state.frames) / state.accum_s;
                state.ms = 1000.0 / std::max(1.0, state.fps);
                const auto& stats = state.ui->renderer().stats();
                std::snprintf(state.text, sizeof(state.text),
                              "%.1f ms/frame  %.1f fps\nrender %llu  html %llu\nDL rec %llu chg %llu same %llu\nroot rast %llu part %llu comp %llu dir %llu\nwork prep %.2f layout %.2f dl %.2f\nrast %.2f comp %.2f ms layer %ux%u%s\nops %u culled %u rects %u dirty %u.%02u%%\nflags %c%c%c%c%c%c%c",
                              state.ms, state.fps,
                              static_cast<unsigned long long>(state.render_count),
                              static_cast<unsigned long long>(state.html_count),
                              static_cast<unsigned long long>(stats.display_list_records),
                              static_cast<unsigned long long>(stats.display_list_changes),
                              static_cast<unsigned long long>(stats.display_list_unchanged),
                              static_cast<unsigned long long>(stats.root_layer_rasterizes),
                              static_cast<unsigned long long>(stats.root_layer_partial_rasterizes),
                              static_cast<unsigned long long>(stats.root_layer_composites),
                              static_cast<unsigned long long>(stats.root_layer_direct_composites),
                              static_cast<double>(stats.prepare_us_this_frame) / 1000.0,
                              static_cast<double>(stats.layout_us_this_frame) / 1000.0,
                              static_cast<double>(stats.display_list_record_us_this_frame) / 1000.0,
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
                state.accum_s = 0.0;
                state.frames = 0;
            }
        }
    }

    std::array<float, 64> ordered{};
    int ordered_count = 0;
    if (state.enabled) {
        ordered_count = state.history_count;
        for (int i = 0; i < ordered_count; ++i) {
            const int src =
                (state.history_head - ordered_count + i +
                 static_cast<int>(state.ms_history.size())) %
                static_cast<int>(state.ms_history.size());
            ordered[static_cast<std::size_t>(i)] =
                state.ms_history[static_cast<std::size_t>(src)];
        }
    }
    auto target = affineui::sokol::frame_target(true, true);
    if (state.enabled) {
        target.debug_overlay_text = state.text;
        target.debug_overlay_frame_ms = ordered.data();
        target.debug_overlay_frame_ms_count =
            static_cast<std::size_t>(ordered_count);
    }
    ui.render(target);
    const auto& stats = ui.renderer().stats();
    const std::uint32_t render_work_us =
        stats.prepare_us_this_frame +
        stats.raster_us_this_frame +
        stats.composite_us_this_frame;
    if (state.enabled && stats.viewport_changed &&
        render_work_us >= 8000u) {
        std::fprintf(stderr,
            "[affineui][resize] %dx%d dpi %.2f total %.2f ms "
            "prep %.2f layout %.2f dl %.2f raster %.2f comp %.2f "
            "ops %u diff %u/%u old %d,%d %dx%d new %d,%d %dx%d "
            "largest %u %d,%d %dx%d "
            "culled %u dirty %.2u.%02u%% layer %ux%u live %ux%u%s "
            "flags %c%c%c%c%c%c%c\n",
            w, h, static_cast<double>(dpi),
            static_cast<double>(render_work_us) / 1000.0,
            static_cast<double>(stats.prepare_us_this_frame) / 1000.0,
            static_cast<double>(stats.layout_us_this_frame) / 1000.0,
            static_cast<double>(stats.display_list_record_us_this_frame) / 1000.0,
            static_cast<double>(stats.raster_us_this_frame) / 1000.0,
            static_cast<double>(stats.composite_us_this_frame) / 1000.0,
            stats.cached_ops,
            stats.display_list_diff_changed_ops,
            static_cast<unsigned>(stats.display_list_diff_first_kind),
            stats.display_list_diff_first_old_bounds.x,
            stats.display_list_diff_first_old_bounds.y,
            stats.display_list_diff_first_old_bounds.w,
            stats.display_list_diff_first_old_bounds.h,
            stats.display_list_diff_first_new_bounds.x,
            stats.display_list_diff_first_new_bounds.y,
            stats.display_list_diff_first_new_bounds.w,
            stats.display_list_diff_first_new_bounds.h,
            static_cast<unsigned>(stats.display_list_diff_largest_kind),
            stats.display_list_diff_largest_bounds.x,
            stats.display_list_diff_largest_bounds.y,
            stats.display_list_diff_largest_bounds.w,
            stats.display_list_diff_largest_bounds.h,
            stats.display_list_ops_culled_this_frame,
            stats.dirty_area_pct_x100 / 100,
            stats.dirty_area_pct_x100 % 100,
            stats.root_layer_capacity_w,
            stats.root_layer_capacity_h,
            stats.root_layer_content_w,
            stats.root_layer_content_h,
            stats.root_layer_allocated_this_frame ? " alloc" : "",
            stats.recorded_this_frame ? 'R' : '-',
            stats.display_list_changed_this_frame ? 'D' : '-',
            stats.root_layer_partial_this_frame ? 'p' : '-',
            stats.root_layer_direct_this_frame ? 'q' : '-',
            stats.layout_dirty ? 'L' : '-',
            stats.paint_dirty ? 'P' : '-',
            stats.animations_active ? 'A' : '-');
    }
    if (state.settle_frames > 0 && !ui.needs_update()) {
        --state.settle_frames;
    }
    if (state.enabled) {
        const auto render_epoch = ui.render_epoch();
        const auto html_epoch = ui.html_epoch();
        state.render_count += render_epoch - state.last_render_epoch;
        state.html_count += html_epoch - state.last_html_epoch;
        state.last_render_epoch = render_epoch;
        state.last_html_epoch = html_epoch;
    }
}
inline void cb_event_(const sapp_event* ev, void* user) {
    auto& state = *static_cast<PerfHudState*>(user);
    auto& ui = *state.ui;
    if (ev && ev->type == SAPP_EVENTTYPE_KEY_DOWN &&
        ev->key_code == SAPP_KEYCODE_R) {
        save_mouse_path(state);
        return;
    }
    const auto e = affineui::sokol::translate(ev);
    record_mouse_event(state, e);
    if (e.type == EventType::None) return;
    const bool consumed = ui.dispatch(e);
    (void)consumed;
    if (e.type == EventType::MouseMove) {
        sapp_set_mouse_cursor(cursor_to_sokol(ui.hovered_cursor()));
    }
}
inline void cb_cleanup_(void* user) {
    auto* state = static_cast<PerfHudState*>(user);
    auto& ui = *state->ui;
    save_mouse_path(*state);
    ui.renderer().shutdown();
    sg_shutdown();
    delete state;
}
}  // namespace detail

/// Configure `desc` so sokol_app drives the given Ui automatically:
/// frame + event + cleanup callbacks installed, `user_data` set to
/// `&ui`. After this, call `sapp_run(&desc)`.
///
/// Useful for "the app IS the UI" (tools, settings panels, debug
/// menus). For mixed games (game + UI overlay), write your own
/// callbacks and call `render(ui)` / `dispatch(ui, ev)` explicitly
/// from inside them.
inline void wire(sapp_desc& desc, Ui& ui, bool perf_hud = false) {
    auto* state = new detail::PerfHudState{};
    state->ui = &ui;
    state->enabled = perf_hud;
    state->recording_dir = detail::default_recording_dir();
    desc.user_data           = state;
    desc.init_userdata_cb    = detail::cb_init_;
    desc.frame_userdata_cb   = detail::cb_frame_;
    desc.event_userdata_cb   = detail::cb_event_;
    desc.cleanup_userdata_cb = detail::cb_cleanup_;
    // Request a GL 4.1 core context on the GL backend (ignored by D3D11/
    // Metal). sokol defaults to GL 4.3 on Linux, which fails to create a
    // context on drivers that cap lower — e.g. WSLg's Mesa/D3D12 (GL 4.2).
    // 4.1 is sokol's own macOS default and is plenty for sokol_gfx + our
    // shaders. Respect an explicit caller override.
    if (desc.gl.major_version == 0) {
        desc.gl.major_version = 4;
        desc.gl.minor_version = 1;
    }
}

}  // namespace affineui::sokol
