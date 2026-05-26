// decius_video_editor - timeline-heavy editing UI using Decius CSS.

#include "decius_interactions.h"

#include <affineui/affineui.h>

#include <sokol_log.h>

#include <sstream>
#include <string>
#include <unordered_map>

namespace dcs = demo::decius;

namespace {

struct VideoState {
    bool playing{false};
    int selected_clip{1};
    std::unordered_map<std::string, float> values{
        {"exposure", .35f}, {"contrast", .58f},
        {"lift", .52f}, {"gamma", .67f}, {"gain", .41f},
    };
};

float value(const VideoState& s, std::string_view id, float fallback) {
    auto it = s.values.find(std::string(id));
    return it == s.values.end() ? fallback : it->second;
}

std::string clip(int id, int left, int width, std::string_view label,
                 std::string_view tone, bool selected) {
    std::ostringstream h;
    h << "<div id=\"clip-" << id << "\" class=\"clip clip-" << tone;
    if (selected) h << " clip-selected";
    h << "\" style=\"left:" << left << "px;width:" << width << "px\">"
      << demo::html_escape(label) << "</div>";
    return h.str();
}

std::string render(const VideoState& s) {
    std::ostringstream h;
    h << R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<link rel="stylesheet" href="frameworks/css/decius-css-0.4.1.bundle.min.css">
<style>
body{margin:0;background:#14161c}
.app{height:100vh;background:var(--dcs-bg-app);display:flex;flex-direction:column;overflow:hidden}
.top{flex:1;min-height:0;display:flex;gap:1px;background:var(--dcs-line)}
.browser{flex:0 0 260px}.viewer{flex:1;min-width:0}.inspector{flex:0 0 300px}
.viewer-screen{height:100%;position:relative;background:radial-gradient(circle at 50% 38%,#30384b,#0e1015 76%);overflow:hidden}
.safe-frame{position:absolute;left:13%;right:13%;top:10%;bottom:10%;border:1px solid rgba(255,255,255,.18)}
.subject{position:absolute;left:38%;top:24%;width:210px;height:260px;background:linear-gradient(180deg,#42516c,#151922);border-radius:20px 20px 8px 8px;box-shadow:0 30px 70px rgba(0,0,0,.55)}
.playhead{position:absolute;top:0;bottom:0;left:348px;width:2px;background:var(--dcs-danger);box-shadow:0 0 8px rgba(239,107,107,.6)}
.timeline{height:258px;flex:0 0 258px;background:var(--dcs-bg);border-top:1px solid var(--dcs-line);display:flex;flex-direction:column}
.ruler{height:28px;position:relative;border-bottom:1px solid var(--dcs-line);font-family:var(--dcs-font-mono);font-size:10px;color:var(--dcs-text-mute)}
.ruler span{position:absolute;top:7px}
.tracks{position:relative;flex:1;overflow:hidden;background:linear-gradient(rgba(255,255,255,.035) 1px,transparent 1px);background-size:100% 44px}
.track-labels{position:absolute;left:0;top:0;bottom:0;width:92px;background:var(--dcs-surface-1);border-right:1px solid var(--dcs-line)}
.track-label{height:44px;display:flex;align-items:center;padding-left:10px;color:var(--dcs-text-mute);font-size:var(--dcs-fs-xs);text-transform:uppercase;letter-spacing:.08em}
.clip{position:absolute;height:30px;top:7px;border-radius:var(--dcs-r-2);padding:6px 8px;color:#fff;font-weight:500;overflow:hidden;white-space:nowrap;border:1px solid rgba(255,255,255,.18)}
.clip-selected{box-shadow:0 0 0 2px var(--dcs-accent),0 8px 20px rgba(0,0,0,.35)}
.clip-blue{background:#2f86ee}.clip-green{background:#2bb872}.clip-violet{background:#6f4eea}.clip-orange{background:#e07024}
.lane-1{transform:translateY(44px)}.lane-2{transform:translateY(88px)}.lane-3{transform:translateY(132px)}
.thumb{height:54px;background:var(--dcs-well);border:1px solid var(--dcs-line-soft);border-radius:var(--dcs-r-2);margin:8px;display:flex;align-items:center;justify-content:center;color:var(--dcs-text-mute)}
</style></head>
<body class="dcs" data-dcs-density="compact" data-dcs-style="3d">
<div class="app">
  <div class="dcs-menubar"><div class="dcs-menubar__brand">)HTML" << dcs::icon("filmstrip") << R"HTML( Decius Cut</div><button class="dcs-menubar__item">File</button><button class="dcs-menubar__item">Timeline</button><button class="dcs-menubar__item">Color</button><div class="dcs-menubar__spacer"></div><div class="dcs-menubar__meta">00:01:42:18</div></div>
  <div class="top">
    <aside class="browser dcs-dockpane"><div class="dcs-dockpane__tabs"><button class="dcs-dockpane__tab" aria-selected="true">)HTML" << dcs::icon("folder-open") << R"HTML( Media</button></div><div class="dcs-dockpane__body">
      <div class="thumb">A001_C014</div><div class="thumb">B-roll harbor</div><div class="thumb">VO final</div><div class="thumb">Grade LUT</div>
    </div></aside>
    <main class="viewer dcs-dockpane"><div class="dcs-dockpane__tabs"><button class="dcs-dockpane__tab" aria-selected="true">)HTML" << dcs::icon("view-render") << R"HTML( Program</button><div class="dcs-dockpane__tools">)HTML" << dcs::button(s.playing ? "Pause" : "Play", s.playing ? "pause" : "play", "dcs-btn--primary", "play") << R"HTML(</div></div><div class="dcs-dockpane__body viewer-screen"><div class="safe-frame"></div><div class="subject"></div></div></main>
    <aside class="inspector dcs-dockpane"><div class="dcs-dockpane__tabs"><button class="dcs-dockpane__tab" aria-selected="true">)HTML" << dcs::icon("color-grade") << R"HTML( Grade</button></div><div class="dcs-dockpane__body" style="padding:12px">
      <div class="dcs-row"><span style="width:70px;color:var(--dcs-text-mute)">Exposure</span>)HTML" << dcs::slider(-2, 2, value(s, "exposure", .35f), true, true, "exposure") << R"HTML(</div>
      <div class="dcs-row"><span style="width:70px;color:var(--dcs-text-mute)">Contrast</span>)HTML" << dcs::slider(0, 1, value(s, "contrast", .58f), false, false, "contrast") << R"HTML(</div>
      <div style="display:flex;gap:18px;margin-top:18px">)HTML" << dcs::knob(0, 1, value(s, "lift", .52f), "Lift", false, 0, "lift") << dcs::knob(0, 1, value(s, "gamma", .67f), "Gamma", false, 0, "gamma") << dcs::knob(0, 1, value(s, "gain", .41f), "Gain", false, 0, "gain") << R"HTML(</div>
    </div></aside>
  </div>
  <div class="timeline">
    <div class="ruler"><span style="left:104px">00:00</span><span style="left:284px">00:10</span><span style="left:464px">00:20</span><span style="left:644px">00:30</span><span style="left:824px">00:40</span></div>
    <div class="tracks"><div class="track-labels"><div class="track-label">V2</div><div class="track-label">V1</div><div class="track-label">A1</div><div class="track-label">MIX</div></div>
      <div style="position:absolute;left:92px;right:0;top:0;bottom:0">)HTML"
      << clip(0, 20, 210, "Title pass", "violet", s.selected_clip == 0)
      << clip(1, 82, 310, "A001_C014 hero", "blue", s.selected_clip == 1)
      << clip(2, 410, 260, "Cutaway harbor", "green", s.selected_clip == 2)
      << "<div class=\"lane-1\">" << clip(3, 50, 260, "Dialogue comp", "orange", s.selected_clip == 3) << "</div>"
      << "<div class=\"lane-2\">" << clip(4, 70, 520, "Music stem", "green", s.selected_clip == 4) << "</div>"
      << R"HTML(<div class="playhead"></div></div>
    </div>
  </div>
</div></body></html>)HTML";
    return h.str();
}

}  // namespace

int main() {
    affineui::Ui ui;
    ui.set_clear_color(affineui::Color::rgb(0x14, 0x16, 0x1c));
    demo::install_resource_loader(ui);
    VideoState state;
    auto rerender = [&] { ui.html(render(state)); ui.mark_dirty(); };
    rerender();
    ui.on_click("#play", [&] { state.playing = !state.playing; rerender(); });
    ui.on_click("#clip-0", [&] { state.selected_clip = 0; rerender(); });
    ui.on_click("#clip-1", [&] { state.selected_clip = 1; rerender(); });
    ui.on_click("#clip-2", [&] { state.selected_clip = 2; rerender(); });
    ui.on_click("#clip-3", [&] { state.selected_clip = 3; rerender(); });
    ui.on_click("#clip-4", [&] { state.selected_clip = 4; rerender(); });
    dcs::install_value_interactions(ui, {
        [&](std::string_view id) { return value(state, id, 0.0f); },
        [&](std::string_view id, float v) { state.values[std::string(id)] = v; },
        rerender,
    });
    sapp_desc desc{};
    desc.width = 1440;
    desc.height = 900;
    desc.window_title = "AffineUI - Decius video editor";
    desc.high_dpi = true;
    desc.swap_interval = 0;
    desc.sample_count = 1;
    desc.logger.func = slog_func;
    affineui::sokol::wire(desc, ui, true);
    sapp_run(&desc);
    return 0;
}
