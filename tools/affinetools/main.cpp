// affinetools — the AffineUI devtools viewer (docs/AFFINETOOLS_DESIGN.md §4).
//
// S1 shell: attach to a running AffineUI app over the tools protocol and
// show live telemetry in a decius-styled window — tab strip (panels land
// in S2+), a live readout, and a status bar. Also opens telemetry dumps
// (--open file.jsonl) for post-mortem summaries.
//
// Usage:
//   affinetools                 attach to the newest advertised target
//   affinetools --pid <N>       attach to a specific process
//   affinetools --open <path>   summarize a telemetry JSONL dump
//
// Targets advertise via <tempdir>/affineui-tools/<pid>.json when the
// tools server is listening (F12 in any App-based app, or
// AFFINEUI_TOOLS_LISTEN=1, or affineui::tools_listen()).
//
// This app is itself the framework's dogfood: a plain affineui::App with
// the persistent-View reconcile path, rebuilt only when the client model
// moves (Client::revision()).

#include <affineui/affineui.h>

#include "affineui_tools.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(AFT_EMBEDDED_ASSETS)
#include "decius_bundle_css.h"   // aft_decius_bundle_css[] + _len
#include "decius_icons_ttf.h"    // aft_decius_icons_ttf[]  + _len
#endif

namespace {

#if !defined(AFT_EMBEDDED_ASSETS)
// On-disk fallback helpers (only when assets are NOT embedded).
std::string exe_dir() {
#if defined(_WIN32)
    char buf[512];
    const unsigned n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    std::string path = (n > 0 && n < sizeof(buf)) ? std::string(buf, n)
                                                  : std::string{};
#else
    std::error_code ec;
    std::string path =
        std::filesystem::read_symlink("/proc/self/exe", ec).string();
#endif
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string{"."}
                                      : path.substr(0, slash);
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}
#endif  // !AFT_EMBEDDED_ASSETS

/// The decius bundle CSS. Embedded (self-contained exe) when built with
/// AFT_EMBEDDED_ASSETS; otherwise resolved beside the exe / CWD as a
/// fallback for odd build configs.
std::string load_decius_bundle() {
#if defined(AFT_EMBEDDED_ASSETS)
    return std::string(reinterpret_cast<const char*>(aft_decius_bundle_css),
                       aft_decius_bundle_css_len);
#else
    const std::string href = affineui::framework_bundle_href(
        affineui::ViewTheme::Decius,
        affineui::framework_default_version(affineui::ViewTheme::Decius));
    if (href.empty()) return {};
    const std::filesystem::path candidates[] = {
        std::filesystem::path(exe_dir()) / href,
        std::filesystem::path(href),
    };
    for (const auto& candidate : candidates) {
        if (std::string css = read_text_file(candidate); !css.empty())
            return css;
    }
    return {};
#endif
}

/// In-memory resource loader for the bundle's @font-face url()s. The only
/// font the shell truly needs is decius-icons (the tab-strip glyphs);
/// serve its embedded TTF for any decius-icons request. The IBM Plex text
/// url()s return empty → the renderer falls back to core's embedded
/// Roboto, which is fine for the tool's text. Keeps the exe self-contained.
affineui::ResourceLoader embedded_resource_loader() {
    return [](std::string_view url) -> std::string {
#if defined(AFT_EMBEDDED_ASSETS)
        if (url.find("decius-icons") != std::string_view::npos) {
            return std::string(
                reinterpret_cast<const char*>(aft_decius_icons_ttf),
                aft_decius_icons_ttf_len);
        }
#else
        (void)url;
#endif
        return {};
    };
}

// ── native devtools chrome (docs/affinetools mock: shell, tab strip,
// status bar, stat tiles). Layers over the app's decius bundle as the
// user stylesheet, using its --dcs-* tokens so it tracks the theme. Kept
// small and aft-* prefixed — the panel widgets themselves are plain
// decius classes. ────────────────────────────────────────────────────────
const char* native_css() {
    return R"CSS(
html,body{margin:0;padding:0;height:100%;background:var(--dcs-bg-app,#14161c);overflow:hidden}
/* The View reconciler wraps content in .aui-root, which carries a default
   padding + min-height for ordinary docs. The devtools shell fills the
   window edge-to-edge, so neutralize the wrapper (same fix the headless
   DENDER dump applies). Normal-flow full-height column — NOT position:fixed,
   which did not re-resolve against a grown viewport on resize (blank). */
html,body,.aui-root{margin:0;padding:0;min-height:0}
.aft-root{height:100vh;display:flex;flex-direction:column;
  background:var(--dcs-bg);color:var(--dcs-text);font-family:var(--dcs-font)}

/* top tab strip */
.aft-tabs{display:flex;align-items:stretch;height:30px;flex:0 0 30px;
  background:var(--dcs-surface-1);border-bottom:1px solid var(--dcs-line)}
.aft-brand{display:flex;align-items:center;gap:6px;padding:0 12px;
  border-right:1px solid var(--dcs-line);color:var(--dcs-accent)}
.aft-brand__name{font-weight:600;font-size:12px;letter-spacing:.02em;
  color:var(--dcs-text)}
.aft-tab{display:inline-flex;align-items:center;gap:6px;padding:0 16px;
  border:none;background:transparent;cursor:pointer;font-size:12px;
  font-family:var(--dcs-font);color:var(--dcs-text-dim);
  border-bottom:2px solid transparent}
.aft-tab .di{color:var(--dcs-text-mute)}
.aft-tab[aria-selected="true"]{background:var(--dcs-bg);color:var(--dcs-text);
  border-bottom-color:var(--dcs-accent)}
.aft-tab[aria-selected="true"] .di{color:var(--dcs-accent)}
.aft-tabs__spacer{flex:1}
.aft-tabs__icon{display:flex;align-items:center;padding:0 12px;border:none;
  background:transparent;color:var(--dcs-text-mute);cursor:pointer}

/* body */
.aft-body{flex:1;min-height:0;overflow:auto;padding:14px 16px}
.aft-panel-title{font-size:13px;font-weight:600;color:var(--dcs-text);
  margin:0 0 2px}
.aft-panel-sub{font-size:11px;color:var(--dcs-text-mute);
  font-family:var(--dcs-font-mono);margin:0 0 14px;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.aft-caption{font-size:9px;text-transform:uppercase;letter-spacing:.08em;
  color:var(--dcs-text-mute);font-family:var(--dcs-font-mono);margin:0 0 8px}
.aft-empty{color:var(--dcs-text-dim);font-size:12px;max-width:44ch;
  line-height:1.5}
.aft-soon{color:var(--dcs-text-mute);font-size:12px;font-style:italic}

/* frametime graph (scrolling per-frame stacked bars) */
.aft-graph{height:104px;background:var(--dcs-well);border:1px solid var(--dcs-line);
  border-radius:var(--dcs-r-2);margin-bottom:14px;position:relative;
  cursor:crosshair}
.aft-graph__legend{display:flex;gap:14px;margin-bottom:8px;font-size:10px;
  font-family:var(--dcs-font-mono);color:var(--dcs-text-mute)}
.aft-graph__key{display:inline-flex;align-items:center;gap:5px}
.aft-graph__sw{width:8px;height:8px;border-radius:2px;display:inline-block}
/* caption + graph controls row. The buttons are standard Decius dcs-btn
   widgets (via View::button + add_class dcs-btn--sm) — no hand-styling. */
.aft-graph__caprow{display:flex;align-items:center;gap:6px;margin-bottom:8px}
.aft-graph__caprow .aft-caption{margin:0}
.aft-graph__capgap{flex:1}
.aft-gzoom{font-size:10px;font-family:var(--dcs-font-mono);
  color:var(--dcs-text-mute);min-width:28px;text-align:center}
/* log panel (windowed list of the visible tail) */
.aft-log{height:180px;background:var(--dcs-well);border:1px solid var(--dcs-line);
  border-radius:var(--dcs-r-2);margin-bottom:14px;overflow:hidden;
  display:flex;flex-direction:column;
  font-family:var(--dcs-font-mono);font-size:11px}
.aft-log__row{display:flex;align-items:center;gap:8px;height:18px;
  padding:0 8px;cursor:pointer;color:var(--dcs-text-dim);white-space:nowrap}
.aft-log__row:hover{background:var(--dcs-surface-2)}
.aft-log__dot{width:6px;height:6px;border-radius:50%;flex:0 0 6px;
  background:var(--dcs-text-mute)}
.aft-log__row--warn .aft-log__dot{background:var(--dcs-warn)}
.aft-log__row--error .aft-log__dot{background:var(--dcs-danger)}
.aft-log__row--warn{color:var(--dcs-warn)}
.aft-log__row--error{color:var(--dcs-danger)}
.aft-log__frame{color:var(--dcs-text-mute);min-width:44px;text-align:right}
.aft-log__text{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;
  color:var(--dcs-text)}

/* stat tile grid (mirrors the mock's Performance Stat row) */
.aft-stats{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px}
.aft-stat{flex:1 1 120px;min-width:110px;background:var(--dcs-well);
  border:1px solid var(--dcs-line);border-radius:var(--dcs-r-2);
  padding:8px 10px;display:flex;flex-direction:column;gap:2px}
.aft-stat__label{display:block;font-size:9px;text-transform:uppercase;
  letter-spacing:.08em;color:var(--dcs-text-mute);font-family:var(--dcs-font-mono)}
.aft-stat__value{display:block;font-size:15px;color:var(--dcs-text);
  font-family:var(--dcs-font-num);font-variant-numeric:tabular-nums}
.aft-stat--ok .aft-stat__value{color:var(--dcs-ok)}
.aft-stat--warn .aft-stat__value{color:var(--dcs-warn)}

/* status bar */
.aft-status{height:22px;flex:0 0 22px;display:flex;align-items:center;gap:12px;
  padding:0 12px;background:var(--dcs-surface-1);
  border-top:1px solid var(--dcs-line);font-size:10px;
  font-family:var(--dcs-font-num);color:var(--dcs-text-mute)}
.aft-status__spacer{flex:1}
.aft-badge{display:inline-flex;align-items:center;gap:5px;height:14px;
  padding:0 6px;border-radius:999px;font-size:9px;letter-spacing:.04em;
  background:var(--dcs-surface-3);color:var(--dcs-text-dim)}
.aft-badge::before{content:"";width:6px;height:6px;border-radius:50%;
  background:var(--dcs-text-mute)}
.aft-badge--ok{color:var(--dcs-ok)}
.aft-badge--ok::before{background:var(--dcs-ok)}
)CSS";
}

// ── model ────────────────────────────────────────────────────────────────

struct Tab {
    const char* id;
    const char* label;
    const char* icon;  // di-* glyph
};
const Tab kTabs[] = {
    {"elements",    "Elements",    "layers"},
    {"sources",     "Sources",     "file"},
    {"performance", "Performance", "gizmo"},
    {"memory",      "Memory",      "cube"},
};

// Frametime graph interaction state.
//
// Coordinate model: the graph paints frames NEWEST-AT-RIGHT. We index the
// selection by "age" — 0 = the newest frame, increasing leftward/older —
// so a selection stays anchored to the same frames as new ones scroll in.
// In `Fixed` scale, ms map to px at a constant rate (absolute comparison);
// in `Fit`, the tallest gap in view fills the graph (relative detail).
struct GraphState {
    enum class Scale { Fit, Fixed };
    enum class Sel { None, Point, Range };

    Scale scale{Scale::Fit};
    int   px_per_frame{2};   // zoom: wider bars = fewer, selectable frames
    Sel   sel{Sel::None};
    std::size_t sel_a{0};    // age index (0 = newest), inclusive
    std::size_t sel_b{0};    // age index, inclusive (Range: a<=b by age)

    // Live drag, in age indices (set by the pointer handler).
    bool   dragging{false};
    std::size_t drag_anchor{0};

    // Last painted geometry, so the pointer handler can map screen x → age.
    // Written by the paint handler, read by on_event (same thread).
    int graph_x{0};
    int graph_w{0};
    std::size_t frames_in_view{0};
};

struct ToolsModel {
    affineui::tools::Client client;
    int  want_pid{0};             // --pid target (0 = newest)
    bool dump_mode{false};
    affineui::tools::DumpSummary dump;
    std::string dump_path;
    double retry_accum_s{1.0e9};  // attach immediately on first tick
    std::string active_tab{"performance"};  // S1: Performance is the live one
    std::function<void(std::string)> select_tab;  // installed in main()
    GraphState graph;
    // Graph control callbacks (installed in main()): toggle scale, change
    // zoom by a step, clear the current selection.
    std::function<void()>    graph_toggle_scale;
    std::function<void(int)> graph_zoom;
    std::function<void()>    graph_clear_sel;

    // Log panel (virtual list). `log_first` is the top visible line index;
    // the wheel handler scrolls it. `log_follow` keeps the view pinned to
    // the newest lines until the user scrolls up. `log_rect` is recorded by
    // build for the wheel/click handlers.
    std::size_t              log_first{0};
    bool                     log_follow{true};
    affineui::Rect           log_rect{};
    std::size_t              log_visible{0};   // rows that fit (build-set)
    std::function<void(std::uint64_t)> select_frame;  // log click → graph
};

// A mock-style stat tile: uppercase label over a tabular value. Label and
// value are block spans so the flex column stacks them (a bare text node
// would fold into a shared inline run and collide).
void stat(affineui::View& v, std::string_view key, const char* label,
          const char* value, const char* tone = "") {
    std::string cls = "aft-stat";
    if (tone[0] != '\0') { cls += " aft-stat--"; cls += tone; }
    const std::string k{key};
    affineui::View::Scope tile = v.container(cls, key);
    {
        affineui::View::Scope l = v.element("span", "aft-stat__label", k + "-l");
        v.text(label, k + "-lt");
    }
    {
        affineui::View::Scope val = v.element("span", "aft-stat__value", k + "-v");
        v.text(value, k + "-vt");
    }
}

// The log panel: a virtual-list of diagnostic lines. Each row shows the
// level dot, the frame it arrived on, and the text. Clicking a row selects
// that frame on the performance graph. Scrolling is wheel-driven (the app
// owns `log_first`); `log_follow` pins to the newest lines until the user
// scrolls up. The whole list materializes only the visible window.
void build_log_panel(affineui::View& v, ToolsModel& m) {
    std::vector<affineui::tools::LogEntry> logs;
    m.client.log_history(logs, 4096);

    {
        affineui::View::Scope caprow =
            v.container("aft-graph__caprow", "log-cap-row");
        char cap[48];
        std::snprintf(cap, sizeof(cap), "Log · %zu lines", logs.size());
        v.text(cap, "log-cap").cls("aft-caption");
        v.container("aft-graph__capgap", "log-gap");
        v.button("Follow", false, "log-follow")
            .add_class("dcs-btn--sm")
            .attr("aria-pressed", m.log_follow ? "true" : "false")
            .on_click([&m] {
                m.log_follow = true;
                m.log_first = 0;  // recomputed to tail in build
            });
    }

    if (logs.empty()) {
        v.paragraph("No diagnostics yet.", "log-empty").cls("aft-soon");
        return;
    }

    // Windowed list: build ONLY the visible tail slice directly (honest
    // mini-virtualization). Follow mode pins the window to the newest line;
    // the wheel handler moves `log_first` to scroll into older lines. This
    // avoids the spacer-scroll virtual-list, whose rows land outside the
    // clip box unless the container's scroll position is synced (T10.1 /
    // T13 clip issue).
    constexpr std::size_t kVisible = 10;  // rows in the 180px box
    m.log_visible = kVisible;
    std::size_t first = m.log_follow || m.log_first + kVisible > logs.size()
                            ? (logs.size() > kVisible ? logs.size() - kVisible
                                                      : 0)
                            : m.log_first;
    const std::size_t last = std::min(logs.size(), first + kVisible);

    affineui::View::Scope list = v.container("aft-log", "log");
    for (std::size_t i = first; i < last; ++i) {
        const affineui::tools::LogEntry& e = logs[i];
        const char* lvl = e.level == 0   ? "debug"
                          : e.level == 2 ? "warn"
                          : e.level == 3 ? "error"
                                         : "info";
        const std::string key = "log-" + std::to_string(i);
        affineui::View::Scope row =
            v.container(std::string("aft-log__row aft-log__row--") + lvl, key);
        row.ref().on_click([&m, frame = e.frame] {
            if (m.select_frame) m.select_frame(frame);
        });
        char meta[24];
        std::snprintf(meta, sizeof(meta), "%llu",
                      static_cast<unsigned long long>(e.frame));
        v.element("span", "aft-log__dot", key + "-d");
        {
            affineui::View::Scope fr = v.element("span", "aft-log__frame", key + "-f");
            v.text(meta, key + "-ft");
        }
        {
            affineui::View::Scope txt = v.element("span", "aft-log__text", key + "-x");
            v.text(e.text, key + "-xt");
        }
    }
}

void build_performance(affineui::View& v, ToolsModel& m,
                       const affineui::tools::ClientStatus& s) {
    char line[256];
    std::snprintf(line, sizeof(line), "%s  (pid %d, affineui %s)",
                  s.exe.c_str(), s.pid, s.affineui_version.c_str());
    v.paragraph("Attached", "live-title").cls("aft-panel-title");
    v.paragraph(line, "live-target").cls("aft-panel-sub");

    // Scrolling frametime graph — one bar per frame, contributors stacked
    // (layout / raster / composite / other), newest at the right, with the
    // 144 Hz (~6.9 ms) budget line. Painted by the "frametime" custom-paint
    // handler in main(); pointer selection is wired via App::on_event.
    {
        affineui::View::Scope caprow = v.container("aft-graph__caprow", "cap-graph-row");
        v.text("Frame time · stacked stages", "cap-graph").cls("aft-caption");
        v.container("aft-graph__capgap", "cap-gap");
        // Scale toggle + zoom controls — standard small Decius buttons.
        const bool fixed = m.graph.scale == GraphState::Scale::Fixed;
        v.button("Fixed", false, "g-scale")
            .add_class("dcs-btn--sm")
            .attr("aria-pressed", fixed ? "true" : "false")
            .on_click([&m] { if (m.graph_toggle_scale) m.graph_toggle_scale(); });
        v.button("−", false, "g-zoomout")
            .add_class("dcs-btn--sm")
            .on_click([&m] { if (m.graph_zoom) m.graph_zoom(-1); });
        char zlbl[16];
        std::snprintf(zlbl, sizeof(zlbl), "%dpx", m.graph.px_per_frame);
        v.text(zlbl, "g-zoom").cls("aft-gzoom");
        v.button("+", false, "g-zoomin")
            .add_class("dcs-btn--sm")
            .on_click([&m] { if (m.graph_zoom) m.graph_zoom(+1); });
    }
    {
        affineui::View::Scope legend =
            v.container("aft-graph__legend", "graph-legend");
        struct { const char* label; const char* color; } keys[] = {
            {"layout", "#b48cff"}, {"raster", "#4ed18a"},
            {"composite", "#f2b14a"}, {"other", "#4d9fff"},
        };
        for (const auto& k : keys) {
            affineui::View::Scope key =
                v.container("aft-graph__key", std::string("lk-") + k.label);
            v.element_ref("span", "aft-graph__sw",
                          std::string("sw-") + k.label)
                .attr("style", std::string("background:") + k.color);
            v.text(k.label, std::string("lt-") + k.label);
        }
    }
    v.canvas("frametime", "graph").cls("aft-graph");

    // Selection summary: stats over the point/range the user picked on the
    // graph. Ages (0 = newest) map to history indices [size-1-hi, size-hi].
    if (m.graph.sel != GraphState::Sel::None) {
        std::vector<affineui::FrameTelemetry> h;
        m.client.frame_history(h, 4096);
        const std::size_t lo_age =
            m.graph.sel == GraphState::Sel::Range
                ? std::min(m.graph.sel_a, m.graph.sel_b)
                : m.graph.sel_a;
        const std::size_t hi_age =
            m.graph.sel == GraphState::Sel::Range
                ? std::max(m.graph.sel_a, m.graph.sel_b)
                : m.graph.sel_a;
        affineui::tools::RangeStats rs;
        if (!h.empty() && hi_age < h.size()) {
            const std::size_t end = h.size() - lo_age;          // exclusive
            const std::size_t begin = h.size() - 1 - hi_age;    // inclusive
            rs = affineui::tools::compute_range_stats(h, begin, end);
        }
        char cap[64];
        if (m.graph.sel == GraphState::Sel::Point) {
            std::snprintf(cap, sizeof(cap), "Selection · 1 frame");
        } else {
            std::snprintf(cap, sizeof(cap), "Selection · %zu frames",
                          rs.frames);
        }
        {
            affineui::View::Scope caprow =
                v.container("aft-graph__caprow", "sel-cap-row");
            v.text(cap, "sel-cap").cls("aft-caption");
            v.container("aft-graph__capgap", "sel-gap");
            v.button("Clear", false, "sel-clear")
                .add_class("dcs-btn--sm")
                .on_click([&m] { if (m.graph_clear_sel) m.graph_clear_sel(); });
        }
        char v_fps[32], v_avg[32], v_minmax[40], v_over[40];
        std::snprintf(v_fps, sizeof(v_fps), "%.0f", rs.avg_fps);
        std::snprintf(v_avg, sizeof(v_avg), "%.2f ms", rs.avg_gap_ms);
        std::snprintf(v_minmax, sizeof(v_minmax), "%.2f / %.2f ms",
                      rs.min_gap_ms, rs.max_gap_ms);
        std::snprintf(v_over, sizeof(v_over), "%zu / %zu", rs.over_budget,
                      rs.frames);
        {
            affineui::View::Scope row = v.container("aft-stats", "sel-row1");
            stat(v, "sel-fps", "avg fps", v_fps, rs.avg_fps >= 120.0 ? "ok" : "");
            stat(v, "sel-avg", "avg frame", v_avg,
                 rs.avg_gap_ms > 8.0 ? "warn" : "ok");
            stat(v, "sel-mm", "min / max", v_minmax);
            stat(v, "sel-over", "over budget", v_over,
                 rs.over_budget > 0 ? "warn" : "ok");
        }
        v.paragraph("Avg per component", "sel-cap2").cls("aft-caption");
        char v_l[24], v_r[24], v_c[24], v_o[24];
        std::snprintf(v_l, sizeof(v_l), "%.2f ms", rs.avg_layout_ms);
        std::snprintf(v_r, sizeof(v_r), "%.2f ms", rs.avg_raster_ms);
        std::snprintf(v_c, sizeof(v_c), "%.2f ms", rs.avg_composite_ms);
        std::snprintf(v_o, sizeof(v_o), "%.2f ms", rs.avg_other_ms);
        {
            affineui::View::Scope row = v.container("aft-stats", "sel-row2");
            stat(v, "sel-l", "layout", v_l);
            stat(v, "sel-r", "raster", v_r);
            stat(v, "sel-c", "composite", v_c);
            stat(v, "sel-o", "other", v_o);
        }
    }

    if (s.have_frame) {
        const affineui::FrameTelemetry& t = s.last_frame;
        const double fps = t.gap_ms > 0.0 ? 1000.0 / t.gap_ms : 0.0;
        char v_fps[32], v_gap[32], v_cb[32], v_frame[32];
        std::snprintf(v_fps, sizeof(v_fps), "%.0f", fps);
        std::snprintf(v_gap, sizeof(v_gap), "%.2f ms", t.gap_ms);
        std::snprintf(v_cb, sizeof(v_cb), "%.2f ms", t.cb_ms);
        std::snprintf(v_frame, sizeof(v_frame), "%llu",
                      static_cast<unsigned long long>(t.frame));
        {
            affineui::View::Scope row = v.container("aft-stats", "stat-row1");
            stat(v, "s-fps", "fps", v_fps, fps >= 120.0 ? "ok" : "");
            // 6.94ms ≈ 144Hz budget: warn past ~8ms.
            stat(v, "s-gap", "frame gap", v_gap, t.gap_ms > 8.0 ? "warn" : "ok");
            stat(v, "s-cb", "cb", v_cb);
            stat(v, "s-frame", "frame", v_frame);
        }

        v.paragraph("Stage timing", "cap-stage").cls("aft-caption");
        char v_layout[32], v_rast[32], v_comp[32], v_ops[32];
        std::snprintf(v_layout, sizeof(v_layout), "%u us", t.layout_us);
        std::snprintf(v_rast, sizeof(v_rast), "%u us", t.raster_us);
        std::snprintf(v_comp, sizeof(v_comp), "%u us", t.composite_us);
        std::snprintf(v_ops, sizeof(v_ops), "%u", t.cached_ops);
        {
            affineui::View::Scope row = v.container("aft-stats", "stat-row2");
            stat(v, "s-layout", "layout", v_layout);
            stat(v, "s-rast", "raster", v_rast);
            stat(v, "s-comp", "composite", v_comp);
            stat(v, "s-ops", "ops", v_ops);
        }

        v.paragraph("Memory", "cap-mem").cls("aft-caption");
        char v_mem[32], v_blk[32], v_alloc[32];
        std::snprintf(v_mem, sizeof(v_mem), "%.2f MB",
                      static_cast<double>(t.mem_live_bytes) / (1024.0 * 1024.0));
        std::snprintf(v_blk, sizeof(v_blk), "%llu",
                      static_cast<unsigned long long>(t.mem_live_blocks));
        std::snprintf(v_alloc, sizeof(v_alloc), "%u / %u", t.allocs, t.frees);
        {
            affineui::View::Scope row = v.container("aft-stats", "stat-row3");
            stat(v, "s-mem", "heap", v_mem);
            stat(v, "s-blk", "blocks", v_blk);
            stat(v, "s-alloc", "alloc/free", v_alloc);
        }
    } else if (s.last_was_idle) {
        std::snprintf(line, sizeof(line),
                      "target idle - %llu presents skipped",
                      static_cast<unsigned long long>(s.idle_skipped));
        v.paragraph(line, "live-idle").cls("aft-soon");
    } else {
        v.paragraph("Waiting for the first frame…", "live-wait")
            .cls("aft-soon");
    }

    // Diagnostics log (virtual list, frame-correlated, click-to-select).
    build_log_panel(v, m);
}

void build_dump(affineui::View& v, const ToolsModel& m) {
    v.paragraph("Telemetry dump", "dump-title").cls("aft-panel-title");
    v.paragraph(m.dump_path, "dump-path").cls("aft-panel-sub");
    if (!m.dump.ok) {
        v.paragraph(m.dump.error, "dump-error").cls("aft-empty");
        return;
    }
    char v_frames[32], v_idles[32], v_avg[32], v_p95[32], v_max[32], v_cb[32];
    std::snprintf(v_frames, sizeof(v_frames), "%zu", m.dump.frames);
    std::snprintf(v_idles, sizeof(v_idles), "%zu", m.dump.idles);
    std::snprintf(v_avg, sizeof(v_avg), "%.2f ms", m.dump.avg_gap_ms);
    std::snprintf(v_p95, sizeof(v_p95), "%.2f ms", m.dump.p95_gap_ms);
    std::snprintf(v_max, sizeof(v_max), "%.2f ms", m.dump.max_gap_ms);
    std::snprintf(v_cb, sizeof(v_cb), "%.2f ms", m.dump.avg_cb_ms);
    {
        affineui::View::Scope row = v.container("aft-stats", "d-row1");
        stat(v, "d-frames", "frames", v_frames);
        stat(v, "d-idles", "idle beats", v_idles);
    }
    v.paragraph("Frame gap", "d-cap").cls("aft-caption");
    {
        affineui::View::Scope row = v.container("aft-stats", "d-row2");
        stat(v, "d-avg", "avg", v_avg);
        stat(v, "d-p95", "p95", v_p95, m.dump.p95_gap_ms > 8.0 ? "warn" : "ok");
        stat(v, "d-max", "max", v_max);
        stat(v, "d-cb", "cb avg", v_cb);
    }
}

void build_view(affineui::View& v, ToolsModel& m) {
    using affineui::View;
    v.set_theme(affineui::ViewTheme::Decius);

    const affineui::tools::ClientStatus s = m.client.status();

    View::Scope root = v.container("dcs aft-root", "root");
    root.ref().attr("data-dcs-density", "compact");

    // ── top tab strip ───────────────────────────────────────────────────
    {
        View::Scope tabs = v.container("aft-tabs", "tabs");
        {
            View::Scope brand = v.container("aft-brand", "brand");
            v.element_ref("i", "di di-gizmo", "brand-icon");
            v.text("affinetools", "brand-name").cls("aft-brand__name");
        }
        for (const Tab& tab : kTabs) {
            const bool active = m.active_tab == tab.id;
            View::Scope t = v.container("aft-tab", tab.id);
            t.ref().attr("aria-selected", active ? "true" : "false");
            if (m.select_tab) {
                const std::string id = tab.id;
                t.ref().on_click([&m, id] {
                    if (m.select_tab) m.select_tab(id);
                });
            }
            v.element_ref("i", std::string("di di-") + tab.icon,
                          std::string(tab.id) + "-i");
            v.text(tab.label, std::string(tab.id) + "-l");
        }
        v.container("aft-tabs__spacer", "tabsp");
        {
            View::Scope cog = v.container("aft-tabs__icon", "cog");
            v.element_ref("i", "di di-cog", "cog-i");
        }
    }

    // ── body: active panel ──────────────────────────────────────────────
    {
        View::Scope body = v.container("aft-body", "body");
        if (m.dump_mode) {
            build_dump(v, m);
        } else if (!s.connected) {
            v.paragraph("Waiting for a target", "wait-title")
                .cls("aft-panel-title");
            v.paragraph(
                "Run any AffineUI app and press F12 (or start it with "
                "AFFINEUI_TOOLS_LISTEN=1) and affinetools will attach.",
                "wait-hint")
                .cls("aft-empty");
        } else if (m.active_tab == "performance") {
            build_performance(v, m, s);
        } else {
            // Elements / Sources / Memory panels land in S2+.
            for (const Tab& tab : kTabs) {
                if (m.active_tab != tab.id) continue;
                v.paragraph(tab.label, "panel-title").cls("aft-panel-title");
                v.paragraph("This panel arrives in a later stage.",
                            "panel-soon")
                    .cls("aft-soon");
            }
        }
    }

    // ── status bar ──────────────────────────────────────────────────────
    {
        View::Scope status = v.container("aft-status", "statusbar");
        if (s.connected) {
            v.text("ATTACHED", "st-badge").cls("aft-badge aft-badge--ok");
            char sess[96];
            std::snprintf(sess, sizeof(sess), "session %s",
                          s.session_id.c_str());
            v.text(sess, "st-session");
            v.container("aft-status__spacer", "st-sp");
            char right[96];
            if (s.have_frame) {
                std::snprintf(right, sizeof(right),
                              "%u nodes · %llu events · %llu dropped",
                              s.last_frame.cached_ops,
                              static_cast<unsigned long long>(s.events),
                              static_cast<unsigned long long>(s.dropped));
            } else {
                std::snprintf(right, sizeof(right),
                              "%llu events · %llu dropped",
                              static_cast<unsigned long long>(s.events),
                              static_cast<unsigned long long>(s.dropped));
            }
            v.text(right, "st-right");
        } else if (m.dump_mode) {
            v.text("DUMP", "st-badge").cls("aft-badge");
            v.text(m.dump.affineui_version.empty()
                       ? "post-mortem"
                       : ("affineui " + m.dump.affineui_version),
                   "st-mode");
        } else {
            v.text("WAITING", "st-badge").cls("aft-badge");
            v.text("no target", "st-none");
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    ToolsModel model;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--pid" && i + 1 < argc) {
            model.want_pid = std::atoi(argv[++i]);
        } else if (arg.rfind("--pid=", 0) == 0) {
            model.want_pid = std::atoi(argv[i] + 6);
        } else if (arg == "--open" && i + 1 < argc) {
            model.dump_mode = true;
            model.dump_path = argv[++i];
        }
    }
    if (model.dump_mode) {
        model.dump = affineui::tools::summarize_dump(model.dump_path);
        std::fprintf(stderr, "[affinetools] dump %s: %zu frames, %zu idles\n",
                     model.dump_path.c_str(), model.dump.frames,
                     model.dump.idles);
    }

    affineui::App::Config cfg;
    cfg.title = "affinetools";
    cfg.width = 760;
    cfg.height = 620;
    cfg.devtools_hotkey = false;  // the tools don't open tools on themselves
    // Serve the embedded icon font for the bundle's @font-face url()s, so
    // the exe is self-contained (no frameworks/ dir needed).
    cfg.resource_loader = embedded_resource_loader();
    affineui::App app{cfg};

    const std::string bundle = load_decius_bundle();
    if (bundle.empty()) {
        std::fprintf(stderr,
                     "[affinetools] decius bundle unavailable; styling plain\n");
    }

    // Tab selection: mutate the model and rebuild once. (Panels other than
    // Performance are placeholders until S2+.)
    model.select_tab = [&](std::string id) {
        if (model.active_tab == id) return;
        model.active_tab = std::move(id);
        app.rebuild_view();
    };

    // Graph controls.
    model.graph_toggle_scale = [&] {
        model.graph.scale = model.graph.scale == GraphState::Scale::Fit
                                ? GraphState::Scale::Fixed
                                : GraphState::Scale::Fit;
        app.rebuild_view();
        app.request_custom_repaint("frametime");
    };
    model.graph_zoom = [&](int step) {
        // px/frame: 1 (dense, zoomed out) … 12 (wide bars, per-frame).
        int z = model.graph.px_per_frame + step;
        z = std::max(1, std::min(12, z));
        if (z == model.graph.px_per_frame) return;
        model.graph.px_per_frame = z;
        // A zoom change moves frames under the selection; clear it to avoid
        // a stale band pointing at the wrong frames.
        model.graph.sel = GraphState::Sel::None;
        app.rebuild_view();
        app.request_custom_repaint("frametime");
    };
    model.graph_clear_sel = [&] {
        model.graph.sel = GraphState::Sel::None;
        app.rebuild_view();
        app.request_custom_repaint("frametime");
    };

    // Log line click → select that frame on the performance graph. The
    // graph is indexed by age (0 = newest); find the age of `frame` from
    // the client's frame history, then pin a point selection there.
    std::vector<affineui::FrameTelemetry> sel_hist;
    model.select_frame = [&](std::uint64_t frame) {
        model.client.frame_history(sel_hist, 4096);
        if (sel_hist.empty()) return;
        // Newest frame is at the back; age = distance from the back.
        std::size_t age = 0;
        bool found = false;
        for (std::size_t i = sel_hist.size(); i-- > 0;) {
            if (sel_hist[i].frame == frame) {
                age = sel_hist.size() - 1 - i;
                found = true;
                break;
            }
        }
        if (!found) return;  // frame scrolled out of history
        model.graph.sel = GraphState::Sel::Point;
        model.graph.sel_a = model.graph.sel_b = age;
        app.rebuild_view();
        app.request_custom_repaint("frametime");
    };

    // Frametime graph: per-frame stacked bars, newest at the right. Scale
    // mode (Fit/Fixed), zoom (px_per_frame), and point/range selection all
    // read from model.graph. The paint handler records the graph geometry
    // back into model.graph so the pointer handler can map screen x → frame
    // age. Budget line at the 144 Hz (~6.94 ms) target.
    constexpr double kBudgetMs = 6.94;   // 144 Hz
    // In Fixed scale, this ms maps to full graph height (headroom past
    // budget so a doubled frame is still visible without clipping).
    constexpr double kFixedTopMs = 20.0;
    std::vector<affineui::FrameTelemetry> hist;
    app.set_custom_paint("frametime", [&](affineui::Painter& p,
                                          const affineui::Rect& r) {
        GraphState& g = model.graph;
        const int bar_w = std::max(1, g.px_per_frame);
        const std::size_t max_bars =
            static_cast<std::size_t>(std::max(1, r.w / bar_w));
        // Fetch a bit more than fits so the newest bar sits flush right.
        model.client.frame_history(hist, max_bars);
        p.fill_rect(r, affineui::Color{20, 22, 28, 255});

        const int pad = 2;
        const int h = r.h - pad * 2;
        const int base_y = r.y + r.h - pad;
        const std::size_t in_view = std::min(hist.size(), max_bars);

        // Record geometry for the pointer handler (age 0 = newest = right).
        g.graph_x = r.x;
        g.graph_w = r.w;
        g.frames_in_view = in_view;
        if (hist.empty()) return;

        double top_ms = kFixedTopMs;
        if (g.scale == GraphState::Scale::Fit) {
            double max_ms = kBudgetMs;
            for (std::size_t k = 0; k < in_view; ++k) {
                max_ms = std::max(max_ms, hist[hist.size() - 1 - k].gap_ms);
            }
            top_ms = max_ms / 0.85;  // tallest bar fills ~85% of height
        }
        auto ms_to_px = [&](double ms) {
            return static_cast<int>((ms / top_ms) * h + 0.5);
        };

        // Selection band (drawn under the bars). Ages are 0-based from the
        // right edge; a..b inclusive.
        auto age_to_x_right = [&](std::size_t age) {
            return r.x + r.w - static_cast<int>(age) * bar_w;
        };
        if (g.sel != GraphState::Sel::None) {
            const std::size_t lo =
                g.sel == GraphState::Sel::Range ? std::min(g.sel_a, g.sel_b)
                                                : g.sel_a;
            const std::size_t hi =
                g.sel == GraphState::Sel::Range ? std::max(g.sel_a, g.sel_b)
                                                : g.sel_a;
            const int x_hi_right = age_to_x_right(lo);      // newer edge
            const int x_lo_left = age_to_x_right(hi) - bar_w;  // older edge
            const int bx = std::max(r.x, x_lo_left);
            const int bw = std::min(r.x + r.w, x_hi_right) - bx;
            if (bw > 0) {
                p.fill_rect(affineui::Rect{bx, r.y, bw, r.h},
                            affineui::Color{77, 159, 255, 40});
            }
        }

        // Budget guide line.
        const int budget_y = base_y - ms_to_px(kBudgetMs);
        p.stroke_line(static_cast<float>(r.x), static_cast<float>(budget_y),
                      static_cast<float>(r.x + r.w),
                      static_cast<float>(budget_y),
                      affineui::Color{242, 177, 74, 90}, 1.0f);

        // Bars: one per frame, newest at the right edge.
        const int fill_w = bar_w > 1 ? bar_w - 1 : bar_w;
        const affineui::Color c_layout{180, 140, 255, 255};
        const affineui::Color c_raster{78, 209, 138, 255};
        const affineui::Color c_comp{242, 177, 74, 255};
        const affineui::Color c_other{77, 159, 255, 255};
        const affineui::Color c_over{255, 122, 122, 255};  // over-budget tint
        for (std::size_t age = 0; age < in_view; ++age) {
            const affineui::FrameTelemetry& f = hist[hist.size() - 1 - age];
            const int x = age_to_x_right(age) - bar_w;
            if (x < r.x) break;
            const bool over = f.gap_ms > kBudgetMs;
            const double stage_ms =
                (f.layout_us + f.raster_us + f.composite_us) / 1000.0;
            const double other_ms = std::max(0.0, f.gap_ms - stage_ms);
            int y = base_y;
            auto seg = [&](double ms, affineui::Color col) {
                const int ph = ms_to_px(ms);
                if (ph <= 0) return;
                p.fill_rect(affineui::Rect{x, y - ph, fill_w, ph}, col);
                y -= ph;
            };
            seg(f.composite_us / 1000.0, c_comp);
            seg(f.raster_us / 1000.0, c_raster);
            seg(f.layout_us / 1000.0, c_layout);
            seg(other_ms, over ? c_over : c_other);
        }
    });

    app.set_view([&model](affineui::View& v) { build_view(v, model); });
    // Decius bundle + the devtools chrome CSS as one user stylesheet. The
    // base_url ("aft-embedded:/css/") makes the bundle's relative
    // url(../fonts/...) resolve to "aft-embedded:/fonts/..." which the
    // embedded resource loader serves — no files on disk.
    std::string css = bundle;
    css += "\n";
    css += native_css();
    app.set_stylesheet(css, "aft-embedded:/css/bundle.css");

    std::uint64_t last_revision = ~0ull;
    app.on_frame([&](double dt) {
        if (model.dump_mode) return;
        // Attach / reconnect at 1 Hz until a target answers.
        if (!model.client.attached()) {
            model.retry_accum_s += dt;
            if (model.retry_accum_s >= 1.0) {
                model.retry_accum_s = 0.0;
                const bool ok = model.want_pid != 0
                                    ? model.client.attach_pid(model.want_pid)
                                    : model.client.attach_newest();
                if (ok) {
                    const auto s = model.client.status();
                    std::fprintf(stderr,
                                 "[affinetools] attached pid %d session %s\n",
                                 s.pid, s.session_id.c_str());
                }
            }
        }
        // Rebuild the panel when the client model moved; the frametime
        // graph is a canvas, so it advances via a cheap repaint request
        // rather than a reconcile.
        const std::uint64_t rev = model.client.revision();
        if (rev != last_revision) {
            last_revision = rev;
            app.rebuild_view();
            app.request_custom_repaint("frametime");
        }
    });

    // Graph selection: map pointer x over the frametime canvas to a frame
    // "age" (0 = newest, at the right edge) and drive point / range
    // selection. Uses the geometry the paint handler recorded in
    // model.graph. A press-release without motion is a point pick; a drag
    // is a range. Clicking outside the graph clears the selection.
    app.on_event([&](const affineui::Event& ev,
                     const std::vector<affineui::Document::HoverInfo>& hover)
                     -> bool {
        if (model.dump_mode) return false;

        // Wheel over the log list scrolls it (virtual list; the app owns
        // first_item). A scroll up drops out of follow mode; scrolling back
        // to the bottom re-enters it.
        if (ev.type == affineui::EventType::MouseWheel) {
            bool over_log = false;
            for (const auto& info : hover) {
                if (std::find(info.classes.begin(), info.classes.end(),
                              "aft-log") != info.classes.end()) {
                    over_log = true;
                    break;
                }
            }
            if (!over_log) return false;
            const std::size_t total = model.client.log_count();
            const std::size_t vis = model.log_visible ? model.log_visible : 10;
            if (total <= vis) return true;
            const std::size_t max_first = total - vis;
            // wheel_dy > 0 is usually "up" (toward older lines).
            const int step = 3;
            std::size_t first = model.log_follow ? max_first : model.log_first;
            if (ev.wheel_dy > 0.0f) {
                first = first >= static_cast<std::size_t>(step)
                            ? first - step
                            : 0;
                model.log_follow = false;
            } else if (ev.wheel_dy < 0.0f) {
                first = std::min(max_first, first + step);
            }
            model.log_first = first;
            model.log_follow = (first >= max_first);
            app.rebuild_view();
            return true;
        }

        GraphState& g = model.graph;
        if (g.graph_w <= 0 || g.frames_in_view == 0) return false;
        const int bar_w = std::max(1, g.px_per_frame);

        auto x_to_age = [&](int x) -> std::size_t {
            const int from_right = (g.graph_x + g.graph_w) - x;
            int age = from_right / bar_w;
            if (age < 0) age = 0;
            const std::size_t max_age = g.frames_in_view - 1;
            return std::min(static_cast<std::size_t>(age), max_age);
        };
        const bool inside =
            ev.pos.x >= g.graph_x && ev.pos.x < g.graph_x + g.graph_w;

        switch (ev.type) {
            case affineui::EventType::MouseDown: {
                if (!inside) {
                    if (g.sel != GraphState::Sel::None) {
                        g.sel = GraphState::Sel::None;
                        app.rebuild_view();
                        app.request_custom_repaint("frametime");
                    }
                    return false;
                }
                g.dragging = true;
                g.drag_anchor = x_to_age(ev.pos.x);
                g.sel = GraphState::Sel::Point;
                g.sel_a = g.drag_anchor;
                g.sel_b = g.drag_anchor;
                app.capture_pointer();
                app.rebuild_view();
                app.request_custom_repaint("frametime");
                return true;
            }
            case affineui::EventType::MouseMove: {
                if (!g.dragging) return false;
                const std::size_t age = x_to_age(ev.pos.x);
                if (age != g.drag_anchor) {
                    g.sel = GraphState::Sel::Range;
                    g.sel_a = std::min(g.drag_anchor, age);
                    g.sel_b = std::max(g.drag_anchor, age);
                } else {
                    g.sel = GraphState::Sel::Point;
                    g.sel_a = g.sel_b = age;
                }
                app.rebuild_view();
                app.request_custom_repaint("frametime");
                return true;
            }
            case affineui::EventType::MouseUp: {
                if (!g.dragging) return false;
                g.dragging = false;
                app.release_pointer();
                return true;
            }
            default:
                return false;
        }
    });

    return app.run();
}
