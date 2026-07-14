#pragma once

#include "affineui/document.h"
#include "affineui/embed.h"
#include "affineui/menu.h"
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

    std::weak_ptr<detail::AppImpl> impl_{};
    friend class App;
};

/// How the window's title bar is drawn. Named as in Electron's
/// `titleBarStyle` (plus Frameless for its `frame: false`), so the vocabulary
/// carries over. See App::Config::titlebar.
enum class TitleBarStyle {
    /// The OS draws its title bar and window buttons; the app draws below it.
    Default,
    /// No system title bar — the content fills the window and the app draws
    /// its own bar. The OS window buttons (macOS traffic lights) are still
    /// shown and still work; move them with Config::traffic_light_position.
    Hidden,
    /// As Hidden, with the macOS traffic lights inset further from the corner.
    HiddenInset,
    /// No system title bar AND no system window buttons: the app draws the
    /// close/minimize/maximize controls itself and drives them with
    /// App::close() / minimize() / toggle_maximize(). Electron's `frame:false`.
    /// Don't pick this unless you are actually drawing the buttons — otherwise
    /// the window cannot be closed except with Cmd-Q.
    Frameless,
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
        // ─── Platform chrome ──────────────────────────────────────────
        // Native application menus. ON by default: on macOS every real app is
        // expected to own the system menu bar (the bar lives at the top of the
        // SCREEN — an app cannot draw it), and without a menu bar there is no
        // Quit item, which means Cmd-Q does not work at all. So the default
        // has to be native.
        //
        // Set false to opt out: no NSMenu is installed and the app keeps the
        // in-window bar it draws itself (View::menu_bar). Apps that want their
        // menus inside a custom title bar on every platform want this.
        //
        // No effect off macOS today — the drawn bar is still the only bar
        // there — but the menu model set via set_menu() is platform-neutral, so
        // this flag is where a native Win32/GTK bar would hang later.
        bool        native_menus{true};

        // Window chrome, named as in Electron's `titleBarStyle` so the
        // vocabulary carries over.
        //
        //   Default    — the OS draws its title bar; the app draws underneath.
        //   Hidden     — no system title bar. The content view fills the whole
        //                window and the app draws its own bar. On macOS the
        //                traffic lights still float over the content (moveable
        //                via traffic_light_position); the window is still
        //                resizable from its edges.
        //   HiddenInset— as Hidden, with the macOS traffic lights inset a
        //                little further from the corner.
        //
        // A Hidden window needs the app to mark its own drag region, or the
        // window cannot be moved: any element carrying the CSS declaration
        // `-affineui-app-region: drag` behaves like a title bar (click-drag
        // moves the window, double-click zooms). Interactive children inside it
        // — buttons, menu triggers — must opt back out with `no-drag`. This
        // mirrors Electron's `-webkit-app-region`.
        TitleBarStyle titlebar{TitleBarStyle::Default};

        // macOS only: where the traffic lights sit, in logical points from the
        // window's top-left. Zero means "platform default for the chosen
        // titlebar style". Ignored when titlebar is Default.
        Point       traffic_light_position{};

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

    /// Request the loop to exit cleanly after the current frame. This is the
    /// app's own decision, so it does NOT run the close-request handler —
    /// quit() means quit.
    void quit(int code = 0);

    // ─── Close requests ───────────────────────────────────────────────

    /// Install the handler for an inbound close request — the window's close
    /// button, Cmd-Q / Alt-F4, the menu's Quit item, a system logout, a
    /// Close control the app drew itself. Return false to CANCEL the close
    /// (the Electron `win.on('close', e => e.preventDefault())` shape).
    ///
    /// This is the one veto point, so it is also the one place an editor gets
    /// to say "you have unsaved changes":
    ///
    ///     app.on_close_request([&] {
    ///         if (!doc.dirty()) return true;
    ///         prompt_save();      // async: cancel now, quit() when resolved
    ///         return false;
    ///     });
    ///
    /// With no handler installed a close request always proceeds. The handler
    /// runs on the UI thread, before any teardown; App::quit() bypasses it.
    void on_close_request(std::function<bool()> cb);

    // ─── Menus ────────────────────────────────────────────────────────

    /// Install (or replace) the application menu. Safe to call at any time,
    /// including from a menu callback — a menu that shows checked/enabled
    /// state is expected to be rebuilt and re-set as that state changes, the
    /// same way the view is.
    ///
    /// On macOS this becomes the system menu bar (NSApp.mainMenu). It is
    /// ignored when Config::native_menus is false, and off macOS today, where
    /// the drawn View::menu_bar remains the only bar.
    ///
    /// Supplying a menu is also what tells the drawn menubar that its menu
    /// TRIGGERS have moved to the system bar and should stop drawing (the rest
    /// of the bar — brand, status, document_title — keeps drawing). An app that
    /// never calls this keeps its drawn menus, so adopting native menus is
    /// opt-in and nothing silently loses its menus.
    ///
    /// Order does not matter: the drawn bar always emits its triggers and the
    /// stylesheet hides them, so this works before or after the view is built.
    void set_menu(Menu menu);

    /// The menu last passed to set_menu().
    [[nodiscard]] const Menu& menu() const noexcept;

    // ─── Window controls ──────────────────────────────────────────────
    // The operations a title bar's buttons perform. An app running with
    // TitleBarStyle::Frameless draws its own close/minimize/maximize and wires
    // them to these; they are the Electron `win.close()/minimize()/maximize()`
    // set, and they work regardless of who painted the button.

    /// Ask to close the window. Runs the close-request handler first, so this
    /// is cancellable — an app-drawn close button gets save-on-exit for free.
    /// (App::quit() is the uncancellable form.)
    void close();

    void minimize();
    /// Toggle zoomed/maximized, as the OS's own button does.
    void toggle_maximize();
    [[nodiscard]] bool is_maximized() const;

    void set_fullscreen(bool on);
    [[nodiscard]] bool is_fullscreen() const;

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
