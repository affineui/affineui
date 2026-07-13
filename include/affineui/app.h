#pragma once

#include "affineui/document.h"
#include "affineui/embed.h"
#include "affineui/telemetry.h"
#include "affineui/types.h"
#include "affineui/image.h"
#include "affineui/view.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace affineui {

namespace detail {
struct AppImpl;
}

/// Invalidating, non-owning access to the small set of App operations that
/// retained helpers need. It never exposes App, Document, Renderer, or Painter
/// pointers; every operation safely no-ops after the App is destroyed.
class AppHandle {
public:
    AppHandle() noexcept = default;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return is_valid(); }

    void set_custom_paint(std::string_view name,
                          Document::CustomPaintFn fn) const;
    bool request_custom_repaint(std::string_view name) const;
    [[nodiscard]] Rect find_element_rect(std::string_view target) const;
    [[nodiscard]] ImageHandle create_image_rgba(
        int width,
        int height,
        std::span<const std::uint8_t> rgba) const;
    void capture_pointer() const;
    void release_pointer() const;

private:
    explicit AppHandle(std::weak_ptr<detail::AppImpl> impl) noexcept
        : impl_(std::move(impl)) {}

    [[nodiscard]] std::shared_ptr<detail::AppImpl> lock_impl() const noexcept;

    std::weak_ptr<detail::AppImpl> impl_{};
    friend class App;
};

class App {
public:
    struct Config {
        std::string title{"AffineUI"};
        int         width{1024};
        int         height{768};
        Color       clear_color{30, 30, 46, 255};
        bool        high_dpi{true};
        bool        vsync{true};
        std::string default_font_family{"sans-serif"};
        int         default_font_size{16};
        std::vector<std::string> asset_folders{"."};
        ResourceLoader resource_loader{};
        bool        perf_overlay{false};
        DebugOverlayCorner perf_overlay_corner{DebugOverlayCorner::top_right};
        // Chrome-DevTools-style hotkey: F12 (or Ctrl+Shift+I) starts the
        // tools server if needed and opens the affinetools viewer attached
        // to this process. Zero configuration, on by default in every
        // App-based app; set false to veto (e.g. an app that binds F12).
        // Compiled out entirely with AFFINEUI_PERF=0.
        bool        devtools_hotkey{true};
        // Called after an interaction changed the dock layout (e.g. a splitter
        // drag). The app reads document().dock_pane_sizes() and persists them.
        std::function<void()> on_layout_changed{};
        // Runtime opt-out for the compile-time bundled Decius resources.
        // The bundle is ON by default (this flag is false); set it to
        // true to disable — no auto-applied stylesheet, no fallback-to-
        // embedded on `frameworks/*` URLs — even when the bundle is
        // compiled in. Use this to run entirely against your own on-disk
        // assets without any implicit bundled fallback. Ignored (no-op)
        // when the bundle wasn't compiled in (AFFINEUI_NO_BUNDLE_DECIUS
        // at build time). Same semantic as the macro, at runtime.
        bool        no_bundle_decius{false};
    };

    App();
    explicit App(Config cfg);
    ~App();

    App(const App&)            = delete;
    App& operator=(const App&) = delete;
    App(App&&) noexcept;
    App& operator=(App&&) noexcept;

    /// Load an HTML string. Retained-mode entry point.
    void load_html(std::string_view html);

    /// Load a command-tree view by inflating it into the retained HTML/CSS
    /// document. Full HTML serialize + reparse — fine for one-shot loads;
    /// apps that rebuild on state changes should use set_view() instead.
    void load_view(const View& view);

    /// Install the app's declarative view builder and build it once. The
    /// App owns a persistent View; every rebuild_view() re-runs the
    /// builder into it and RECONCILES the result into the live document:
    /// attribute/text diffs ride the cheap paint-only mutation path,
    /// structural edits settle with one scoped restyle, and no HTML is
    /// ever reparsed after the initial shell load. This is the fast path
    /// for apps that rebuild the view per state change (or per frame).
    void set_view(std::function<void(View&)> builder);

    /// Re-run the installed view builder and reconcile the result into
    /// the live document. No-op until set_view() has been called.
    void rebuild_view();

    /// Load HTML from a file (resolved via resource loader if absolute
    /// scheme; otherwise interpreted relative to CWD).
    bool load_html_file(std::string_view path);

    /// Install or replace the user stylesheet (sits above document
    /// stylesheets in the cascade as `author` origin).
    void set_stylesheet(std::string_view css);

    /// As above, with the stylesheet's base URL so its relative url()s (icon
    /// fonts, images) resolve against the location it was loaded from — the
    /// same resolution a <link>ed sheet gets. Use with read_framework_bundle's
    /// out_base_url.
    void set_stylesheet(std::string_view css, std::string_view base_url);

    /// Immediate-mode entry point. The view function is invoked when
    /// AffineUI knows the UI definition could have changed (a state
    /// hook mutated, an event handler returned, or
    /// `imm::invalidate()` was called) — NOT every frame. Painting
    /// runs every frame off the retained DOM regardless.
    void mount(std::function<void()> view_fn);

    /// Force the imm view function to be re-evaluated before the next
    /// paint. Same effect as calling `imm::invalidate()` from inside
    /// the view fn.
    void invalidate();

    /// Register a custom paint (canvas) handler — see
    /// Document::set_custom_paint. Elements opt in with
    /// `data-aui-paint="name"` (View::canvas emits one); the handler
    /// draws immediate-mode vector content into the active Painter each
    /// time the element paints. An empty fn removes the registration.
    void set_custom_paint(std::string_view name, Document::CustomPaintFn fn);

    /// Repaint every `data-aui-paint="name"` element on the next frame —
    /// the cheap path for per-frame animated geometry (drag previews,
    /// meters): no restyle, no layout, no reconcile.
    void request_custom_repaint(std::string_view name);

    /// Create a renderer-owned dynamic RGBA8 image. Safe handles invalidate
    /// on reset or renderer shutdown and never retain a Painter.
    [[nodiscard]] ImageHandle create_image_rgba(
        int width,
        int height,
        std::span<const std::uint8_t> rgba);

    [[nodiscard]] AppHandle handle() const noexcept { return AppHandle{impl_}; }

    /// Programmatically set the value a named widget displays, in place
    /// (no view rebuild) and without echoing an on_change — the
    /// write-back half of a data binding, e.g. an inspector tracking a
    /// 3D gizmo drag. See Document::set_widget_value.
    bool set_widget_value(std::string_view name, std::string_view value);

    /// Enable/disable the native performance overlay. The overlay is drawn
    /// outside the document tree so it never affects layout.
    void set_perf_overlay_enabled(bool enabled);
    [[nodiscard]] bool perf_overlay_enabled() const noexcept;
    void set_perf_overlay_corner(DebugOverlayCorner corner);

    /// The most recent presented frame's telemetry record: wall-clock frame
    /// gap (the truth metric for stalls), stage timings, op/dirty counters,
    /// and allocator deltas. The same record the AFFINEUI_TELEMETRY JSONL
    /// sink streams (schema: docs/AFFINETOOLS_PROTOCOL.md). Zeroed until the
    /// first frame presents, and always zeroed when the library is compiled
    /// with AFFINEUI_PERF=0.
    [[nodiscard]] const FrameTelemetry& frame_telemetry() const noexcept;

    /// Dispatch a translated input event through the loaded view/document.
    /// Returns true when the event was consumed by a command callback or the
    /// document requested a redraw.
    bool dispatch(const Event& ev);

    using EventHandler = std::function<bool(
        const Event&, const std::vector<Document::HoverInfo>&)>;

    /// Register a capture-phase handler that runs before the document and its
    /// focused widget. Returning true consumes the event. This is the explicit
    /// override point for app-global command systems; ordinary on_event
    /// handlers run after document/widget dispatch and do not see commands a
    /// focused editor consumed.
    void on_event_capture(EventHandler cb);

    /// Register a low-level native event handler (same contract as
    /// Ui::on_event): the handler receives the already-hit-tested hover
    /// chain, deepest first, after CSS hover/active state updates.
    /// Native widget kits use this to implement pointer-drag behaviors
    /// (knob scrubs, patch-cable drags) against their own state.
    void on_event(EventHandler cb);

    /// Register a per-frame tick callback, invoked once per host frame
    /// with the elapsed seconds since the previous frame — the
    /// requestAnimationFrame analog for native animations/physics.
    /// The callback runs before the idle short-circuit; call
    /// invalidate() from inside it to schedule a repaint, or do
    /// nothing to stay idle-cheap.
    void on_frame(std::function<void(double dt_seconds)> cb);

    /// Capture pointer moves for a native event handler. While
    /// captured, MouseMove events are offered to on_event handlers
    /// before DOM hover hit-testing (drag controls update without
    /// thrashing unrelated :hover state).
    void capture_pointer();
    void release_pointer();
    bool pointer_captured() const;

    /// Start the main loop. Returns the OS exit code. Standalone apps
    /// call this; embedders instead drive should_render()/render() from
    /// their own pulse (see below).
    int run();

    /// Convenience: install a view fn and run() in one call.
    int run(std::function<void()> view_fn);

    /// Request the loop to exit cleanly after the current frame.
    void quit(int code = 0);

    // ─── Embed API ────────────────────────────────────────────────────
    // For hosts that own their own pulse (game engine, editor host, etc.)
    // and want AffineUI to react rather than run its own main loop.
    //
    // Every host pulse should:
    //   for (const Event& ev : host_input_this_pulse)
    //       app.dispatch(ev);                 // input entry point (above)
    //   if (app.should_render()) app.render();
    //
    // Input is not coalesced by the library — pass every event the host
    // has; handlers see them all. Render is gated by
    // (dirty || animations_active) AND min_frame_time_ms elapsed since
    // the last render, so a host pulse that ticks faster than the paint
    // budget skips the paint transparently.
    //
    // Resize: should_render() detects DOM/animation dirtiness but does
    // NOT probe the host swapchain for a viewport change (it doesn't
    // have one). When the host resizes its target, either dispatch()
    // a Resize event or call invalidate() before the next pulse — the
    // internal cb_frame path handles viewport-change detection because
    // it owns the sokol swapchain; hosts must signal it explicitly.

    /// Minimum wall-clock time between renders, in milliseconds. Zero
    /// means "paint every time should_render() would otherwise be true"
    /// (no throttle). Typical embed value is one display refresh (16.7
    /// for 60 Hz, 8.3 for 120 Hz). Default is 0.
    void set_min_frame_time(double ms);
    [[nodiscard]] double min_frame_time() const noexcept;

    /// True iff a paint is needed (document dirty or animations running)
    /// AND at least min_frame_time_ms has elapsed since the last render.
    /// Cheap; call each pulse. NOTE: does not detect host-swapchain
    /// resizes — signal those via dispatch(Resize) or invalidate().
    [[nodiscard]] bool should_render();

    /// Paint one frame to the host swapchain. Currently a stub while the
    /// cb_frame body is being extracted; standalone run() is unaffected.
    /// When the extraction lands, this will advance the min-frame-time
    /// gate on real paint (not on the call itself).
    void render();

    /// The underlying retained document. Lives as long as the App.
    Document&       document();
    const Document& document() const;

    /// Current window size in logical pixels.
    Size window_size() const;

    /// Current drawable framebuffer size in physical pixels.
    Size framebuffer_size() const;

    /// Current display scale factor (1.0 = 1x, 2.0 = Retina-class).
    float dpi_scale() const;

private:
    std::shared_ptr<detail::AppImpl> impl_;
};

}  // namespace affineui
