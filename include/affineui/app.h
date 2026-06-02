#pragma once

#include "affineui/document.h"
#include "affineui/embed.h"
#include "affineui/types.h"
#include "affineui/view.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace affineui {

namespace detail {
struct AppImpl;
}

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
    /// document. This is a compatibility bridge while the local DOM sink
    /// grows direct weak-handle mutation support.
    void load_view(const View& view);

    /// Load HTML from a file (resolved via resource loader if absolute
    /// scheme; otherwise interpreted relative to CWD).
    bool load_html_file(std::string_view path);

    /// Install or replace the user stylesheet (sits above document
    /// stylesheets in the cascade as `author` origin).
    void set_stylesheet(std::string_view css);

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

    /// Enable/disable the native performance overlay. The overlay is drawn
    /// outside the document tree so it never affects layout.
    void set_perf_overlay_enabled(bool enabled);
    [[nodiscard]] bool perf_overlay_enabled() const noexcept;
    void set_perf_overlay_corner(DebugOverlayCorner corner);

    /// Dispatch a translated input event through the loaded view/document.
    /// Returns true when the event was consumed by a command callback or the
    /// document requested a redraw.
    bool dispatch(const Event& ev);

    /// Start the main loop. Returns the OS exit code.
    int run();

    /// Convenience: install a view fn and run() in one call.
    int run(std::function<void()> view_fn);

    /// Request the loop to exit cleanly after the current frame.
    void quit(int code = 0);

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
    std::unique_ptr<detail::AppImpl> impl_;
};

}  // namespace affineui
