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
#include "affineui/decius_bundle.h"

#include "affineui/document.h"
#include "affineui/log.h"
#include "affineui/renderer.h"
#include "affineui/themes.h"
#include "affineui/tools.h"
#include "core/diag.h"
#include "core/log_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#if !defined(AFFINEUI_STUB_BUILD)
#    include "sokol_gfx.h"
#    include "sokol_app.h"
#    include "sokol_glue.h"
#    include "sokol_log.h"
#endif

#if defined(__APPLE__) && !defined(AFFINEUI_STUB_BUILD)
// See src/framework/app/app_mac_native.mm. Drains the NSEvent queue in
// place of AppKit's per-run-loop-iteration delivery, so cb_event fires
// for every queued input event on each app pulse instead of one per
// display refresh (which during modal drag stalls mouse-up behind a
// backlog and produces multi-second unresponsive tails).
extern "C" void affineui_mac_drain_native_events(void);
#endif

namespace affineui {

namespace detail {

// Log frame-stamping source (log.cpp installs this via a fn-ptr, which
// can't capture — so cb_frame publishes the current frame + t_ms here).
std::atomic<std::uint64_t> g_log_frame{0};
std::atomic<double>        g_log_t_ms{0.0};
std::pair<std::uint64_t, double> app_log_frame_source() {
    return {g_log_frame.load(std::memory_order_relaxed),
            g_log_t_ms.load(std::memory_order_relaxed)};
}

// Sokol does not expose exact swap-image age to App today. Use the common
// triple-buffered desktop budget so a newly composited retained layer reaches
// every backbuffer before the app goes back to zero-idle work.
constexpr int kSwapchainSettleFrames = 3;

struct AppImpl {
    // Owning App, so the free-function frame loop (which holds only an
    // AppImpl*) can drive App::rebuild_view() — a member with the friend
    // access load_html()/reconcile need. Repointed on App move (see the
    // move ctor/assignment, which are NOT =default for this reason).
    App*                  owner{nullptr};
    App::Config           config;
    Document              document;
    Renderer              renderer;
    std::function<void()> view_fn;
    std::vector<WidgetClickBinding> view_click_bindings;
    std::vector<WidgetChangeBinding> view_change_bindings;
    std::vector<WidgetChangeBinding> view_commit_bindings;
    std::vector<Document::WidgetChange> deferred_widget_changes;
    std::vector<App::EventHandler> event_handlers;
    std::vector<std::function<void(double)>> frame_callbacks;
    std::vector<Document::HoverInfo> hover_chain_scratch;
    // Reconcile fast path (App::set_view / rebuild_view): the app-owned
    // persistent View that rebuilds diff against.
    std::function<void(View&)> view_builder;
    std::unique_ptr<View>      retained_view;
    bool                       view_bootstrapped{false};
    // A scroll re-window recycles rows under a stationary cursor: after the
    // frame-loop reconcile, replay a MouseMove from the last pointer position
    // so :hover tracks what is actually under the cursor (browser semantics).
    bool                       hover_refresh_pending{false};
    int                        last_mouse_x{0};
    int                        last_mouse_y{0};
    bool                       has_last_mouse{false};
    // View diagnostics already printed to stderr (each distinct message once).
    std::set<std::string>      reported_view_diagnostics;
    bool                  pointer_captured{false};
    bool                  quit_requested{false};
    int                   exit_code{0};
    int                   last_cursor{-1};  // last sapp cursor we set
    bool                  last_ime_active{false};  // last IME intent pushed
    Rect                  last_ime_rect{};         // last caret rect pushed
    bool                  dirty{true};
    // View reconcile pending: set by invalidate() when a set_view() builder
    // is installed, so the frame loop re-runs the builder and reconciles the
    // diff into the live document BEFORE painting. Distinct from `dirty`
    // (needs paint): rebuild_view() itself sets `dirty`, so keying the
    // rebuild off `dirty` would loop forever, and animation frames set
    // `dirty` every frame without any state change to reconcile.
    bool                  view_dirty{false};
    bool                  animations_active{false};
    int                   last_w{-1};
    int                   last_h{-1};
    float                 last_dpi{1.0f};   // updated each frame for event→pt conversion
    int                   settle_frames{0};
    // Min-frame-time gate (App::set_min_frame_time). Milliseconds
    // between successive renders; 0 disables (paint whenever should_
    // render() is otherwise true). Applied identically inside cb_frame
    // for standalone run() and directly by embed hosts calling
    // should_render()/render(). last_render_time is set at the top of
    // render(), so the gate measures start-to-start.
    // Default is 0 (no throttle) to match the App::set_min_frame_time
    // public contract; callers who want a cap opt in explicitly.
    double                min_frame_time_ms{0.0};
    std::chrono::steady_clock::time_point last_render_time{};
    bool                  last_render_time_set{false};
    // Input trace counters (AFFINEUI_INPUT_TRACE=1): MOUSE_MOVE events
    // seen + dispatched per frame. With app-level coalescing removed
    // these two are always equal (every event dispatches). Reset at the
    // top of each render(); the count reveals how many events the OS
    // delivered between paints.
    std::uint32_t         moves_received_since_frame{0};
    std::uint32_t         moves_dispatched_since_frame{0};
    // Wall time of the last raw MOUSE_MOVE received in cb_event. Frame
    // trace prints (frame_now - stamp) as "age" so we can see whether
    // events themselves are stale on arrival or fresh but lag downstream.
    std::chrono::steady_clock::time_point last_move_stamp{};
    bool                  has_last_move_stamp{false};
    // Resize coalescing: the win32 sizing modal loop delivers WM_SIZE per
    // mouse move; each Resize dispatch pays a full relayout + text
    // re-measure, so uncoalesced resizes are seconds-slow. One per frame.
    bool                  has_pending_resize{false};
    bool                  perf_overlay_enabled{false};
    DebugOverlayCorner    perf_overlay_corner{DebugOverlayCorner::top_right};
    double                perf_accum_s{0.0};
    int                   perf_frames{0};
    double                perf_ms{0.0};
    double                perf_fps{0.0};
    std::array<float, 64> perf_ms_history{};
    int                   perf_history_head{0};
    int                   perf_history_count{0};
    char                  perf_text[512]{};
    // R0/R1 telemetry (docs/TRACING_AND_PERFORMANCE_LOGGING.md; schema in
    // docs/AFFINETOOLS_PROTOCOL.md). Wall clock is measured at cb_frame
    // ENTRY so stalls anywhere in the pipeline surface as frame gaps.
    FrameTelemetry        telemetry{};
    std::chrono::steady_clock::time_point session_start{};
    std::chrono::steady_clock::time_point last_frame_time{};
    bool                  frame_clock_started{false};
    std::uint64_t         presented_frames{0};
    std::uint64_t         skipped_since_present{0};
    std::uint64_t         skipped_presents_total{0};
    double                last_idle_emit_ms{-1.0e12};
    float                 gap_max_ms_window{0.0f};
    mem::Stats            mem_prev{};
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

ResourceLoader make_asset_resource_loader(std::vector<std::string> folders) {
    if (folders.empty()) folders.push_back(".");
    std::vector<std::filesystem::path> roots;
    roots.reserve(folders.size());
    for (const auto& folder : folders) {
        roots.emplace_back(folder);
    }

    return [roots](std::string_view url) -> std::string {
        if (!local_asset_url(url)) return {};

        // Resolve the url against each asset root and read it. `../` is
        // allowed and NORMAL here — a framework stylesheet in css/ points
        // at ../fonts/ for its icon font, exactly as a browser resolves it
        // relative to the sheet's own location. An absolute url() (from an
        // absolute stylesheet base_url) is read directly. No traversal
        // sandbox: this is a local desktop binary loading its own shipped
        // assets, not a served document. (An earlier blanket "reject any
        // path containing ..'" refused the framework's own font urls and
        // left every icon blank.)
        const std::filesystem::path url_path{std::string(url)};
        if (url_path.is_absolute()) {
            return read_file(url_path.lexically_normal());
        }
        for (const auto& root : roots) {
            const auto resolved = (root / url_path).lexically_normal();
            if (auto bytes = read_file(resolved); !bytes.empty()) {
                return bytes;
            }
        }
        return {};
    };
}

bool dispatch_loaded_view_event(AppImpl& impl, const Event& ev) {
    const auto event_handlers = impl.event_handlers;
    // While a native handler holds pointer capture, MouseMove routes to
    // it before DOM hover hit-testing (same contract as Ui::dispatch) so
    // drags don't thrash unrelated :hover state.
    if (impl.pointer_captured && ev.type == EventType::MouseMove &&
        !event_handlers.empty()) {
        impl.document.hovered_info_chain(impl.hover_chain_scratch);
        bool captured_consumed = false;
        for (const auto& cb : event_handlers) {
            captured_consumed =
                cb(ev, impl.hover_chain_scratch) || captured_consumed;
        }
        if (captured_consumed) {
            impl.dirty = true;
            return true;
        }
    }

    if (ev.type == EventType::Resize) {
        impl.dirty = true;
    }

    DispatchResult result;
    {
        detail::TraceSpan dispatch_span("dispatch");
        result = impl.document.dispatch(ev);
    }
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

    // Native event handlers (widget kits) see the event with the fresh
    // hover chain before view click/change bindings. A consuming handler
    // owns the event outright.
    if (!event_handlers.empty()) {
        impl.document.hovered_info_chain(impl.hover_chain_scratch);
        bool native_consumed = false;
        for (const auto& cb : event_handlers) {
            native_consumed =
                cb(ev, impl.hover_chain_scratch) || native_consumed;
        }
        if (native_consumed) {
            impl.dirty = true;
            if (ev.type == EventType::MouseUp) {
                (void) impl.document.take_activated_widgets();
            }
            return true;
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

    auto changes = impl.document.take_widget_changes();
    if (result.defer_widget_changes) {
        // Deferral coalesces a gesture's rebuild to its end. But LIVE
        // changes are previews (on_change is bound to cheap preview work
        // that must not rebuild) — they have to dispatch every frame or
        // there is no realtime feedback (the colour picker not recolouring
        // the material as you drag). So split: live changes fall through
        // to immediate dispatch below; only committed changes defer.
        std::vector<Document::WidgetChange> live_now;
        std::vector<Document::WidgetChange> deferred;
        for (auto& change : changes) {
            if (change.live) {
                live_now.push_back(std::move(change));
            } else {
                deferred.push_back(std::move(change));
            }
        }
        if (!deferred.empty()) {
            impl.deferred_widget_changes.insert(
                impl.deferred_widget_changes.end(),
                std::make_move_iterator(deferred.begin()),
                std::make_move_iterator(deferred.end()));
            consumed = true;
            impl.dirty = true;
        }
        if (live_now.empty()) return consumed;
        changes = std::move(live_now);  // dispatch these now (on_change only)
    } else if (!impl.deferred_widget_changes.empty()) {
        impl.deferred_widget_changes.insert(
            impl.deferred_widget_changes.end(),
            std::make_move_iterator(changes.begin()),
            std::make_move_iterator(changes.end()));
        changes = std::move(impl.deferred_widget_changes);
        impl.deferred_widget_changes.clear();

        std::vector<Document::WidgetChange> coalesced;
        coalesced.reserve(changes.size());
        for (auto& change : changes) {
            auto it = std::find_if(
                coalesced.begin(), coalesced.end(),
                [&](const Document::WidgetChange& existing) {
                    return existing.name == change.name;
                });
            if (it == coalesced.end()) {
                coalesced.push_back(std::move(change));
            } else {
                // Per widget, the newest value wins; a committed change
                // absorbs the live previews before it (the gesture is
                // over by the time deferred changes flush), so apps see
                // one change AND one commit — never a stale preview.
                it->value = std::move(change.value);
                it->live = it->live && change.live;
            }
        }
        changes = std::move(coalesced);
    }
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
            // Committed changes additionally fire the commit bindings
            // (the end of a gesture / a discrete edit).
            if (!change.live) {
                for (const auto& binding : impl.view_commit_bindings) {
                    if (binding.name == change.name && binding.handler) {
                        callbacks.push_back({binding.handler, change.value});
                    }
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

    // A virtual list that scrolled needs its window re-windowed so the builder
    // reads the new scroll offset back and renders the rows now under the
    // viewport. Only MARK it here (view_dirty — the same coalescing rail as
    // App::invalidate()): wheel/trackpad events arrive far faster than
    // frames, and rebuilding per event (reconcile + settle each time) is what
    // turns smooth native scrolling jerky. The frame pulse reconciles at most
    // once per frame with the latest offsets; the scroll itself was already
    // applied natively, so nothing lags visually.
    if (!impl.document.take_widget_scrolls().empty() && impl.view_builder) {
        impl.view_dirty = true;
        impl.hover_refresh_pending = true;
        consumed = true;
        impl.dirty = true;
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
    // R1 JSONL sink: AFFINEUI_TELEMETRY=<path> streams per-frame records
    // (idempotent; no-op stub when compiled with AFFINEUI_PERF=0).
    telemetry::sink_open_from_env();
    // affinetools attach: AFFINEUI_TOOLS_LISTEN=1|<port> starts the
    // protocol server (docs/AFFINETOOLS_DESIGN.md §3.1; off by default).
    tools_listen_from_env();
    // Frame-stamp diagnostics: every affineui::log line records the
    // presented-frame index current when it was emitted, so the devtools
    // log panel can align a line with the performance graph.
    detail::set_log_frame_source(&detail::app_log_frame_source);
    impl_->renderer.set_clear_color(impl_->config.clear_color);
    {
        // Base loader: the user's explicit resource_loader, or a
        // filesystem-backed one over their asset_folders.
        auto base_loader =
            impl_->config.resource_loader
                ? impl_->config.resource_loader
                : detail::make_asset_resource_loader(impl_->config.asset_folders);
#ifdef AFFINEUI_NO_BUNDLE_DECIUS
        impl_->document.set_resource_loader(std::move(base_loader));
#else
        // Runtime opt-out (Config::no_bundle_decius = true) skips the
        // wrap AND the auto-apply below — matches the macro semantic
        // but at runtime, so the language bindings can turn the embed
        // off per-app.
        if (impl_->config.no_bundle_decius) {
            impl_->document.set_resource_loader(std::move(base_loader));
        } else {
            // Wrap with the compile-time Decius bundle as an INVISIBLE
            // FALLBACK. Every URL request goes through the base loader
            // first — asset_folders / user's own resource_loader always
            // win. Only if the base comes back empty do we serve from
            // the embedded bytes. The moment a matching file is dropped
            // where the base loader can find it, it takes over; the
            // embed is a floor, never a ceiling.
            impl_->document.set_resource_loader(
                [base = std::move(base_loader)](std::string_view url) -> std::string {
                    std::string from_disk = base(url);
                    if (!from_disk.empty()) return from_disk;
                    return affineui::decius::load(url);
                });
        }
#endif
    }
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
#ifndef AFFINEUI_NO_BUNDLE_DECIUS
    // Auto-apply the bundled Decius stylesheet on top of the UA baseline.
    // This is what makes `App(Config{}).load_view(view)` show a styled
    // UI with zero explicit configuration — matches the "just works"
    // contract. Users who provide their own stylesheet later via
    // `App::set_stylesheet(...)` replace this; users who drop a
    // matching CSS file in an asset folder shadow the embedded one
    // via the resource-loader chain above. Gated on the runtime
    // opt-out flag so language bindings can turn it off per-app.
    if (!impl_->config.no_bundle_decius && affineui::decius::available()) {
        impl_->document.set_user_stylesheet(
            affineui::decius::css_bundle(), "frameworks/css/");
    }
#endif
    // Back-pointer for the free-function frame loop (see AppImpl::owner).
    impl_->owner = this;
}

App::~App() = default;
App::App(App&& other) noexcept : impl_{std::move(other.impl_)} {
    // The moved-in impl still carries the source App's owner pointer; repoint
    // it at this App so the frame loop drives the right instance.
    if (impl_) impl_->owner = this;
}
App& App::operator=(App&& other) noexcept {
    impl_ = std::move(other.impl_);
    if (impl_) impl_->owner = this;
    return *this;
}

void App::load_html(std::string_view html) {
    const auto transient = impl_->document.capture_transient_state();
    impl_->deferred_widget_changes.clear();
    impl_->view_click_bindings.clear();
    impl_->view_change_bindings.clear();
    impl_->view_commit_bindings.clear();
    impl_->document.clear_scripts();
    impl_->document.set_html(html);
    impl_->document.restore_transient_state(transient);
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
    impl_->view_commit_bindings = view.commit_bindings();
}

namespace detail {
// Replay an already-built widget subtree through a ViewSink — used by
// the reconcile bootstrap so the document sink learns every node the
// shell load didn't contain. Creates carry attrs/text from the node.
void replay_view_node(ViewSink& sink, const WidgetNode& node,
                      const WidgetNode* parent, std::size_t index) {
    switch (node.kind) {
        case WidgetKind::Text:
            sink.create_text(node, parent, index);
            return;
        case WidgetKind::RawHtml:
            sink.create_raw_html(node, parent, index);
            if (!node.text.empty()) sink.set_raw_html(node, node.text);
            return;
        default:
            break;
    }
    sink.create_element(node, parent, index);
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        replay_view_node(sink, node.children[i], &node, i);
    }
}
}  // namespace detail

void App::set_view(std::function<void(View&)> builder) {
    impl_->view_builder = std::move(builder);
    impl_->view_bootstrapped = false;
    rebuild_view();
}

void App::rebuild_view() {
    if (!impl_->view_builder) return;
    if (!impl_->retained_view) {
        impl_->retained_view = std::make_unique<View>();
    }
    // Dock surgery (tearoff / drag-to-dock) restructured the DOM outside the
    // retained view. Incremental reconcile on top of that leaves the
    // surgery's wrapper elements behind as duplicate panels — resync by
    // re-bootstrapping. The arrangement itself is not lost: the builder's
    // dock_layout provider replays it (the harvest ran on the surgical DOM).
    if (impl_->view_bootstrapped &&
        impl_->document.take_dock_structure_changed()) {
        impl_->view_bootstrapped = false;
    }
    View& view = *impl_->retained_view;

    // Feed each virtual list its container's live scroll geometry so the build
    // renders the window currently under the viewport. Set every rebuild (the
    // View is reused) before the builder runs.
    {
        Document* doc = &impl_->document;
        view.set_scroll_provider(
            [doc](std::string_view name, Axis axis) -> View::ScrollGeometry {
                const auto g = doc->virtual_scroll_geometry(
                    name, axis == Axis::Horizontal);
                return {g.offset, g.viewport, g.known};
            });
    }

    if (!impl_->view_bootstrapped) {
        // Bootstrap: build without a sink, load only the document SHELL
        // (head/styles/body attrs, empty <main>), then replay the built
        // tree through the document sink so it owns every node mapping.
        view.begin(static_cast<ViewSink*>(nullptr));
        try {
            impl_->view_builder(view);
        } catch (...) {
            // The builder may have opened its own nested reconcile session
            // before throwing. Collapse it so this owner-level end() fully
            // settles and tears down the bootstrap pass.
            view.collapse_reconcile_nesting();
            view.end();
            throw;
        }
        view.end();
        impl_->config.clear_color = view.background_color();
        impl_->renderer.set_clear_color(impl_->config.clear_color);
        load_html(view.to_html_shell());
        impl_->document.attach_script(DocumentScript::UiControls);
        if (auto* sink = impl_->document.begin_view_mutations()) {
            const auto& root = view.root();
            for (std::size_t i = 0; i < root.children.size(); ++i) {
                detail::replay_view_node(*sink, root.children[i], nullptr,
                                         i);
            }
        }
        impl_->document.end_view_mutations();
        impl_->view_bootstrapped = true;
    } else {
        // Steady state: rebuild into the persistent view; only actual
        // differences reach the document (attribute/text mutations on
        // the cheap path; structural edits settle in one restyle).
        auto* sink = impl_->document.begin_view_mutations();
        view.begin(sink);
        bool view_end_attempted = false;
        try {
            impl_->view_builder(view);
            view_end_attempted = true;
            view.end();
        } catch (...) {
            const auto failure = std::current_exception();
            // A builder exception mid-batch must not leave the mutation
            // window open: ev_insert stays suppressed, recorded work never
            // settles, and blocks keep pointing at elements the batch
            // already destroyed — every later layout then walks freed
            // memory (and on win32 the WndProc kernel-callback filter
            // swallows the resulting faults, so the app limps on corrupt).
            // Close the window so the document settles what was applied,
            // then propagate.
            if (!view_end_attempted) {
                view_end_attempted = true;
                // The builder may have thrown between its own begin()/end();
                // collapse that nesting so this end() actually settles the
                // session (removes trailing children, flushes, tears down)
                // rather than merely unwinding one nested level.
                view.collapse_reconcile_nesting();
                try {
                    view.end();
                } catch (const std::exception& e) {
                    std::fprintf(stderr,
                                 "AffineUI view cleanup failed: %s\n",
                                 e.what());
                } catch (...) {
                    std::fprintf(stderr,
                                 "AffineUI view cleanup failed\n");
                }
            }
            impl_->document.end_view_mutations();
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "AffineUI view builder failed: %s\n",
                             e.what());
            } catch (...) {
                std::fprintf(stderr, "AffineUI view builder failed\n");
            }
            std::rethrow_exception(failure);
        }
        // Document-level selector attributes (density / accent / theme) only
        // reach the DOM through the bootstrap shell; a retained-view rebuild
        // must re-stamp them on the live <body> or selector() flips are
        // silent no-ops. Stamped INSIDE the mutation window so the flip and
        // the reconcile settle once, together. No-op when unchanged.
        for (const auto& attr : view.resolved_document_attrs()) {
            impl_->document.set_body_attribute(attr.name, attr.value);
        }
        impl_->document.end_view_mutations();
    }

    // Builder-misuse diagnostics (undeclared dock panels, bad selectors, …)
    // are collected quietly during the build; a silent vector helps nobody,
    // so print each distinct message once per process.
    for (const auto& d : view.diagnostics()) {
        if (impl_->reported_view_diagnostics.insert(d).second) {
            std::fprintf(stderr, "AffineUI view diagnostic: %s\n", d.c_str());
        }
    }
    view.clear_diagnostics();

    impl_->view_click_bindings = view.click_bindings();
    impl_->view_change_bindings = view.change_bindings();
    impl_->view_commit_bindings = view.commit_bindings();
    impl_->dirty = true;
    impl_->animations_active = false;
    // The reconcile has landed; nothing further to rebuild until the next
    // invalidate(). Clearing here also covers the direct App::rebuild_view()
    // and set_view() entry points, so a subsequent frame won't rebuild again.
    impl_->view_dirty = false;
}
bool App::load_html_file(std::string_view)     { return false; }
void App::set_stylesheet(std::string_view css) {
    set_stylesheet(css, {});
}
void App::set_stylesheet(std::string_view css, std::string_view base_url) {
    impl_->document.set_user_stylesheet(css, base_url);
    impl_->dirty = true;
    impl_->animations_active = false;
    // Replacing the stylesheet re-parses the retained document from its
    // stored HTML — for a reconciled view that is the empty bootstrap
    // shell, so rebuild the view world from scratch.
    if (impl_->view_bootstrapped) {
        impl_->view_bootstrapped = false;
        rebuild_view();
    }
}
void App::mount(std::function<void()> view_fn) { impl_->view_fn = std::move(view_fn); impl_->dirty = true; impl_->animations_active = false; }
void App::invalidate() {
    impl_->dirty = true;
    impl_->animations_active = false;
    // When the app is driven by a set_view() builder, an invalidate() means
    // "state changed, re-run the builder and reconcile" — flag it so the
    // frame loop rebuilds before the next paint. Many invalidate() calls in
    // one frame coalesce into a single reconcile. No builder installed =>
    // legacy load_view/load_html path, nothing to rebuild.
    if (impl_->view_builder) impl_->view_dirty = true;
}

void App::set_custom_paint(std::string_view name,
                           Document::CustomPaintFn fn) {
    impl_->document.set_custom_paint(name, std::move(fn));
}

void App::request_custom_repaint(std::string_view name) {
    if (impl_->document.request_custom_repaint(name)) {
        impl_->dirty = true;
    }
}

bool App::set_widget_value(std::string_view name, std::string_view value) {
    const bool changed = impl_->document.set_widget_value(name, value);
    if (changed) impl_->dirty = true;
    return changed;
}

void App::set_min_frame_time(double ms) {
    impl_->min_frame_time_ms = ms < 0.0 ? 0.0 : ms;
}

double App::min_frame_time() const noexcept {
    return impl_->min_frame_time_ms;
}

bool App::should_render() {
    // Dirty conditions (same set the internal frame path checks). The
    // viewport-change condition needs live swapchain size, which we only
    // check inside the render loop; a host that resizes should invalidate()
    // explicitly and the internal check will fire on the next render().
    const bool needs = impl_->dirty || impl_->animations_active ||
                       impl_->settle_frames > 0 ||
                       impl_->perf_overlay_enabled;
    if (!needs) return false;
    if (impl_->min_frame_time_ms <= 0.0) return true;
    if (!impl_->last_render_time_set) return true;
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - impl_->last_render_time).count();
    return elapsed_ms >= impl_->min_frame_time_ms;
}

void App::render() {
    // Embed entry point. Full extraction of cb_frame's body into an
    // App-callable render step is in progress; until it lands, invoking
    // render() from a host that is not driving cb_frame is a no-op.
    // Notably, the min-frame-time gate is NOT advanced here — advancing
    // it without an actual paint would starve subsequent should_render()
    // calls even though nothing was drawn. The gate is advanced only
    // inside cb_frame's real render path (see the last_render_time write
    // there). Standalone run() is unaffected.
}

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

const FrameTelemetry& App::frame_telemetry() const noexcept {
    return impl_->telemetry;
}

bool App::dispatch(const Event& ev) {
    return detail::dispatch_loaded_view_event(*impl_, ev);
}

void App::on_event(EventHandler cb) {
    impl_->event_handlers.emplace_back(std::move(cb));
}

void App::on_frame(std::function<void(double)> cb) {
    impl_->frame_callbacks.emplace_back(std::move(cb));
}

void App::capture_pointer() { impl_->pointer_captured = true; }
void App::release_pointer() { impl_->pointer_captured = false; }
bool App::pointer_captured() const { return impl_->pointer_captured; }

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

void update_perf_overlay_text(detail::AppImpl& impl, double gap_ms) {
    // R0: history, fps, and the sparkline derive from wall-clock frame GAPS
    // measured at cb_frame entry — not from frame durations. A pipeline that
    // stalls between callbacks reads healthy durations but its gaps blow up
    // (the "60 fps while one frame every 3 s" blind spot, TRACING §1.3).
    if (gap_ms > 0.0) {
        impl.perf_ms_history[static_cast<std::size_t>(impl.perf_history_head)] =
            static_cast<float>(gap_ms);
        impl.perf_history_head =
            (impl.perf_history_head + 1) %
            static_cast<int>(impl.perf_ms_history.size());
        impl.perf_history_count = std::min(
            impl.perf_history_count + 1,
            static_cast<int>(impl.perf_ms_history.size()));
        impl.perf_accum_s += gap_ms / 1000.0;
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
                  "gap^ %.1f ms  skip %llu\n"
                  "fb %dx%d css %dx%d dpi %.2f\n"
                  "work prep %.2f layout %.2f dl %.2f\n"
                  "rast %.2f comp %.2f ms layer %ux%u%s\n"
                  "ops %u culled %u rects %u dirty %u.%02u%%\n"
                  "flags %c%c%c%c%c%c%c",
                  impl.perf_ms,
                  impl.perf_fps,
                  static_cast<double>(impl.gap_max_ms_window),
                  static_cast<unsigned long long>(impl.skipped_presents_total),
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
    impl.gap_max_ms_window = 0.0f;  // gap^ is max-over-refresh-window
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

// Sync the OS cursor to the DOM's hovered cursor kind. Called at the
// end of each MouseMove dispatch in cb_event so cursor tracking stays
// current at input rate, decoupled from the paint schedule.
void sync_hover_cursor(detail::AppImpl& impl) {
    const int cur = impl.document.hovered_cursor();
    if (cur == impl.last_cursor) return;
    sapp_set_mouse_cursor(map_cursor(cur));
    impl.last_cursor = cur;
}

// Push the document's text-input intent to the platform IME: keep the
// input method enabled only while a text control is focused, and anchor
// the candidate window on the caret (converted to physical client px).
// Push-on-change — called after any event that can move focus or caret.
void sync_ime_state(detail::AppImpl& impl) {
    const bool active = impl.document.text_input_active();
    if (active != impl.last_ime_active) {
        impl.last_ime_active = active;
        sapp_ime_set_enabled(active);
    }
    if (!active) return;
    const Rect caret = impl.document.caret_rect();
    if (caret.w <= 0 || caret.h <= 0) return;
    if (caret.x == impl.last_ime_rect.x && caret.y == impl.last_ime_rect.y &&
        caret.w == impl.last_ime_rect.w && caret.h == impl.last_ime_rect.h) {
        return;
    }
    impl.last_ime_rect = caret;
    const float dpi = current_dpi_scale(impl);
    sapp_ime_set_rect(static_cast<int>(static_cast<float>(caret.x) * dpi),
                      static_cast<int>(static_cast<float>(caret.y) * dpi),
                      static_cast<int>(static_cast<float>(caret.w) * dpi),
                      static_cast<int>(static_cast<float>(caret.h) * dpi));
}

void cb_frame(void* user) {
    auto* impl = static_cast<detail::AppImpl*>(user);
    try {
        // R0 wall clock: the gap since the previous callback ENTRY is the
        // truth metric — measured before any frame work so a stall anywhere
        // in the pipeline shows up in it (TRACING §1.3).
        const auto frame_now = std::chrono::steady_clock::now();
        if (!impl->frame_clock_started) {
            impl->frame_clock_started = true;
            impl->session_start = frame_now;
            impl->last_frame_time = frame_now;
        }
        const double gap_ms =
            std::chrono::duration<double, std::milli>(
                frame_now - impl->last_frame_time).count();
        impl->last_frame_time = frame_now;
        const double t_ms =
            std::chrono::duration<double, std::milli>(
                frame_now - impl->session_start).count();
        if (gap_ms > impl->gap_max_ms_window) {
            impl->gap_max_ms_window = static_cast<float>(gap_ms);
        }
        // Publish the current frame + wall-clock time for log stamping. The
        // frame about to be assembled is presented_frames+1 (the graph's
        // newest bar), so logs during this callback align with it.
        detail::g_log_frame.store(impl->presented_frames + 1,
                                  std::memory_order_relaxed);
        detail::g_log_t_ms.store(t_ms, std::memory_order_relaxed);

        // Coalesced resize + pointer motion: at most one of each per frame.
        if (impl->has_pending_resize) {
            impl->has_pending_resize = false;
            Event resize_ev{};
            resize_ev.type = EventType::Resize;
            (void) detail::dispatch_loaded_view_event(*impl, resize_ev);
        }
        static const bool input_trace =
            std::getenv("AFFINEUI_INPUT_TRACE") != nullptr;
        // Pulse start: drain any OS input events queued behind AppKit's
        // per-run-loop-iteration throttle. This dispatches cb_event N
        // times, one per queued NSEvent, so mouse-up on a fast drag isn't
        // buried behind a backlog. No-op on non-Apple platforms.
        const auto phase_t0 = std::chrono::steady_clock::now();
#if defined(__APPLE__)
        affineui_mac_drain_native_events();
#endif
        const auto phase_t_after_dispatch = std::chrono::steady_clock::now();
        // Sample age + counters AFTER the drain so all three describe the
        // SAME batch of events (the drain updates last_move_stamp and the
        // moves_received/dispatched counters as it dispatches).
        double age_ms = -1.0;
        if (impl->has_last_move_stamp) {
            age_ms = std::chrono::duration<double, std::milli>(
                         phase_t_after_dispatch - impl->last_move_stamp)
                         .count();
        }
        const std::uint32_t raw_this_frame = impl->moves_received_since_frame;
        const std::uint32_t dsp_this_frame = impl->moves_dispatched_since_frame;
        impl->moves_received_since_frame = 0;
        impl->moves_dispatched_since_frame = 0;
        // Native frame ticks run before the idle short-circuit so
        // physics/animation callbacks can invalidate() to keep drawing;
        // an idle callback that touches nothing costs almost nothing.
        if (!impl->frame_callbacks.empty()) {
            const double dt = sapp_frame_duration_unfiltered();
            const auto callbacks = impl->frame_callbacks;
            for (const auto& cb : callbacks) {
                cb(dt);
            }
        }
        impl->last_dpi = sapp_dpi_scale();
        // Size the frame from the REAL swapchain, not sapp_width():
        // sokol_app's win32 high-dpi path can report a framebuffer size
        // scaled by dpi even though the D3D11 swapchain is client-pixel
        // sized, which would lay out (and crop) the document ~dpi× too
        // large. The swapchain is ground truth.
        const sg_swapchain sc_probe = sglue_swapchain();
        const int w = sc_probe.width > 0 ? sc_probe.width : sapp_width();
        const int h = sc_probe.height > 0 ? sc_probe.height : sapp_height();
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
            // R0: count skipped presents so "quiet" and "stalled" are
            // distinguishable; heartbeat at most once a second while any
            // telemetry consumer is on (docs/AFFINETOOLS_PROTOCOL.md `idle`).
            impl->skipped_since_present += 1;
            impl->skipped_presents_total += 1;
            if ((telemetry::sink_active() || tools::wants_telemetry()) &&
                t_ms - impl->last_idle_emit_ms >= 1000.0) {
                impl->last_idle_emit_ms = t_ms;
                if (telemetry::sink_active()) {
                    telemetry::sink_write_idle(t_ms,
                                               impl->skipped_since_present);
                }
                if (tools::wants_telemetry()) {
                    tools::push_idle(t_ms, impl->skipped_since_present);
                }
            }
            if (input_trace) {
                std::fprintf(stderr,
                    "[skip] gap=%.1f sc=%dx%d sapp=%dx%d dirty=0 anim=0 "
                    "vp_ch=0 settle=%d pend_resize=%d\n",
                    gap_ms, w, h, sapp_width(), sapp_height(),
                    impl->settle_frames,
                    impl->has_pending_resize ? 1 : 0);
                std::fflush(stderr);
            }
            // Devtools read commands still get answered on quiet frames —
            // the reads are read-only-never-relayout, so last-laid-out
            // state is exactly what they want (DESIGN §2.4).
            if (tools_has_pending()) tools_pump(impl->document);
            if (impl->quit_requested) sapp_request_quit();
            return;
        }
        // Reconcile pending view work BEFORE painting. invalidate() (from a
        // widget callback, the on_frame tick, or host code) sets view_dirty
        // when a set_view() builder is installed; re-run the builder here so
        // only the diff reaches the document (the cheap paint-only mutation
        // path) instead of the full serialize+reparse load_view() pays. Many
        // invalidate()s in one frame coalesce into a single reconcile. Guard
        // re-entrancy: rebuild_view() sets dirty (schedules this paint) but
        // must not re-arm view_dirty, so clear the flag first. A builder
        // exception is reported and swallowed here — the frame must still
        // present so the app stays responsive rather than tearing down.
        if (impl->view_dirty && impl->view_builder && impl->owner) {
            impl->view_dirty = false;
            try {
                impl->owner->rebuild_view();
                // A scroll-driven re-window recycled rows under a stationary
                // cursor: replay a MouseMove from the last pointer position
                // (a browser re-hit-tests hover on scroll the same way) so
                // :hover follows what is actually under the cursor.
                if (impl->hover_refresh_pending) {
                    impl->hover_refresh_pending = false;
                    if (impl->has_last_mouse) {
                        Event hover_ev{};
                        hover_ev.type = EventType::MouseMove;
                        hover_ev.pos.x = impl->last_mouse_x;
                        hover_ev.pos.y = impl->last_mouse_y;
                        (void) detail::dispatch_loaded_view_event(*impl,
                                                                  hover_ev);
                    }
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                             "AffineUI frame-loop rebuild_view failed: %s\n",
                             e.what());
            } catch (...) {
                std::fprintf(stderr,
                             "AffineUI frame-loop rebuild_view failed\n");
            }
        }
        // Min-frame-time gate (App::set_min_frame_time). Distinct from the
        // idle short-circuit above: here the app IS dirty (there IS work to
        // paint), but the caller wants at least min_frame_time_ms between
        // paints. Skip render, keep dirty, revisit on the next pulse. Zero
        // (default) disables — every dirty pulse paints.
        if (impl->min_frame_time_ms > 0.0 && impl->last_render_time_set) {
            const double elapsed_since_render_ms =
                std::chrono::duration<double, std::milli>(
                    frame_now - impl->last_render_time).count();
            if (elapsed_since_render_ms < impl->min_frame_time_ms) {
                if (impl->quit_requested) sapp_request_quit();
                return;
            }
        }
        impl->last_render_time = frame_now;
        impl->last_render_time_set = true;
        impl->last_w = w;
        impl->last_h = h;
        std::array<float, 64> ordered_perf_ms{};
        int ordered_perf_count = 0;
        if (impl->perf_overlay_enabled) {
            update_perf_overlay_text(*impl, gap_ms);
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
        static const bool frame_trace =
            std::getenv("AFFINEUI_FRAME_TRACE") != nullptr;
        const auto ft0 = std::chrono::steady_clock::now();
        {
            detail::TraceSpan frame_span("frame");
            impl->renderer.render_to(impl->document, target);
        }
        if (frame_trace) {
            const double total_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - ft0)
                    .count();
            const auto& st = impl->renderer.stats();
            std::fprintf(
                stderr,
                "[frame] %7.2f ms  prep=%.1f layout=%.1f record=%.1f "
                "raster=%.1f composite=%.1f  dl_changed=%u culled=%u "
                "dirty_rects=%u area=%u%%  %s%s%s%s%s\n",
                total_ms, st.prepare_us_this_frame / 1000.0,
                st.layout_us_this_frame / 1000.0,
                st.display_list_record_us_this_frame / 1000.0,
                st.raster_us_this_frame / 1000.0,
                st.composite_us_this_frame / 1000.0,
                st.display_list_diff_changed_ops,
                st.display_list_ops_culled_this_frame, st.dirty_rects,
                st.dirty_area_pct_x100 / 100,
                st.layout_dirty ? "L" : "-", st.paint_dirty ? "P" : "-",
                st.recorded_this_frame ? "R" : "-",
                st.root_layer_rasterized_this_frame ? "Z" : "-",
                st.animations_active ? "A" : "-");
            std::fflush(stderr);
        }
        impl->animations_active = impl->renderer.stats().animations_active;
        impl->dirty = impl->animations_active;
        if (impl->settle_frames > 0 && !impl->dirty) {
            --impl->settle_frames;
        }

        // R1: assemble the presented frame's telemetry record — readable via
        // App::frame_telemetry(), streamed by the AFFINEUI_TELEMETRY sink
        // (schema: docs/AFFINETOOLS_PROTOCOL.md). Struct writes are trivial
        // and unconditional; formatting/IO happen only behind sink_active().
        auto& tl = impl->telemetry;
        tl.v = kTelemetrySchemaVersion;
        tl.frame = ++impl->presented_frames;
        tl.t_ms = t_ms;
        tl.gap_ms = gap_ms;
        tl.cb_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - frame_now)
                       .count();
        tl.skipped = impl->skipped_since_present;
        impl->skipped_since_present = 0;
        tl.fb_w = w;
        tl.fb_h = h;
        tl.dpi = impl->last_dpi;
        fill_from_render_stats(tl, impl->renderer.stats());
        const mem::Stats mem_now = mem::stats();
        fill_mem_delta(tl, impl->mem_prev, mem_now);
        impl->mem_prev = mem_now;
        if (telemetry::sink_active()) {
            telemetry::sink_write_frame(tl);
        }
        if (tools::wants_telemetry()) {
            tools::push_frame(tl);
        }
        // Service queued devtools read commands at the frame boundary,
        // after render — blocks/StyleStore hold this frame's laid-out
        // state (AFFINETOOLS_DESIGN.md §2.1 typed-command pump).
        if (tools_has_pending()) tools_pump(impl->document);

        if (input_trace) {
            const auto now = std::chrono::steady_clock::now();
            const double dispatch_ms =
                std::chrono::duration<double, std::milli>(
                    phase_t_after_dispatch - phase_t0).count();
            const double cb_total_ms =
                std::chrono::duration<double, std::milli>(
                    now - frame_now).count();
            const auto& s = impl->renderer.stats();
            const char flag_rast   = s.root_layer_rasterized_this_frame ? 'R' : '-';
            const char flag_part   = s.root_layer_partial_this_frame    ? 'P' : '-';
            const char flag_direct = s.root_layer_direct_this_frame     ? 'D' : '-';
            const char flag_reused = s.root_layer_reused_this_frame     ? 'u' : '-';
            const char flag_alloc  = s.root_layer_allocated_this_frame  ? 'A' : '-';
            std::fprintf(stderr,
                "[input] gap=%.1f age=%.1f raw=%u dsp=%u sc=%dx%d sapp=%dx%d "
                "vp_ch=%d settle=%d rndr[%c%c%c%c%c] rndr_vp=%d rndr_ldirty=%d "
                "rndr_pdirty=%d dlchg=%d dlops=%u cap=%ux%u ct=%ux%u | "
                "drain=%.1f prep=%.1f layout=%.1f dlrec=%.1f rast=%.1f "
                "comp=%.1f | cb=%.1f\n",
                gap_ms, age_ms,
                raw_this_frame,
                dsp_this_frame,
                w, h, sapp_width(), sapp_height(),
                viewport_changed ? 1 : 0,
                impl->settle_frames,
                flag_rast, flag_part, flag_direct, flag_reused, flag_alloc,
                s.viewport_changed ? 1 : 0,
                s.layout_dirty ? 1 : 0,
                s.paint_dirty ? 1 : 0,
                s.display_list_changed_this_frame ? 1 : 0,
                s.cached_ops,
                s.root_layer_capacity_w, s.root_layer_capacity_h,
                s.root_layer_content_w,  s.root_layer_content_h,
                dispatch_ms,
                s.prepare_us_this_frame / 1000.0,
                s.layout_us_this_frame / 1000.0,
                s.display_list_record_us_this_frame / 1000.0,
                s.raster_us_this_frame / 1000.0,
                s.composite_us_this_frame / 1000.0,
                cb_total_ms);
            std::fflush(stderr);
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
        case SAPP_EVENTTYPE_KEY_DOWN: {
#if AFFINEUI_PERF
            // DevTools hotkey (zero config, on by default): F12 or the
            // platform's Chrome-DevTools shortcut.
            //   macOS:         Option+Cmd+I  (⌥⌘I)  = ALT | SUPER
            //   Windows/Linux: Ctrl+Shift+I         = CTRL | SHIFT
            // sokol modifier map on macOS: Option -> SAPP_MODIFIER_ALT,
            // Cmd -> SAPP_MODIFIER_SUPER, Ctrl -> SAPP_MODIFIER_CTRL.
#if defined(__APPLE__)
            constexpr std::uint32_t kDevtoolsMods =
                SAPP_MODIFIER_ALT | SAPP_MODIFIER_SUPER;
#else
            constexpr std::uint32_t kDevtoolsMods =
                SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT;
#endif
            if (impl->config.devtools_hotkey &&
                (ev->key_code == SAPP_KEYCODE_F12 ||
                 (ev->key_code == SAPP_KEYCODE_I &&
                  (ev->modifiers & kDevtoolsMods) == kDevtoolsMods))) {
                (void) tools_open_devtools();
                return;
            }
#endif
            aui_ev.type     = EventType::KeyDown;
            aui_ev.key_code = static_cast<int>(ev->key_code);
            aui_ev.key      = key_to_affine(ev->key_code);
            (void) detail::dispatch_loaded_view_event(*impl, aui_ev);
            sync_ime_state(*impl);
            return;
        }
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
                sync_ime_state(*impl);
            }
            return;
        case SAPP_EVENTTYPE_IME_COMPOSITION:
            aui_ev.type = EventType::Composition;
            aui_ev.text = ev->ime_composition;
            aui_ev.composition_cursor = ev->ime_composition_cursor;
            aui_ev.composition_clause_begin = ev->ime_composition_clause_begin;
            aui_ev.composition_clause_end = ev->ime_composition_clause_end;
            (void) detail::dispatch_loaded_view_event(*impl, aui_ev);
            sync_ime_state(*impl);
            return;
        case SAPP_EVENTTYPE_RESIZED:
            impl->has_pending_resize = true;  // coalesced: ≤1 per frame
            {
                static const bool input_trace_local =
                    std::getenv("AFFINEUI_INPUT_TRACE") != nullptr;
                if (input_trace_local) {
                    std::fprintf(stderr,
                        "[resize-evt] sapp=%dx%d fb=%dx%d\n",
                        sapp_width(), sapp_height(),
                        ev->framebuffer_width, ev->framebuffer_height);
                    std::fflush(stderr);
                }
            }
            return;
        case SAPP_EVENTTYPE_MOUSE_LEAVE:
            aui_ev.type = EventType::MouseMove;
            aui_ev.pos  = Point{-1, -1};
            (void) detail::dispatch_loaded_view_event(*impl, aui_ev);
            // Leave is a MouseMove-shaped event; the hover chain clears,
            // and the cursor kind that was in effect is stale. Same sync
            // path the normal MouseMove branch runs at end of dispatch.
            sync_hover_cursor(*impl);
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

    if (aui_ev.type == EventType::MouseMove) {
        ++impl->moves_received_since_frame;
        ++impl->moves_dispatched_since_frame;
        impl->last_move_stamp = std::chrono::steady_clock::now();
        impl->has_last_move_stamp = true;
    }
    if (aui_ev.type == EventType::MouseMove ||
        aui_ev.type == EventType::MouseWheel) {
        impl->last_mouse_x = aui_ev.pos.x;
        impl->last_mouse_y = aui_ev.pos.y;
        impl->has_last_mouse = true;
    }
    // Every input event dispatches immediately. Callers who want
    // per-frame semantics can accumulate inside their own handler; the
    // library does not coalesce input on their behalf. Fresh mouse
    // position on every OS event, no queued playback.
    const bool consumed = detail::dispatch_loaded_view_event(*impl, aui_ev);
    (void) consumed;
    if (aui_ev.type == EventType::MouseMove) sync_hover_cursor(*impl);
    if (aui_ev.type == EventType::MouseDown ||
        aui_ev.type == EventType::MouseUp) {
        sync_ime_state(*impl);  // click may have moved focus / caret
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
    detail::maybe_start_stack_sampler();  // AFFINEUI_SAMPLER=1
    sapp_desc desc{};
    desc.user_data            = impl_.get();
    desc.init_userdata_cb     = cb_init;
    desc.frame_userdata_cb    = cb_frame;
    desc.cleanup_userdata_cb  = cb_cleanup;
    desc.event_userdata_cb    = cb_event;
    // Config::width/height are logical points; sokol_app's win32
    // backend scales the created window by the monitor DPI itself.
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
